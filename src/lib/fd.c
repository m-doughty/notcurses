#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include "internal.h"
#ifdef USING_PIDFD
#error "USING_PIDFD was already defined; it should not be."
#endif
#ifdef __MINGW32__
#include <io.h>
#include <windows.h>
#else
#include <spawn.h>
#endif
#ifdef __APPLE__
#include <sys/stat.h>
#include <sys/sysctl.h>
#endif
#if (defined(__linux__))
#include <linux/wait.h>
#include <asm/unistd.h>
#include <linux/sched.h>
#define NCPOLLEVENTS (POLLIN | POLLRDHUP)
#if (defined(__NR_clone3) && defined(P_PIDFD) && defined(CLONE_CLEAR_SIGHAND))
#define USING_PIDFD
#endif
#else
#define NCPOLLEVENTS (POLLIN)
#endif

// release the memory and fd, but don't join the thread (since we might be
// getting called within the thread's context, on a callback).
// (fork) How a reader finished.
typedef enum {
  FDTHREAD_DONE,      // end of file, error, its process gone, or a callback's
                      //  nonzero return
  FDTHREAD_STOPPED,   // stopped from outside
  FDTHREAD_DESTROYED, // destroyed from a callback: the plane has been freed,
                      //  unless it is a subprocess's (see subproc_free())
  FDTHREAD_ABANDONED, // (Windows) given up on by stop_reader(): the reader
                      //  has freed the plane, and must touch nothing else
} fdthread_end;

static int
ncfdplane_destroy_inner(ncfdplane* n){
  int ret = close(n->fd);
#ifndef __MINGW32__
  close(n->wakepipe[0]);
  close(n->wakepipe[1]);
#endif
  free(n);
  return ret;
}

#ifndef __MINGW32__
// (fork) A reader is stopped cooperatively, as on Windows: a destroy from
// another thread sets the plane's stop flag and writes to its wake pipe,
// which is in every poll() here; the reader looks after every wakeup and
// after every callback, and once stopped delivers nothing more. Upstream
// used pthread_cancel(), which on macOS can be lost outright -- the reader
// slept on in poll() with the cancel pending, and the destroy hung in
// pthread_join() (about one run in fifty of SubprocDestroyCmdHung; a second
// pthread_cancel() freed it, and a bare create/cancel/join loop reproduces
// it) -- and which, landing at a cancellation point inside a callback,
// killed the callback halfway through, locks and all, where the destroy is
// documented to wait for it. Each wakeup reads once, so a descriptor in
// blocking mode can never strand the reader in read(), out of the wake
// pipe's reach.
static int
make_wakepipe(int pipes[static 2]){
#ifdef __linux__
  return pipe2(pipes, O_CLOEXEC | O_NONBLOCK);
#else
  if(pipe(pipes)){
    return -1;
  }
  for(int i = 0 ; i < 2 ; ++i){
    if(set_fd_cloexec(pipes[i], 1, NULL) || set_fd_nonblocking(pipes[i], 1, NULL)){
      close(pipes[0]);
      close(pipes[1]);
      return -1;
    }
  }
  return 0;
#endif
}

// Ask the reader to stop, and wake it if it is in poll().
static void
ask_reader_to_stop(ncfdplane* n){
  fdreader_stop(&n->reader);
  const char c = 0;
  // the pipe is nonblocking: should it somehow be full, the reader has a
  // wakeup pending already
  if(write(n->wakepipe[1], &c, 1) < 0 && errno != EAGAIN && errno != EWOULDBLOCK){
    logerror("couldn't wake fdplane reader (%s)", strerror(errno));
  }
}

// If pidfd is < 0, it won't be used in the poll(). Once the subprocess it
// watches has gone, whatever it left in the pipe is still delivered. A
// subprocess's plane (|subproc|) destroyed from a callback is left for the
// subprocess to free (subproc_free()): its other thread may be using it.
static fdthread_end
fdthread(ncfdplane* ncfp, int pidfd, bool subproc){
  struct pollfd pfds[3];
  memset(pfds, 0, sizeof(pfds));
  pfds[0].fd = ncfp->fd;
  pfds[0].events = NCPOLLEVENTS;
  pfds[1].fd = ncfp->wakepipe[0];
  pfds[1].events = POLLIN;
  const int fdcount = pidfd < 0 ? 2 : 3;
  if(pidfd >= 0){
    pfds[2].fd = pidfd;
    pfds[2].events = NCPOLLEVENTS;
  }
  char* buf = malloc(BUFSIZ + 1);
  ssize_t r = 0;
  int err = 0;
  bool exited = false; // the subprocess has gone; drain, then leave
  if(buf == NULL){
    r = -1;
    err = ENOMEM;
  }
  while(buf && !fdreader_stopping(&ncfp->reader)){
    // draining after an exit: only the pipe, and only what is there now
    if(poll(pfds, exited ? 1 : fdcount, exited ? 0 : -1) < 0){
      if(errno == EINTR){
        continue;
      }
      r = -1;
      err = errno;
      break;
    }
    if(fdreader_stopping(&ncfp->reader)){
      break;
    }
    if(!exited && fdcount > 2 && pfds[2].revents){
      exited = true;
    }
    if(pfds[0].revents == 0){
      if(exited){
        r = 0;
        break;
      }
      continue;
    }
    r = read(ncfp->fd, buf, BUFSIZ);
    if(r < 0){
      if(errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK){
        continue;
      }
      err = errno;
      break;
    }
    if(r == 0){
      // if we're not doing follow, break out on a zero-byte read
      if(exited || !ncfp->follow){
        break;
      }
      continue;
    }
    buf[r] = '\0';
    if( (r = ncfp->cb(ncfp, buf, r, ncfp->curry)) ){
      err = errno;
      break;
    }
    if(ncfp->destroyed){
      break;
    }
  }
  free(buf);
  // stopped from outside: report nothing
  if(!ncfp->destroyed && fdreader_stopping(&ncfp->reader)){
    return FDTHREAD_STOPPED;
  }
  if(r <= 0 && !ncfp->destroyed){
    ncfp->donecb(ncfp, r == 0 ? 0 : err, ncfp->curry);
  }
  // destroyed from within a callback, the done callback included: free on
  // the way out
  if(ncfp->destroyed){
    if(!subproc){
      ncfdplane_destroy_inner(ncfp);
    }
    return FDTHREAD_DESTROYED;
  }
  return FDTHREAD_DONE;
}

