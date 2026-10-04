#include "main.h"
#include <atomic>
#include <cerrno>
#include <chrono>
#include <mutex>
#include <cstring>
#include <string>
#include <thread>
#include <fcntl.h>
#include <unistd.h>
#include <condition_variable>
#ifdef _WIN32
#include <io.h>
#include <windows.h>
#endif

static pthread_cond_t cond;
static pthread_mutex_t lock;

// (fork) The same cases on Windows, with what Windows has: the NUL device,
// cmd.exe's `type` in place of cat, and ping for a process that outlives
// the test unless it is ended.
#ifdef _WIN32
#define NC_NULL_DEVICE "NUL"
#define NC_O_CLOEXEC O_NOINHERIT
static char const * const succeeds_argv[] = { "cmd.exe", "/c", "type", "NUL", nullptr, };
static char const * const fails_argv[] = { "cmd.exe", "/c", "type", "C:\\notcurses-test-nope\\nope", nullptr, };
static char const * const hangs_argv[] = { "ping", "-n", "3600", "127.0.0.1", nullptr, };
// writes a line, then runs on until it is ended
static char const * const talks_argv[] = { "cmd.exe", "/c", "echo", "hi", "&",
                                            "ping", "-n", "3600", "127.0.0.1", nullptr, };
#else
#define NC_NULL_DEVICE "/dev/null"
#define NC_O_CLOEXEC O_CLOEXEC
static char const * const succeeds_argv[] = { "/bin/cat", "/dev/null", nullptr, };
// assuming the path /dev/nope doesn't exist, cat ought be successfully
// launched (fork() and exec() both succeed), but then immediately fail.
static char const * const fails_argv[] = { "/bin/cat", "/dev/nope", nullptr, };
static char const * const hangs_argv[] = { "/bin/cat", nullptr, };
// writes a line, then runs on until it is ended
static char const * const talks_argv[] = { "/bin/sh", "-c", "echo hi; exec sleep 3600", nullptr, };
#endif

auto testfdcb(struct ncfdplane* ncfd, const void* buf, size_t s, void* curry) -> int {
  struct ncplane* n = ncfdplane_plane(ncfd);
  pthread_mutex_lock(&lock);
  if(ncplane_putnstr(n, s, static_cast<const char*>(buf)) <= 0){
    pthread_mutex_unlock(&lock);
    return -1;
  }
  notcurses_render(ncplane_notcurses(ncfdplane_plane(ncfd)));
  pthread_mutex_unlock(&lock);
  (void)curry;
  (void)s;
  return 0;
}

auto testfdeof(struct ncfdplane* n, int fderrno, void* curry) -> int {
  bool* outofline_cancelled = static_cast<bool*>(curry);
  pthread_mutex_lock(&lock);
  *outofline_cancelled = true;
  pthread_cond_signal(&cond);
  pthread_mutex_unlock(&lock);
  (void)n;
  (void)fderrno;
  return 0;
}

// Collects what a subprocess writes, for a test that checks it.
struct capture {
  std::string out;
  bool done = false;
  int status = -1;
};

auto capturecb(struct ncfdplane* ncfd, const void* buf, size_t s, void* curry) -> int {
  auto cap = static_cast<capture*>(curry);
  pthread_mutex_lock(&lock);
  cap->out.append(static_cast<const char*>(buf), s);
  pthread_cond_signal(&cond);
  pthread_mutex_unlock(&lock);
  (void)ncfd;
  return 0;
}

auto captureeof(struct ncfdplane* n, int status, void* curry) -> int {
  auto cap = static_cast<capture*>(curry);
  pthread_mutex_lock(&lock);
  cap->status = status;
  cap->done = true;
  pthread_cond_signal(&cond);
  pthread_mutex_unlock(&lock);
  (void)n;
  return 0;
}

auto testfdeofdestroys(struct ncfdplane* n, int fderrno, void* curry) -> int {
  bool* inline_cancelled = static_cast<bool*>(curry);
  pthread_mutex_lock(&lock);
  int ret = ncfdplane_destroy(n);
  *inline_cancelled = true;
  pthread_cond_signal(&cond);
  pthread_mutex_unlock(&lock);
  (void)fderrno;
  return ret;
}