static void *
ncfdplane_thread(void* vncfp){
  fdthread(vncfp, -1, false);
  return NULL;
}
#else
// (fork) Windows has no poll() for pipes, files or consoles -- WSAPoll()
// takes sockets and nothing else -- so the reader blocks in read() instead.
// A destroy from another thread asks it to stop and cancels that read (see
// stop_reader() and fdreader.h); the reader looks before every read and when
// every read returns, and once a stop has been asked for it delivers nothing
// more. At end of file a plane that does not follow is done; one that follows
// waits briefly and reads again (tailing a file, without the spin a poll()
// on a regular file amounts to), unless 'eof_ends': the read end of a
// subprocess's pipe, where end of file means every writer has gone and
// nothing more can arrive. A subprocess's reader reports nothing itself;
// its waiter reports the exit (see ncsubproc_waiter()).
static fdthread_end
fdthread(ncfdplane* ncfp, bool eof_ends){
  char* buf = malloc(BUFSIZ + 1);
  ssize_t r = 0;
  int err = 0;
  if(buf == NULL){
    r = -1;
    err = ENOMEM;
  }
  while(buf && fdreader_enter_read(&ncfp->reader)){
    r = read(ncfp->fd, buf, BUFSIZ);
    const int rerr = errno;
    const fdreader_after after = fdreader_leave_read(&ncfp->reader);
    if(after == FDREADER_OWNED){
      // the destroy gave up on this read and has returned (stop_reader()):
      // the plane is ours to free, and there is nobody left to tell
      loginfo("abandoned fdplane reader finished (%" PRIdPTR ")", (intptr_t)r);
      ncfdplane_destroy_inner(ncfp);
      free(buf);
      return FDTHREAD_ABANDONED;
    }
    if(after == FDREADER_STOP){
      break; // stopped from outside: whatever the read brought is dropped
    }
    if(r < 0){
      if(rerr == EINTR){
        continue;
      }
      err = rerr;
      break;
    }
    if(r == 0){
      if(eof_ends || !ncfp->follow){
        break;
      }
      Sleep(50);
      continue;
    }
    buf[r] = '\0';
    int cbr = ncfp->cb(ncfp, buf, r, ncfp->curry);
    if(cbr){
      r = cbr;
      err = errno;
      break;
    }
    if(ncfp->destroyed){
      break;
    }
  }
  // as on POSIX, a stop from outside reports nothing; a destroy from
  // within a callback frees on the way out -- but not a subprocess's plane
  // (eof_ends), which its waiter may be using: see subproc_free().
  free(buf);
  if(r <= 0 && !ncfp->destroyed && !fdreader_stopping(&ncfp->reader) && !eof_ends){
    ncfp->donecb(ncfp, r == 0 ? 0 : err, ncfp->curry);
  }
  if(ncfp->destroyed){
    if(!eof_ends){
      ncfdplane_destroy_inner(ncfp);
    }
    return FDTHREAD_DESTROYED;
  }
  return fdreader_stopping(&ncfp->reader) ? FDTHREAD_STOPPED : FDTHREAD_DONE;
}

static void *
ncfdplane_thread(void* vncfp){
  fdthread(vncfp, false);
  return NULL;
}

// (fork) Stop a reader thread from outside it, and join it. Asking is not
// enough when the reader is blocked in read(): winpthreads' read() is no
// cancellation point, and a pipe whose writer is alive and quiet never
// returns. So cancel the read, and keep cancelling until the thread has gone,
// since a cancel issued before the read began cancels nothing. Both cancels
// are aimed only while fdreader_reading() says the reader is in read(), so
// neither can break a callback's own I/O: CancelIoEx() on the plane's handle
// (whose only reader is ours: the plane owns the descriptor), which reaches
// console reads too, and CancelSynchronousIo() on the thread. The same
// shape as stop_inputlayer() in in.c.
//
// A read can still refuse to end: a device whose driver cannot cancel, say,
// or a reader waiting for the C runtime's lock on its descriptor rather than
// in the read itself. So after FDREADER_STOP_MS the destroy stops waiting
// instead of hanging: the thread is detached and abandoned together with the
// plane, the descriptor and its buffer, all of which the blocked read is
// still using, and it frees them itself if the read ever returns, delivering
// nothing (fdreader.h). Closing the descriptor here would block on that same
// runtime lock, and freeing the plane would pull it out from under the read.
// A reader in a callback is waited for however long it takes, as documented:
// the callback is the caller's code, and must not still be running once the
// destroy has returned.
//
// Answers 0 once the thread has been joined, 1 if it was abandoned (the
// caller must not touch the plane again), and -1 if the join failed.
static int
stop_reader(ncfdplane* n, void** res){
  fdreader_stop(&n->reader);
  const pthread_t tid = n->tid;
  HANDLE th = pthread_gethandle(tid);
  HANDLE fh = (HANDLE)_get_osfhandle(n->fd);
  const ULONGLONG start = GetTickCount64();
  bool warned = false;
  for(;;){
    if(fdreader_reading(&n->reader)){
      if(fh != INVALID_HANDLE_VALUE && (intptr_t)fh != -2){
        CancelIoEx(fh, NULL);
      }
      CancelSynchronousIo(th);
    }
    const DWORD w = WaitForSingleObject(th, 20);
    if(w == WAIT_OBJECT_0){
      break;
    }
    if(w != WAIT_TIMEOUT){
      // without a handle to wait on there is no timing the thread out; the
      // join can still see it go, as it did before there was a bound
      logerror("couldn't wait on fdplane thread (%lu); joining", GetLastError());
      break;
    }
    if(GetTickCount64() - start >= FDREADER_STOP_MS){
      if(fdreader_abandon(&n->reader)){
        // from here on the plane is the reader's: only the copied tid is used
        pthread_detach(tid);
        logerror("fdplane read didn't stop within %dms; abandoned its thread, "
                 "which frees the plane if the read ever returns",
                 FDREADER_STOP_MS);
        return 1;
      }
      if(!warned){
        logwarn("fdplane callback still running after %dms; waiting for it",
                FDREADER_STOP_MS);
        warned = true;
      }
    }
  }
  if(pthread_join(tid, res)){
    logerror("error joining fdplane thread");
    return -1;
  }
  return 0;
}
#endif

static ncfdplane*
ncfdplane_create_internal(ncplane* n, const ncfdplane_options* opts, int fd,
                          ncfdplane_callback cbfxn, ncfdplane_done_cb donecbfxn,
                          bool thread){
  if(opts->flags > 0){
    logwarn("provided unsupported flags %016" PRIx64, opts->flags);
  }
  ncfdplane* ret = malloc(sizeof(*ret));
  if(ret == NULL){
    return ret;
  }
  ret->cb = cbfxn;
  ret->donecb = donecbfxn;
  ret->follow = opts->follow;
  ret->ncp = n;
  ret->destroyed = false;
  fdreader_init(&ret->reader);
#ifndef __MINGW32__
  if(make_wakepipe(ret->wakepipe)){
    logerror("couldn't create fdplane wake pipe (%s)", strerror(errno));
    free(ret);
    return NULL;
  }
#endif
  ncplane_set_scrolling(ret->ncp, true);
  ret->fd = fd;
  ret->curry = opts->curry;
  if(thread){
    if(pthread_create(&ret->tid, NULL, ncfdplane_thread, ret)){
#ifndef __MINGW32__
      close(ret->wakepipe[0]);
      close(ret->wakepipe[1]);
#endif
      free(ret);
      return NULL;
    }
  }
  return ret;
}

ncfdplane* ncfdplane_create(ncplane* n, const ncfdplane_options* opts, int fd,
                            ncfdplane_callback cbfxn, ncfdplane_done_cb donecbfxn){
  ncfdplane_options zeroed = {0};
  if(!opts){
    opts = &zeroed;
  }
  if(fd < 0 || !cbfxn || !donecbfxn){
    return NULL;
  }
  return ncfdplane_create_internal(n, opts, fd, cbfxn, donecbfxn, true);
}

ncplane* ncfdplane_plane(ncfdplane* n){
  return n->ncp;
}

int ncfdplane_destroy(ncfdplane* n){
  int ret = 0;
  if(n){
    if(pthread_equal(pthread_self(), n->tid)){
      n->destroyed = true; // ncfdplane_destroy_inner() is called on thread exit
    }else{
      void* vret = NULL;
#ifndef __MINGW32__
      ask_reader_to_stop(n);
      if(pthread_join(n->tid, &vret)){
        logerror("error joining fdplane thread");
        ret = -1;
      }
#else
      const int stopped = stop_reader(n, &vret);
      if(stopped > 0){
        return -1; // abandoned: the reader frees the plane (stop_reader())
      }
      ret |= stopped;
#endif
      ret |= ncfdplane_destroy_inner(n);
    }
  }
  return ret;
}

// (fork) Everything an ncsubproc holds -- its plane (with the pipe's read
// end), the pipe's write end and the pidfd (POSIX), its process and reader
// thread handles (Windows), its lock -- once both its threads are done.
static void
subproc_free(ncsubproc* n){
  if(n->nfp){
    ncfdplane_destroy_inner(n->nfp);
  }
#ifndef __MINGW32__
  if(n->pipewfd >= 0){
    close(n->pipewfd);
  }
  if(n->pidfd >= 0){
    close(n->pidfd);
  }
#else
  if(n->hreader){
    CloseHandle(n->hreader);
  }
  if(n->hproc){
    CloseHandle(n->hproc);
  }
#endif
  pthread_mutex_destroy(&n->lock);
  free(n);
}

// (fork) Has |n| a waiter thread, besides its reader? (Not with a pidfd,
// which the reader polls.)
static inline bool
subproc_has_waiter(const ncsubproc* n){
#ifndef __MINGW32__
  return n->pidfd < 0;
#else
  (void)n;
  return true;
#endif
}

// (fork) One of |n|'s threads (the reader, or the waiter) is done with it.
// Normally ncsubproc_destroy() then joins it. After a destroy from within a
// callback nobody will, so the thread detaches itself, and the last one out
// frees everything. (A thread that finished before that destroy is detached
// by it: see subproc_destroy_from_callback().)
static void
subproc_thread_done(ncsubproc* n, bool reader){
  pthread_mutex_lock(&n->lock);
  if(reader){
    n->readerdone = true;
  }else{
    n->waiterdone = true;
  }
  const bool last = --n->live == 0;
  const bool self = n->selfdestroyed;
  pthread_mutex_unlock(&n->lock);
  if(self){
    pthread_detach(pthread_self());
    if(last){
      subproc_free(n);
    }
  }
}

// (fork) Is the caller one of |n|'s own threads -- inside one of its
// callbacks, that is, whose thread can't be joined from itself?
static bool
subproc_own_thread(const ncsubproc* n){
  if(pthread_equal(pthread_self(), n->nfp->tid)){
    return true;
  }
  return subproc_has_waiter(n) && pthread_equal(pthread_self(), n->waittid);
}

// (fork) ncsubproc_destroy() from within one of |n|'s callbacks, on one of
// its own threads (or the reader's plane destroyed from one, |implicit|).
// The man page has it reclaim everything as the thread exits; so end the
// process, ask the reader to stop, and leave the rest to the threads:
// subproc_thread_done() frees it all once the last has finished. No
// callback is invoked after this one returns.
static int
subproc_destroy_from_callback(ncsubproc* n, bool implicit){
  pthread_mutex_lock(&n->lock);
  if(n->selfdestroyed){
    pthread_mutex_unlock(&n->lock);
    if(!implicit){
      logerror("ncsubproc %p was already destroyed", n);
    }
    return -1;
  }
  n->selfdestroyed = true;
  // a thread already through subproc_thread_done() saw no self-destroy, and
  // was waiting to be joined: detach it instead
  if(n->readerdone){
    pthread_detach(n->nfp->tid);
  }
  if(subproc_has_waiter(n) && n->waiterdone){
    pthread_detach(n->waittid);
  }
#ifndef __MINGW32__
  if(n->pidfd >= 0){
#ifdef USING_PIDFD
    if(syscall(__NR_pidfd_send_signal, n->pidfd, SIGKILL, NULL, 0)){
      logwarn("couldn't signal pidfd %d (%s)", n->pidfd, strerror(errno));
    }
#endif
  }else if(!n->waited){
    loginfo("sending SIGKILL to PID %d", n->pid);
    kill(n->pid, SIGKILL);
  }
  pthread_mutex_unlock(&n->lock);
  ask_reader_to_stop(n->nfp);
#else
  if(!n->waited){
    loginfo("terminating process %d", (int)n->pid);
    TerminateProcess(n->hproc, 1);
  }
  pthread_mutex_unlock(&n->lock);
  // the reader can't be waited for from here: asked to stop, it leaves at
  // its next look, and a read it is blocked in is cancelled -- once, so a
  // read that doesn't cancel lasts until the process's output ends (all its
  // writers gone, which ending the process normally makes so)
  fdreader_stop(&n->nfp->reader);
  if(fdreader_reading(&n->nfp->reader)){
    HANDLE fh = (HANDLE)_get_osfhandle(n->nfp->fd);
    if(fh != INVALID_HANDLE_VALUE && (intptr_t)fh != -2){
      CancelIoEx(fh, NULL);
    }
    CancelSynchronousIo(n->hreader);
  }
#endif
  return 0;
}