// (fork) A terminal nobody types into, for a reader to block on: the slave
// of a fresh pty on POSIX (its master, kept open in *keep, never writes), and
// the console's own input buffer on Windows, whose reads are the hardest to
// cancel. The descriptor is the caller's (an ncfdplane's) to close; -1 if
// none could be opened.
static auto open_quiet_terminal(int* keep) -> int {
  *keep = -1;
#ifdef _WIN32
  HANDLE h = CreateFileW(L"CONIN$", GENERIC_READ | GENERIC_WRITE,
                         FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                         OPEN_EXISTING, 0, nullptr);
  if(h == INVALID_HANDLE_VALUE){
    return -1;
  }
  int fd = _open_osfhandle(reinterpret_cast<intptr_t>(h), _O_RDONLY | _O_BINARY);
  if(fd < 0){
    CloseHandle(h);
  }
  return fd;
#else
  int master = posix_openpt(O_RDWR | O_NOCTTY);
  if(master < 0){
    return -1;
  }
  const char* name = nullptr;
  if(grantpt(master) == 0 && unlockpt(master) == 0){
    name = ptsname(master);
  }
  int fd = name ? open(name, O_RDONLY | O_NOCTTY | NC_O_CLOEXEC) : -1;
  if(fd < 0){
    close(master);
    return -1;
  }
  *keep = master;
  return fd;
#endif
}

// (fork) Destroy an ncfdplane whose reader is waiting on a descriptor that
// will never produce anything by itself. The destroy must never hang, and
// must come back promptly with the reader stopped and joined: woken from its
// poll() on POSIX, its read() cancelled on Windows -- not merely bounded, by
// abandoning the reader after FDREADER_STOP_MS (stop_reader() in fd.c).
static void destroy_blocked_fdplane(struct ncfdplane* ncfdp){
  const auto start = std::chrono::steady_clock::now();
  const int ret = ncfdplane_destroy(ncfdp);
  const auto took = std::chrono::steady_clock::now() - start;
  CHECK(took < std::chrono::milliseconds(FDREADER_STOP_MS + 500)); // no hang
  CHECK(0 == ret); // the reader was stopped and joined
  CHECK(took < std::chrono::milliseconds(FDREADER_STOP_MS)); // not abandoned
}

// (fork) Descriptors (POSIX) or handles (Windows) the process holds open.
static auto open_handles() -> long {
#ifdef _WIN32
  DWORD count = 0;
  if(!GetProcessHandleCount(GetCurrentProcess(), &count)){
    return -1;
  }
  return static_cast<long>(count);
#else
  long max = sysconf(_SC_OPEN_MAX);
  if(max <= 0 || max > 65536){
    max = 65536;
  }
  long count = 0;
  for(int fd = 0 ; fd < max ; ++fd){
    if(fcntl(fd, F_GETFD) != -1){
      ++count;
    }
  }
  return count;
#endif
}

// (fork) Destroys the ncsubproc from within its own callback: a data callback
// (on the reader's thread), or the done callback (on the waiter's, or on the
// reader's where a pidfd does the waiting), either with ncsubproc_destroy()
// or by destroying its plane (ncfdplane_destroy()). Any callback after that
// is counted as a violation.
struct selfdestroy {
  std::atomic<ncsubproc*> subproc{nullptr}; // published once create returns
  std::atomic<int> destroyed{0};            // destroys performed
  std::atomic<int> answer{-2};              // what the destroy answered
  std::atomic<int> late{0};                 // callbacks after the destroy
  bool viaplane = false;
};

static auto selfdestroy_now(struct ncfdplane* ncfd, selfdestroy* sd) -> void {
  if(sd->destroyed.load()){
    ++sd->late;
    return;
  }
  ncsubproc* sp;
  while((sp = sd->subproc.load()) == nullptr){ // create hasn't returned yet
    std::this_thread::yield();
  }
  sd->answer = sd->viaplane ? ncfdplane_destroy(ncfd) : ncsubproc_destroy(sp);
  ++sd->destroyed;
}