#ifndef __MINGW32__
// get 2 pipes, and ensure they're both set to close-on-exec
static int
lay_pipes(int pipes[static 2]){
#ifdef __linux__
  if(pipe2(pipes, O_CLOEXEC)){ // can't use O_NBLOCK here (affects client)
#else
  if(pipe(pipes)){
#endif
    return -1;
  }
#ifndef __linux__
  if(set_fd_cloexec(pipes[0], 1, NULL) || set_fd_cloexec(pipes[1], 1, NULL)){
    close(pipes[0]);
    close(pipes[1]);
    return -1;
  }
#endif
  return 0;
}

// ncsubproc creates a pipe, retaining the read end. it clone()s a subprocess,
// getting a pidfd. the subprocess dup2()s the write end of the pipe onto file
// descriptors 1 and 2, exec()s, and begins running. the parent creates an
// ncfdplane around the read end, involving creation of a new thread. the
// parent then returns. (fork) The parent's copy of the write end goes back
// in *pipewfd: holding it open, as the parent always has, the reader never
// sees end of file (a subprocess's plane follows), so it is kept until
// ncsubproc_destroy() closes it, rather than leaked as it used to be. On
// failure nothing is left open.
static pid_t
launch_pipe_process(int* pipefd, int* pipewfd, int* pidfd, unsigned usepath,
                    const char* bin,  char* const arg[], char* const env[]){
  *pidfd = -1;
  *pipewfd = -1;
  int pipes[2];
  if(lay_pipes(pipes)){
    return -1;
  }
  pid_t p = -1;
#ifdef USING_PIDFD
  // on linux, we try to use the brand-new pidfd capability via clone3(). if
  // that fails, fall through to posix_spawn(), our only option on freebsd.
  // FIXME clone3 is not yet supported on debian sparc64/alpha as of 2020-07
  struct clone_args clargs;
  memset(&clargs, 0, sizeof(clargs));
  clargs.pidfd = (uintptr_t)pidfd;
  clargs.flags = CLONE_CLEAR_SIGHAND | CLONE_FS | CLONE_PIDFD;
  clargs.exit_signal = SIGCHLD;
  p = syscall(__NR_clone3, &clargs, sizeof(clargs));
  if(p == 0){ // child
    if(dup2(pipes[1], STDOUT_FILENO) < 0 || dup2(pipes[1], STDERR_FILENO) < 0){
      logerror("couldn't dup() %d (%s)", pipes[1], strerror(errno));
      exit(EXIT_FAILURE);
    }
    if(env){
      execvpe(bin, arg, env);
    }else if(usepath){
      execvp(bin, arg);
    }else{
      execv(bin, arg);
    }
    exit(EXIT_FAILURE);
  }else if(p < 0){
    logwarn("clone3() failed (%s), using posix_spawn()", strerror(errno));
  }
#endif
  if(p < 0){
    posix_spawn_file_actions_t factions;
    if(posix_spawn_file_actions_init(&factions)){
      logerror("couldn't initialize spawn file actions");
      return -1;
    }
    posix_spawn_file_actions_adddup2(&factions, pipes[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&factions, pipes[1], STDERR_FILENO);
    int r;
    if(usepath){
      r = posix_spawnp(&p, bin, &factions, NULL, arg, env);
    }else{
      r = posix_spawn(&p, bin, &factions, NULL, arg, env);
    }
    if(r){
      logerror("posix_spawn %s failed (%s)", bin, strerror(errno));
    }
    posix_spawn_file_actions_destroy(&factions);
  }
  if(p > 0){ // parent
    *pipefd = pipes[0];
    *pipewfd = pipes[1];
    set_fd_nonblocking(*pipefd, 1, NULL);
  }else{
    close(pipes[0]);
    close(pipes[1]);
    if(*pidfd >= 0){
      close(*pidfd);
      *pidfd = -1;
    }
  }
  return p;
}

// nuke the just-spawned process, and reap it. called before the subprocess
// reader thread is launched (which otherwise reaps the subprocess).
static int
kill_and_wait_subproc(pid_t pid, int pidfd, int* status){
  int ret = -1;
  // on linux, we try pidfd_send_signal, if the pidfd has been defined.
  // otherwise, we fall back to regular old kill();
  if(pidfd >= 0){
#ifdef USING_PIDFD
    ret = syscall(__NR_pidfd_send_signal, pidfd, SIGKILL, NULL, 0);
    siginfo_t info;
    memset(&info, 0, sizeof(info));
    waitid(P_PIDFD, pidfd, &info, 0);
#endif
  }
  if(ret < 0){
    kill(pid, SIGKILL);
  }
  // process ought be available immediately following waitid(), so supply
  // WNOHANG to avoid possible lockups due to weirdness
  if(pid != waitpid(pid, status, WNOHANG)){
    return -1;
  }
  return 0;
}

// need a poll on both main fd and pidfd
static void *
ncsubproc_thread(void* vncsp){
  ncsubproc* ncsp = vncsp;
  const fdthread_end end = fdthread(ncsp->nfp, ncsp->pidfd, true);
  if(end == FDTHREAD_DESTROYED){
    // (fork) its plane destroyed from a callback (ncfdplane_destroy()): the
    // subprocess goes with it, as if from ncsubproc_destroy() there
    subproc_destroy_from_callback(ncsp, true);
  }
  pthread_mutex_lock(&ncsp->lock);
  const bool self = ncsp->selfdestroyed;
  pthread_mutex_unlock(&ncsp->lock);
  int* status = NULL;
  if(self){
    // (fork) nobody will join us; reap the process here if no waiter will
    if(ncsp->pidfd >= 0){
      kill_and_wait_subproc(ncsp->pid, ncsp->pidfd, NULL);
    }
  }else if(end != FDTHREAD_STOPPED){
    // (fork) stopped by ncsubproc_destroy(), which ends the process itself,
    // and whose waiter reaps it, nothing more is ours to do: killing it here
    // could hit whatever unrelated process had since been given its PID.
    int st = -1;
    if(kill_and_wait_subproc(ncsp->pid, ncsp->pidfd, &st)){
      st = -1;
    }
    if( (status = malloc(sizeof(*status))) ){
      *status = st;
    }
  }
  subproc_thread_done(ncsp, true); // ncsp may be gone after this
  return status;
}

// this is only used if we don't have a pidfd available for poll()ing. in that
// case, we want to perform a blocking waitpid() on the pid, calling the
// completion callback when it exits (since the process exit won't necessarily
// wake up our poll()ing thread).
static void *
ncsubproc_waiter(void* vncsp){
  ncsubproc* ncsp = vncsp;
  int st = 0;
  pid_t pid;
  while((pid = waitpid(ncsp->pid, &st, 0)) < 0 && errno == EINTR){
    ;
  }
  const bool reaped = pid == ncsp->pid;
  pthread_mutex_lock(&ncsp->lock);
  if(reaped){
    ncsp->waited = true;
  }
  bool self = ncsp->selfdestroyed;
  pthread_mutex_unlock(&ncsp->lock);
  if(reaped && !self && !ncsp->nfp->destroyed){
    ncsp->nfp->donecb(ncsp->nfp, st, ncsp->nfp->curry);
    // (fork) which may have destroyed it
    pthread_mutex_lock(&ncsp->lock);
    self = ncsp->selfdestroyed;
    pthread_mutex_unlock(&ncsp->lock);
  }
  int* status = NULL;
  if(reaped && !self && (status = malloc(sizeof(*status)))){
    *status = st;
  }
  subproc_thread_done(ncsp, false); // ncsp may be gone after this
  return status;
}

static ncfdplane*
ncsubproc_launch(ncplane* n, ncsubproc* ret, const ncsubproc_options* opts, int fd,
                 ncfdplane_callback cbfxn, ncfdplane_done_cb donecbfxn){
  ncfdplane_options popts = {
    .curry = opts->curry,
    .follow = true,
  };
  ret->nfp = ncfdplane_create_internal(n, &popts, fd, cbfxn, donecbfxn, false);
  if(ret->nfp == NULL){
    close(fd); // (fork) the plane would have owned it
    return NULL;
  }
  ret->live = 1; // (fork) the reader; see subproc_thread_done()
  if(pthread_create(&ret->nfp->tid, NULL, ncsubproc_thread, ret)){
    ncfdplane_destroy_inner(ret->nfp);
    ret->nfp = NULL;
    return NULL; // (fork) not on to a waiter that would use the plane
  }
  if(ret->pidfd < 0){
    // if we don't have a pidfd to throw into our poll(), we need spin up a
    // thread to call waitpid() on our pid
    pthread_mutex_lock(&ret->lock);
    ++ret->live; // (fork) and the waiter
    pthread_mutex_unlock(&ret->lock);
    if(pthread_create(&ret->waittid, NULL, ncsubproc_waiter, ret)){
      // (fork) stop and join the reader before freeing the plane it reads
      ask_reader_to_stop(ret->nfp);
      pthread_join(ret->nfp->tid, NULL);
      ncfdplane_destroy_inner(ret->nfp);
      ret->nfp = NULL;
    }
  }
  return ret->nfp;
}
#else
// (fork) ncsubproc on Windows: CreateProcessW() with the child's stdout and
// stderr on one inheritable pipe, and its stdin inherited, as on POSIX. The
// read end becomes a C file descriptor for an ncfdplane, read by its own
// thread until the last writer is gone; a second thread waits on the process
// and reports its exit code through the done callback -- the shape POSIX
// takes when it has no pidfd (macOS, the BSDs).

// The UTF-8 string 's' as UTF-16, or NULL if it is not valid UTF-8.
static wchar_t*
widen(const char* s){
  int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, NULL, 0);
  if(n <= 0){
    return NULL;
  }
  wchar_t* w = malloc(sizeof(*w) * n);
  if(w && MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, w, n) != n){
    free(w);
    w = NULL;
  }
  return w;
}

// arg[] as one command line, each argument quoted so that the C runtime's
// argv parsing (and CommandLineToArgvW()) gives it back unchanged: an
// argument with no space, tab, newline, vertical tab or quote goes in as it
// is; any other is quoted, its quotes escaped, and backslashes doubled where
// they precede a quote (including the closing one). The rules are ASCII, so
// they are applied to the UTF-8 before it is widened. NULL on failure.
static char*
quote_command_line(const char* bin, char* const arg[]){
  const char* only[] = { bin, NULL };
  const char* const* args = (arg && arg[0]) ? (const char* const*)arg : only;
  size_t cap = 1;
  for(size_t i = 0 ; args[i] ; ++i){
    cap += 2 * strlen(args[i]) + 3;
  }
  char* out = malloc(cap);
  if(out == NULL){
    return NULL;
  }
  size_t o = 0;
  for(size_t i = 0 ; args[i] ; ++i){
    const char* a = args[i];
    if(i){
      out[o++] = ' ';
    }
    if(*a && !strpbrk(a, " \t\n\v\"")){
      memcpy(out + o, a, strlen(a));
      o += strlen(a);
      continue;
    }
    out[o++] = '"';
    for(const char* p = a ; ; ++p){
      size_t slashes = 0;
      while(*p == '\\'){
        ++slashes;
        ++p;
      }
      if(*p == '\0'){
        memset(out + o, '\\', slashes * 2);
        o += slashes * 2;
        break;
      }
      if(*p == '"'){
        memset(out + o, '\\', slashes * 2 + 1);
        o += slashes * 2 + 1;
      }else{
        memset(out + o, '\\', slashes);
        o += slashes;
      }
      out[o++] = *p;
    }
    out[o++] = '"';
  }
  out[o] = '\0';
  return out;
}

// The length of the name in a "NAME=value" environment string. A name may
// itself begin with '=' (the per-drive "=C:" entries), so the search for the
// '=' that ends it starts one character in.
static size_t
env_name_len(const wchar_t* s){
  const wchar_t* eq = *s ? wcschr(s + 1, L'=') : NULL;
  return eq ? (size_t)(eq - s) : wcslen(s);
}

static int
env_compare(const void* va, const void* vb){
  const wchar_t* a = *(const wchar_t* const*)va;
  const wchar_t* b = *(const wchar_t* const*)vb;
  // CSTR_LESS_THAN (1), CSTR_EQUAL (2), CSTR_GREATER_THAN (3)
  return CompareStringOrdinal(a, (int)env_name_len(a), b, (int)env_name_len(b), TRUE) - 2;
}

// env[] as a CreateProcessW() environment block: UTF-16 strings, each
// NUL-terminated, sorted by name without regard to case as the
// documentation requires, and a final NUL. NULL on failure.
static wchar_t*
environment_block(char* const env[]){
  size_t count = 0;
  while(env[count]){
    ++count;
  }
  wchar_t** wide = calloc(count + 1, sizeof(*wide));
  if(wide == NULL){
    return NULL;
  }
  wchar_t* block = NULL;
  size_t total = 1;
  for(size_t i = 0 ; i < count ; ++i){
    if((wide[i] = widen(env[i])) == NULL){
      goto done;
    }
    total += wcslen(wide[i]) + 1;
  }
  qsort(wide, count, sizeof(*wide), env_compare);
  if((block = malloc(sizeof(*block) * total)) == NULL){
    goto done;
  }
  wchar_t* w = block;
  for(size_t i = 0 ; i < count ; ++i){
    const size_t len = wcslen(wide[i]) + 1;
    memcpy(w, wide[i], sizeof(*w) * len);
    w += len;
  }
  *w = L'\0';
done:
  for(size_t i = 0 ; i < count ; ++i){
    free(wide[i]);
  }
  free(wide);
  return block;
}