auto selfdestroycb(struct ncfdplane* ncfd, const void* buf, size_t s, void* curry) -> int {
  (void)buf;
  (void)s;
  selfdestroy_now(ncfd, static_cast<selfdestroy*>(curry));
  return 0;
}

auto selfdestroyeof(struct ncfdplane* ncfd, int status, void* curry) -> int {
  (void)status;
  selfdestroy_now(ncfd, static_cast<selfdestroy*>(curry));
  return 0;
}

auto ignorecb(struct ncfdplane* ncfd, const void* buf, size_t s, void* curry) -> int {
  (void)ncfd;
  (void)buf;
  (void)s;
  (void)curry;
  return 0;
}

// test ncfdplanes and ncsubprocs
TEST_CASE("FdsAndSubprocs"
          * doctest::description("Fdplanes and subprocedures")) {
  REQUIRE(0 == pthread_cond_init(&cond, NULL));
  REQUIRE(0 == pthread_mutex_init(&lock, NULL));
  auto nc_ = testing_notcurses();
  if(!nc_){
    return;
  }
  struct ncplane* n_ = notcurses_stdplane(nc_);
  REQUIRE(n_);
  REQUIRE(0 == ncplane_cursor_move_yx(n_, 0, 0));

  // destroy the ncfdplane outside of its own context
  SUBCASE("FdPlaneDestroyOffline") {
    bool outofline_cancelled = false;
    ncfdplane_options opts{};
    opts.curry = &outofline_cancelled;
    int fd = open(NC_NULL_DEVICE, O_RDONLY|NC_O_CLOEXEC);
    REQUIRE(0 <= fd);
    auto ncfdp = ncfdplane_create(n_, &opts, fd, testfdcb, testfdeof);
    REQUIRE(ncfdp);
    pthread_mutex_lock(&lock);
    CHECK(0 == notcurses_render(nc_));
    while(!outofline_cancelled){
      pthread_cond_wait(&cond, &lock);
    }
    pthread_mutex_unlock(&lock);
    CHECK(0 == ncfdplane_destroy(ncfdp));
    CHECK(0 == notcurses_render(nc_));
  }

  // destroy the ncfdplane within its own context, i.e. from the eof callback
  SUBCASE("FdPlaneDestroyInline") {
    bool inline_cancelled = false;
    ncfdplane_options opts{};
    opts.curry = &inline_cancelled;
    int fd = open(NC_NULL_DEVICE, O_RDONLY|NC_O_CLOEXEC);
    REQUIRE(0 <= fd);
    auto ncfdp = ncfdplane_create(n_, &opts, fd, testfdcb, testfdeofdestroys);
    REQUIRE(ncfdp);
    pthread_mutex_lock(&lock);
    CHECK(0 == notcurses_render(nc_));
    while(!inline_cancelled){
      pthread_cond_wait(&cond, &lock);
    }
    pthread_mutex_unlock(&lock);
    CHECK(0 == notcurses_render(nc_));
  }

  /*
  SUBCASE("SubprocDestroyCmdExecFails") {
    char * const argv[] = { "/should-not-exist", nullptr, };
    bool outofline_cancelled = false;
    ncsubproc_options opts{};
    opts.curry = &outofline_cancelled;
    auto ncsubp = ncsubproc_createvp(n_, &opts, argv[0], argv, testfdcb, testfdeof);
    REQUIRE(ncsubp);
    pthread_mutex_lock(&lock);
    CHECK(0 == notcurses_render(nc_));
    while(!outofline_cancelled){
      pthread_cond_wait(&cond, &lock);
    }
    lck.unlock();
    CHECK(0 != ncsubproc_destroy(ncsubp));
    // FIXME we ought get indication of an error here! or via callback...
    CHECK(0 == notcurses_render(nc_));
  }
  */

  // destroy an ncfdplane whose reader is blocked: a pipe with a live, quiet
  // writer never reaches end of file, so the destroy has to stop the read
  SUBCASE("FdPlaneDestroyBlocked") {
    capture cap;
    ncfdplane_options opts{};
    opts.curry = &cap;
    int pipes[2];
#ifdef _WIN32
    REQUIRE(0 == _pipe(pipes, 256, O_BINARY | O_NOINHERIT));
#else
    REQUIRE(0 == pipe(pipes));
#endif
    auto ncfdp = ncfdplane_create(n_, &opts, pipes[0], capturecb, captureeof);
    REQUIRE(ncfdp);
    // one byte through, so the reader is known to be running; it then
    // blocks in its next read, which is given a moment to begin
    REQUIRE(1 == write(pipes[1], "x", 1));
    pthread_mutex_lock(&lock);
    while(cap.out.empty()){
      pthread_cond_wait(&cond, &lock);
    }
    pthread_mutex_unlock(&lock);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    destroy_blocked_fdplane(ncfdp);
    CHECK(!cap.done); // stopped from outside: no done callback
    CHECK("x" == cap.out);
    CHECK(0 == close(pipes[1]));
    CHECK(0 == notcurses_render(nc_));
  }

  // (fork) destroy a reader the moment it has been created, again and again:
  // stopped with pthread_cancel(), as it was, one in a few hundred hung for
  // good on macOS, the cancel lost (see ask_reader_to_stop() in fd.c)
  SUBCASE("FdPlaneCreateDestroyRace") {
    int pipes[2];
#ifdef _WIN32
    REQUIRE(0 == _pipe(pipes, 256, O_BINARY | O_NOINHERIT));
#else
    REQUIRE(0 == pipe(pipes));
#endif
    for(int i = 0 ; i < 500 ; ++i){
      capture cap;
      ncfdplane_options opts{};
      opts.curry = &cap;
      const int fd = dup(pipes[0]); // each plane owns, and closes, its own
      REQUIRE(0 <= fd);
      auto ncfdp = ncfdplane_create(n_, &opts, fd, capturecb, captureeof);
      REQUIRE(ncfdp);
      CHECK(0 == ncfdplane_destroy(ncfdp));
      CHECK(!cap.done);
    }
    CHECK(0 == close(pipes[0]));
    CHECK(0 == close(pipes[1]));
    CHECK(0 == notcurses_render(nc_));
  }

  // the same over a terminal that never produces input (see
  // open_quiet_terminal()): on Windows, the console read a cancel has the
  // hardest time reaching
  SUBCASE("FdPlaneDestroyBlockedTerminal") {
    capture cap;
    ncfdplane_options opts{};
    opts.curry = &cap;
    int keep = -1;
    const int fd = open_quiet_terminal(&keep);
    REQUIRE(0 <= fd);
    auto ncfdp = ncfdplane_create(n_, &opts, fd, capturecb, captureeof);
    REQUIRE(ncfdp);
    // nothing comes through to show the reader running, so it is simply
    // given time to block in its read
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    destroy_blocked_fdplane(ncfdp);
    CHECK(!cap.done); // stopped from outside: no done callback
    if(keep >= 0){
      CHECK(0 == close(keep));
    }
    CHECK(0 == notcurses_render(nc_));
  }

  SUBCASE("SubprocDestroyCmdSucceeds") {
    char const * const * argv = succeeds_argv;
    bool outofline_cancelled = false;
    ncsubproc_options opts{};
    opts.curry = &outofline_cancelled;
    auto ncsubp = ncsubproc_createvp(n_, &opts, argv[0], argv, testfdcb, testfdeof);
    REQUIRE(ncsubp);
    pthread_mutex_lock(&lock);
    CHECK(0 == notcurses_render(nc_));
    while(!outofline_cancelled){
      pthread_cond_wait(&cond, &lock);
    }
    pthread_mutex_unlock(&lock);
    CHECK(0 == ncsubproc_destroy(ncsubp));
    CHECK(0 == notcurses_render(nc_));
  }

  // the program is launched, but then immediately fails
  SUBCASE("SubprocDestroyCmdFailed") {
    char const * const * argv = fails_argv;
    bool outofline_cancelled = false;
    ncsubproc_options opts{};
    opts.curry = &outofline_cancelled;
    auto ncsubp = ncsubproc_createvp(n_, &opts, argv[0], argv, testfdcb, testfdeof);
    REQUIRE(ncsubp);
    pthread_mutex_lock(&lock);
    CHECK(0 == notcurses_render(nc_));
    while(!outofline_cancelled){
      pthread_cond_wait(&cond, &lock);
    }
    pthread_mutex_unlock(&lock);
    CHECK(0 != ncsubproc_destroy(ncsubp));
    CHECK(0 == notcurses_render(nc_));
  }

  // (fork) create and destroy each kind, over and over, and hold no more
  // descriptors afterwards than before: a subprocess used to leak its pipe,
  // both ends, and its pidfd, and a plane its wake pipe
  SUBCASE("FdPlanesAndSubprocsLeaveNothingOpen") {
    auto cycle = [&]{
      int pipes[2];
#ifdef _WIN32
      REQUIRE(0 == _pipe(pipes, 256, O_BINARY | O_NOINHERIT));
#else
      REQUIRE(0 == pipe(pipes));
#endif
      {
        capture cap;
        ncfdplane_options opts{};
        opts.curry = &cap;
        auto ncfdp = ncfdplane_create(n_, &opts, pipes[0], capturecb, captureeof);
        REQUIRE(ncfdp);
        CHECK(0 == ncfdplane_destroy(ncfdp)); // closes pipes[0]
      }
      CHECK(0 == close(pipes[1]));
      { // runs to the end by itself, and is destroyed once it has
        capture cap;
        ncsubproc_options opts{};
        opts.curry = &cap;
        auto ncsubp = ncsubproc_createvp(n_, &opts, succeeds_argv[0], succeeds_argv,
                                         capturecb, captureeof);
        REQUIRE(ncsubp);
        pthread_mutex_lock(&lock);
        while(!cap.done){
          pthread_cond_wait(&cond, &lock);
        }
        pthread_mutex_unlock(&lock);
        CHECK(0 == ncsubproc_destroy(ncsubp));
      }
      { // destroyed while it runs
        capture cap;
        ncsubproc_options opts{};
        opts.curry = &cap;
        auto ncsubp = ncsubproc_createvp(n_, &opts, hangs_argv[0], hangs_argv,
                                         capturecb, captureeof);
        REQUIRE(ncsubp);
        ncsubproc_destroy(ncsubp); // killed: its answer is the kill's
      }
    };
    // whatever the first round sets up for good (on Windows, say, the
    // loader's and the thread pool's handles) is not a leak
    cycle();
    const long before = open_handles();
    REQUIRE(0 < before);
    constexpr int rounds = 25;
    for(int i = 0 ; i < rounds ; ++i){
      cycle();
    }
    const long after = open_handles();
#ifdef _WIN32
    // the system's own handles come and go a little; a leak would be at
    // least one a round
    CHECK(after < before + rounds / 2);
#else
    CHECK(after == before);
#endif
    CHECK(0 == notcurses_render(nc_));
  }

  // (fork) The man page has a destroy from within a callback reclaim
  // everything as the thread exits. It used to join its own thread, and
  // then free the plane its reader was still using.
  SUBCASE("SubprocDestroyedFromItsOwnCallbacks") {
    // once its threads are gone, the descriptors it held must be too
    auto settles = [](long before){
      for(int i = 0 ; i < 500 ; ++i){
        const long now = open_handles();
#ifdef _WIN32
        if(now < before + 4){ // the system's own handles come and go a little
#else
        if(now <= before){
#endif
          return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
      return false;
    };
    // the handle count needs a round's worth of one-time setup behind it
    {
      capture cap;
      ncsubproc_options opts{};
      opts.curry = &cap;
      auto ncsubp = ncsubproc_createvp(n_, &opts, succeeds_argv[0], succeeds_argv,
                                       capturecb, captureeof);
      REQUIRE(ncsubp);
      pthread_mutex_lock(&lock);
      while(!cap.done){
        pthread_cond_wait(&cond, &lock);
      }
      pthread_mutex_unlock(&lock);
      ncsubproc_destroy(ncsubp);
    }
    struct scenario {
      const char* what;
      char const * const * argv;
      bool fromdone; // destroy from the done callback, not a data callback
      bool viaplane; // with ncfdplane_destroy() on its plane
    } scenarios[] = {
      { "data callback", talks_argv, false, false, },
      { "data callback, via its plane", talks_argv, false, true, },
      { "done callback", succeeds_argv, true, false, },
    };
    for(const auto& sc : scenarios){
      const std::string what = sc.what;
      CAPTURE(what);
      // static: should a thread outlive a failure here, it finds this alive
      static selfdestroy sd;
      sd.subproc = nullptr;
      sd.destroyed = 0;
      sd.answer = -2;
      sd.late = 0;
      sd.viaplane = sc.viaplane;
      const long before = open_handles();
      ncsubproc_options opts{};
      opts.curry = &sd;
      auto ncsubp = ncsubproc_createvp(n_, &opts, sc.argv[0], sc.argv,
                                       sc.fromdone ? ignorecb : selfdestroycb,
                                       selfdestroyeof);
      REQUIRE(ncsubp);
      sd.subproc = ncsubp;
      for(int i = 0 ; i < 1000 && sd.destroyed.load() == 0 ; ++i){
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
      CHECK(1 == sd.destroyed.load());
      CHECK(0 == sd.answer.load());
      CHECK(settles(before)); // everything reclaimed, threads gone
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      CHECK(0 == sd.late.load()); // and nothing called back since
    }
    CHECK(0 == notcurses_render(nc_));
  }

  SUBCASE("SubprocDestroyCmdHung") {
    char const * const * argv = hangs_argv;
    bool outofline_cancelled = false;
    ncsubproc_options opts{};
    opts.curry = &outofline_cancelled;
    auto ncsubp = ncsubproc_createvp(n_, &opts, argv[0], argv, testfdcb, testfdeof);
    REQUIRE(ncsubp);
    WARN(0 != ncsubproc_destroy(ncsubp)); // FIXME
    CHECK(0 == notcurses_render(nc_));
  }

#ifdef _WIN32
  // Windows looks the program up before launching anything, so a name that
  // is nowhere on the search path is refused at once (on POSIX the launch
  // can succeed and the exec then fail; see the commented-out case above).
  SUBCASE("SubprocCmdNotFound") {
    char const * const argv[] = { "notcurses-test-no-such-program", nullptr, };
    bool outofline_cancelled = false;
    ncsubproc_options opts{};
    opts.curry = &outofline_cancelled;
    CHECK(nullptr == ncsubproc_createvp(n_, &opts, argv[0], argv, testfdcb, testfdeof));
    CHECK(!outofline_cancelled);
  }

  // An argument with a space reaches the child as one argument, quoted, and
  // the child's output reaches the callback. cmd.exe's echo prints the rest
  // of its command line as it found it, quotes and all.
  SUBCASE("SubprocArgumentsAreQuoted") {
    capture cap;
    ncsubproc_options opts{};
    opts.curry = &cap;
    char const * const argv[] = { "cmd.exe", "/c", "echo", "two words", nullptr, };
    auto ncsubp = ncsubproc_createvp(n_, &opts, argv[0], argv, capturecb, captureeof);
    REQUIRE(ncsubp);
    pthread_mutex_lock(&lock);
    while(!cap.done){
      pthread_cond_wait(&cond, &lock);
    }
    pthread_mutex_unlock(&lock);
    CHECK(0 == ncsubproc_destroy(ncsubp)); // the reader has stopped: cap is ours
    CHECK(0 == cap.status);
    CHECK("\"two words\"\r\n" == cap.out);
  }
#endif

  CHECK(0 == pthread_cond_destroy(&cond));
  // FIXME why does this (very rarely) fail? ugh
  WARN(0 == pthread_mutex_destroy(&lock));

  CHECK(0 == notcurses_stop(nc_));
}