// Start the process. On success fills in ret->hproc and ret->pid, puts the
// read end of its output pipe in *pipefd, and answers 0.
static int
launch_windows_process(ncsubproc* ret, int* pipefd, unsigned usepath,
                       const char* bin, char* const arg[], char* const env[]){
  int r = -1;
  char* cmd = NULL;
  wchar_t* wbin = NULL;
  wchar_t* wapp = NULL;
  wchar_t* wcmd = NULL;
  wchar_t* wenv = NULL;
  HANDLE rd = NULL;
  HANDLE wr = NULL;
  HANDLE in = NULL;
  LPPROC_THREAD_ATTRIBUTE_LIST attrs = NULL;
  bool attrs_ready = false;
  if((wbin = widen(bin)) == NULL){
    logerror("%s is not valid UTF-8", bin);
    goto done;
  }
  if(usepath){
    // Look it up as CreateProcessW() would (adding .exe), but here, so the
    // command line can carry arg[0] as given rather than the program's path.
    DWORD need = SearchPathW(NULL, wbin, L".exe", 0, NULL, NULL);
    if(need == 0 || (wapp = malloc(sizeof(*wapp) * need)) == NULL){
      logerror("couldn't find %s on the search path", bin);
      goto done;
    }
    DWORD got = SearchPathW(NULL, wbin, L".exe", need, wapp, NULL);
    if(got == 0 || got >= need){
      logerror("couldn't find %s on the search path", bin);
      goto done;
    }
  }else{
    wapp = wbin;
    wbin = NULL;
  }
  if((cmd = quote_command_line(bin, arg)) == NULL || (wcmd = widen(cmd)) == NULL){
    logerror("couldn't build a command line for %s", bin);
    goto done;
  }
  if(env && (wenv = environment_block(env)) == NULL){
    logerror("couldn't build an environment for %s", bin);
    goto done;
  }
  SECURITY_ATTRIBUTES sa = { .nLength = sizeof(sa), .lpSecurityDescriptor = NULL,
                             .bInheritHandle = TRUE, };
  if(!CreatePipe(&rd, &wr, &sa, 0) || !SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0)){
    logerror("couldn't create a pipe for %s (%lu)", bin, GetLastError());
    goto done;
  }
  // stdin is inherited, as on POSIX: a duplicate, so that it can be made
  // inheritable without changing the parent's own handle.
  HANDLE pin = GetStdHandle(STD_INPUT_HANDLE);
  if(pin && pin != INVALID_HANDLE_VALUE){
    if(!DuplicateHandle(GetCurrentProcess(), pin, GetCurrentProcess(), &in,
                        0, TRUE, DUPLICATE_SAME_ACCESS)){
      in = NULL;
    }
  }
  // Only these handles reach the child. bInheritHandles alone would hand it
  // every inheritable handle this process holds (another child's pipe, say,
  // which would then never see end of file).
  HANDLE inherit[2];
  DWORD ninherit = 0;
  inherit[ninherit++] = wr;
  if(in){
    inherit[ninherit++] = in;
  }
  SIZE_T asize = 0;
  InitializeProcThreadAttributeList(NULL, 1, 0, &asize);
  if((attrs = malloc(asize)) == NULL || !InitializeProcThreadAttributeList(attrs, 1, 0, &asize)){
    logerror("couldn't prepare to launch %s (%lu)", bin, GetLastError());
    goto done;
  }
  attrs_ready = true;
  if(!UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                inherit, sizeof(*inherit) * ninherit, NULL, NULL)){
    logerror("couldn't restrict the handles %s inherits (%lu)", bin, GetLastError());
    goto done;
  }
  STARTUPINFOEXW si;
  memset(&si, 0, sizeof(si));
  si.StartupInfo.cb = sizeof(si);
  si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
  si.StartupInfo.hStdInput = in;
  si.StartupInfo.hStdOutput = wr;
  si.StartupInfo.hStdError = wr;
  si.lpAttributeList = attrs;
  PROCESS_INFORMATION pi;
  memset(&pi, 0, sizeof(pi));
  const DWORD flags = EXTENDED_STARTUPINFO_PRESENT | (wenv ? CREATE_UNICODE_ENVIRONMENT : 0);
  if(!CreateProcessW(wapp, wcmd, NULL, NULL, TRUE, flags, wenv, NULL,
                     &si.StartupInfo, &pi)){
    logerror("couldn't launch %s (%lu)", bin, GetLastError());
    goto done;
  }
  CloseHandle(pi.hThread);
  // The child has its own copy of the write end. This one has to go, or end
  // of file never comes.
  CloseHandle(wr);
  wr = NULL;
  int fd = _open_osfhandle((intptr_t)rd, _O_RDONLY | _O_BINARY);
  if(fd < 0){
    logerror("couldn't wrap the pipe from %s", bin);
    TerminateProcess(pi.hProcess, 1);
    WaitForSingleObject(pi.hProcess, INFINITE);
    CloseHandle(pi.hProcess);
    goto done;
  }
  rd = NULL; // owned by fd now
  ret->hproc = pi.hProcess;
  ret->pid = (pid_t)pi.dwProcessId;
  *pipefd = fd;
  r = 0;
done:
  if(attrs_ready){
    DeleteProcThreadAttributeList(attrs);
  }
  free(attrs);
  if(in){
    CloseHandle(in);
  }
  if(wr){
    CloseHandle(wr);
  }
  if(rd){
    CloseHandle(rd);
  }
  free(wbin);
  free(wapp);
  free(wcmd);
  free(wenv);
  free(cmd);
  return r;
}

// The subprocess's output, until its pipe closes. Reports nothing: the
// waiter does.
static void *
ncsubproc_thread(void* vncsp){
  ncsubproc* ncsp = vncsp;
  const fdthread_end end = fdthread(ncsp->nfp, true);
  if(end == FDTHREAD_ABANDONED){
    return NULL; // (fork) ncsubproc_destroy() has been and gone, and ncsp too
  }
  if(end == FDTHREAD_DESTROYED){
    // (fork) its plane destroyed from a callback (ncfdplane_destroy()): the
    // subprocess goes with it, as if from ncsubproc_destroy() there
    subproc_destroy_from_callback(ncsp, true);
  }
  subproc_thread_done(ncsp, true); // ncsp may be gone after this
  return NULL;
}

// How long the waiter gives the reader, once the process has ended, to
// deliver what it wrote: the reader's end of file comes when the last writer
// goes, which is at once unless a process it started holds the pipe open.
#define NCSUBPROC_DRAIN_MS 1000

// Wait for the process to end, let the reader drain its output, and report
// the exit code through the done callback, unless the plane was destroyed
// from within a callback. Draining first means "done" follows the last of
// the output, so a destroy from the done callback loses none of it. Answers
// the exit code, for ncsubproc_destroy(); -1 if it could not be had.
static void *
ncsubproc_waiter(void* vncsp){
  ncsubproc* ncsp = vncsp;
  int* status = malloc(sizeof(*status));
  if(status == NULL){
    return NULL;
  }
  DWORD code = 0;
  if(WaitForSingleObject(ncsp->hproc, INFINITE) != WAIT_OBJECT_0
     || !GetExitCodeProcess(ncsp->hproc, &code)){
    *status = -1;
  }else{
    *status = (int)code;
  }
  // (fork) on our own handle to the reader: after a destroy from within a
  // callback the reader detaches itself, and its pthread_t can go any time
  WaitForSingleObject(ncsp->hreader, NCSUBPROC_DRAIN_MS);
  pthread_mutex_lock(&ncsp->lock);
  ncsp->waited = true;
  bool self = ncsp->selfdestroyed;
  pthread_mutex_unlock(&ncsp->lock);
  if(!self && !ncsp->nfp->destroyed){
    ncsp->nfp->donecb(ncsp->nfp, *status, ncsp->nfp->curry);
    // (fork) which may have destroyed it
    pthread_mutex_lock(&ncsp->lock);
    self = ncsp->selfdestroyed;
    pthread_mutex_unlock(&ncsp->lock);
  }
  if(self){
    free(status);
    status = NULL;
  }
  subproc_thread_done(ncsp, false); // ncsp may be gone after this
  return status;
}

// The reader and the waiter. On failure everything this made is gone, the fd
// included; the process is the caller's to end.
static ncfdplane*
ncsubproc_launch(ncplane* n, ncsubproc* ret, const ncsubproc_options* opts, int fd,
                 ncfdplane_callback cbfxn, ncfdplane_done_cb donecbfxn){
  ncfdplane_options popts = {
    .curry = opts->curry,
    .follow = true,
  };
  ret->nfp = ncfdplane_create_internal(n, &popts, fd, cbfxn, donecbfxn, false);
  if(ret->nfp == NULL){
    close(fd);
    return NULL;
  }
  ret->live = 2; // (fork) the reader and the waiter: subproc_thread_done()
  if(pthread_create(&ret->nfp->tid, NULL, ncsubproc_thread, ret)){
    ncfdplane_destroy_inner(ret->nfp);
    ret->nfp = NULL;
    return NULL;
  }
  // (fork) taken before the reader can possibly be detached (no callback
  // can destroy the subprocess before this returns it), and kept until
  // subproc_free(), for the waiter's drain wait
  if(!DuplicateHandle(GetCurrentProcess(), pthread_gethandle(ret->nfp->tid),
                      GetCurrentProcess(), (HANDLE*)&ret->hreader, 0, FALSE,
                      DUPLICATE_SAME_ACCESS)){
    logerror("couldn't duplicate the reader's handle (%lu)", GetLastError());
    ret->hreader = NULL;
    TerminateProcess(ret->hproc, 1);
    if(stop_reader(ret->nfp, NULL) <= 0){ // else abandoned: the reader frees it
      ncfdplane_destroy_inner(ret->nfp);
    }
    ret->nfp = NULL;
    return NULL;
  }
  if(pthread_create(&ret->waittid, NULL, ncsubproc_waiter, ret)){
    TerminateProcess(ret->hproc, 1);
    if(stop_reader(ret->nfp, NULL) <= 0){ // else abandoned: the reader frees it
      ncfdplane_destroy_inner(ret->nfp);
    }
    CloseHandle(ret->hreader);
    ret->hreader = NULL;
    ret->nfp = NULL;
    return NULL;
  }
  return ret->nfp;
}
#endif

// use of env implies usepath
static ncsubproc*
ncexecvpe(ncplane* n, const ncsubproc_options* opts, unsigned usepath,
          const char* bin,  char* const arg[], char* const env[],
          ncfdplane_callback cbfxn, ncfdplane_done_cb donecbfxn){
  ncsubproc_options zeroed = {0};
  if(!opts){
    opts = &zeroed;
  }
  if(!cbfxn || !donecbfxn){
    return NULL;
  }
  if(opts->flags > 0){
    logwarn("provided unsupported flags %016" PRIx64, opts->flags);
  }
  int fd = -1;
  ncsubproc* ret = malloc(sizeof(*ret));
  if(ret == NULL){
    return NULL;
  }
  memset(ret, 0, sizeof(*ret));
  // (fork) A zeroed mutex is a valid one only where PTHREAD_MUTEX_INITIALIZER
  // happens to be all zeroes (glibc); on macOS and winpthreads it is not.
  if(pthread_mutex_init(&ret->lock, NULL)){
    free(ret);
    return NULL;
  }
#ifndef __MINGW32__
  ret->pid = launch_pipe_process(&fd, &ret->pipewfd, &ret->pidfd, usepath,
                                 bin, arg, env);
  if(ret->pid < 0){
    pthread_mutex_destroy(&ret->lock);
    free(ret);
    return NULL;
  }
  if((ret->nfp = ncsubproc_launch(n, ret, opts, fd, cbfxn, donecbfxn)) == NULL){
    kill_and_wait_subproc(ret->pid, ret->pidfd, NULL);
    close(ret->pipewfd); // (fork) ncsubproc_launch() saw to the read end
    if(ret->pidfd >= 0){
      close(ret->pidfd);
    }
    pthread_mutex_destroy(&ret->lock);
    free(ret);
    return NULL;
  }
  return ret;
#else
  ret->pidfd = -1;
  if(launch_windows_process(ret, &fd, usepath, bin, arg, env)){
    pthread_mutex_destroy(&ret->lock);
    free(ret);
    return NULL;
  }
  if((ret->nfp = ncsubproc_launch(n, ret, opts, fd, cbfxn, donecbfxn)) == NULL){
    TerminateProcess(ret->hproc, 1);
    WaitForSingleObject(ret->hproc, INFINITE);
    CloseHandle(ret->hproc);
    pthread_mutex_destroy(&ret->lock);
    free(ret);
    return NULL;
  }
  return ret;
#endif
}

ncsubproc* ncsubproc_createv(ncplane* n, const ncsubproc_options* opts,
                             const char* bin, const char* const arg[],
                             ncfdplane_callback cbfxn, ncfdplane_done_cb donecbfxn){
  return ncexecvpe(n, opts, 0, bin, (char* const *)arg, NULL, cbfxn, donecbfxn);
}

ncsubproc* ncsubproc_createvp(ncplane* n, const ncsubproc_options* opts,
                              const char* bin, const char* const arg[],
                              ncfdplane_callback cbfxn, ncfdplane_done_cb donecbfxn){
  return ncexecvpe(n, opts, 1, bin, (char* const *)arg, NULL, cbfxn, donecbfxn);
}

ncsubproc* ncsubproc_createvpe(ncplane* n, const ncsubproc_options* opts,
                       const char* bin, const char* const arg[],
                       const char* const env[],
                       ncfdplane_callback cbfxn, ncfdplane_done_cb donecbfxn){
  return ncexecvpe(n, opts, 1, bin, (char* const *)arg, (char* const*)env, cbfxn, donecbfxn);
}

int ncsubproc_destroy(ncsubproc* n){
  int ret = 0;
  if(n){
    if(subproc_own_thread(n)){
      // (fork) from within a callback: its thread can't join itself
      return subproc_destroy_from_callback(n, false);
    }
    void* vret = NULL;
#ifdef __MINGW32__
    int stopped = 0;
#endif
//fprintf(stderr, "pid: %u pidfd: %d waittid: %u\n", n->pid, n->pidfd, n->waittid);
#ifndef __MINGW32__
#ifdef USING_PIDFD
    if(n->pidfd >= 0){
      loginfo("sending SIGKILL to pidfd %d", n->pidfd);
      if(syscall(__NR_pidfd_send_signal, n->pidfd, SIGKILL, NULL, 0)){
        kill(n->pid, SIGKILL);
      }
    }else // (fork) no pidfd after all (posix_spawn()): as without them
#endif
    {
      pthread_mutex_lock(&n->lock);
      if(!n->waited){
        loginfo("sending SIGKILL to PID %d", n->pid);
        kill(n->pid, SIGKILL);
      }
      pthread_mutex_unlock(&n->lock);
    }
    // the thread waits on the subprocess via pidfd (iff pidfd >= 0), and
    // then exits. don't try to stop the thread in that case; rely instead on
    // killing the subprocess.
    if(n->pidfd < 0){
      ask_reader_to_stop(n->nfp);
      // shouldn't need a cancellation of waittid thanks to SIGKILL
      pthread_join(n->waittid, &vret);
    }
    if(vret == NULL){
      pthread_join(n->nfp->tid, &vret);
    }else{
      pthread_join(n->nfp->tid, NULL);
    }
#else
    // (fork) End it if it is still running -- the answer is then not
    // success, as SIGKILL's is not on POSIX -- and let the waiter collect
    // the exit code. Then stop the reader rather than wait for its end of
    // file: a grandchild can hold the pipe open after the child has gone.
    pthread_mutex_lock(&n->lock);
    if(!n->waited){
      loginfo("terminating process %d", (int)n->pid);
      TerminateProcess(n->hproc, 1);
    }
    pthread_mutex_unlock(&n->lock);
    pthread_join(n->waittid, &vret);
    // abandoned (stop_reader() answers 1), the reader frees the plane itself
    if((stopped = stop_reader(n->nfp, NULL)) > 0){
      n->nfp = NULL;
    }
#endif
    // (fork) both threads are gone; what they used goes too, rather than
    // leaking descriptors with every subprocess
    subproc_free(n);
    if(vret == NULL){
      ret = -1;
    }else if(vret != PTHREAD_CANCELED){
      ret = *(int*)vret;
      free(vret);
    }
#ifdef __MINGW32__
    if(stopped > 0){
      ret = -1; // the process is gone, but its reader could not be stopped
    }
#endif
  }
  return ret;
}

ncplane* ncsubproc_plane(ncsubproc* n){
  return n->nfp->ncp;
}

// if ttyfp is a tty, return a file descriptor extracted from it. otherwise,
// try to get the controlling terminal. otherwise, return -1.
int get_tty_fd(FILE* ttyfp){
  int fd = -1;
  if(ttyfp){
    if((fd = fileno(ttyfp)) < 0){
      logwarn("no file descriptor was available in outfp %p", ttyfp);
    }else{
      if(tty_check(fd)){
        fd = dup(fd);
      }else{
        loginfo("fd %d is not a TTY", fd);
        fd = -1;
      }
    }
  }
  if(fd < 0){
    fd = open("/dev/tty", O_RDWR | O_CLOEXEC | O_NOCTTY);
    if(fd < 0){
      loginfo("couldn't open /dev/tty (%s)", strerror(errno));
    }else{
      if(!tty_check(fd)){
        loginfo("file descriptor for /dev/tty (%d) is not actually a TTY", fd);
        close(fd);
        fd = -1;
      }
    }
  }
  if(fd >= 0){
    loginfo("returning TTY fd %d", fd);
  }
  return fd;
}

#ifdef __APPLE__
// (fork) The controlling terminal's own device node, opened afresh, or -1.
// macOS's poll() does not support /dev/tty -- the indirection device answers
// POLLNVAL at once, every time -- but polls the device it stands for, which
// the kernel will name: the process's e_tdev, looked up by devname_r().
static int
open_controlling_tty_device(void){
  struct kinfo_proc kp;
  memset(&kp, 0, sizeof(kp));
  size_t len = sizeof(kp);
  int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid(), };
  if(sysctl(mib, 4, &kp, &len, NULL, 0) || len < sizeof(kp)){
    logwarn("couldn't look up the controlling terminal (%s)", strerror(errno));
    return -1;
  }
  const dev_t tdev = kp.kp_eproc.e_tdev;
  if(tdev == NODEV){
    loginfo("no controlling terminal");
    return -1;
  }
  char path[80] = "/dev/";
  if(devname_r(tdev, S_IFCHR, path + 5, (int)(sizeof(path) - 5)) == NULL){
    logwarn("controlling terminal device %ld has no name", (long)tdev);
    return -1;
  }
  int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOCTTY | O_NONBLOCK);
  if(fd < 0){
    // (EACCES after su(1), say: the node is the login user's, mode 620)
    logwarn("couldn't open terminal device %s (%s)", path, strerror(errno));
    return -1;
  }
  struct stat st;
  if(fstat(fd, &st) || !S_ISCHR(st.st_mode) || st.st_rdev != tdev || !tty_check(fd)){
    logwarn("%s is not the controlling terminal", path);
    close(fd);
    return -1;
  }
  loginfo("controlling terminal is %s (fd %d)", path, fd);
  return fd;
}
#endif

// (fork) The controlling terminal, for the input thread to poll() and read()
// when stdin is something else: on its own open file description (never a
// dup() of stdout's, whose flags it would share), and nonblocking, so that
// no read() can ever strand the thread -- with the input discarded between
// poll() and read() by a TCSAFLUSH, say -- where a stop cannot reach it.
// -1 if there is none, or it cannot be had in that form. *|selectonly| is
// set if it can only be waited on with select(), not poll(): macOS's
// /dev/tty, the fallback when the terminal's own device node can't be
// opened (as after su(1), with the node mode 620 and someone else's).
int get_tty_input_fd(bool* selectonly){
  *selectonly = false;
#ifdef __MINGW32__
  return -1; // no POSIX controlling terminal; the console is stdin's own
#else
#ifdef __APPLE__
  int fd = open_controlling_tty_device();
  if(fd < 0){
    // select() watches /dev/tty where poll() can't (it answers POLLNVAL)
    fd = open("/dev/tty", O_RDONLY | O_CLOEXEC | O_NOCTTY | O_NONBLOCK);
    if(fd < 0){
      loginfo("couldn't open /dev/tty (%s)", strerror(errno));
    }else if(!tty_check(fd)){
      loginfo("file descriptor for /dev/tty (%d) is not actually a TTY", fd);
      close(fd);
      fd = -1;
    }else if(fd >= FD_SETSIZE){
      logwarn("/dev/tty is fd %d, beyond select(); terminal input is unread", fd);
      close(fd);
      fd = -1;
    }else{
      logwarn("terminal device unavailable; watching /dev/tty with select()");
      *selectonly = true;
    }
  }
#else
  int fd = open("/dev/tty", O_RDONLY | O_CLOEXEC | O_NOCTTY | O_NONBLOCK);
  if(fd < 0){
    loginfo("couldn't open /dev/tty (%s)", strerror(errno));
  }else if(!tty_check(fd)){
    loginfo("file descriptor for /dev/tty (%d) is not actually a TTY", fd);
    close(fd);
    fd = -1;
  }
#endif
  if(fd >= 0){
    // the open asked for O_NONBLOCK; make sure of it rather than trust it
    if(set_fd_nonblocking(fd, 1, NULL)){
      logerror("couldn't make terminal fd %d nonblocking", fd);
      close(fd);
      fd = -1;
    }
  }
  return fd;
#endif
}
