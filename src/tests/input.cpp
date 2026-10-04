#include "main.h"
#include <algorithm>
#include <chrono>
#include <initializer_list>
#include <string>
#include <thread>
#include <vector>
#include <fcntl.h>
#ifdef __MINGW32__
#include <io.h>
#else
#include <poll.h>
#include <spawn.h>
#include <signal.h>
#include <unistd.h>
#include <termios.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
extern char** environ;
#endif

namespace {

// (fork) An instance whose input is a pipe the test writes to. Standard
// input is redirected for the instance's lifetime and restored afterwards.
// Where the process has a controlling terminal (a developer's shell), the
// pipe is distinct from it and notcurses treats its bytes as bulk text,
// so control sequences are only decoded from it where there is none (CI,
// and Windows, whose console is never a POSIX tty).
class PipedNotcurses {
public:
  PipedNotcurses(){
#ifdef __MINGW32__
    if(_pipe(fds_, 1 << 16, _O_BINARY)){
      return;
    }
    savedhandle_ = GetStdHandle(STD_INPUT_HANDLE);
#else
    if(pipe(fds_)){
      return;
    }
#endif
    savedfd_ = dup(0);
    if(savedfd_ < 0 || dup2(fds_[0], 0) < 0){
      return;
    }
#ifdef __MINGW32__
    // the console layer reads the Win32 handle, not the CRT descriptor
    SetStdHandle(STD_INPUT_HANDLE, (HANDLE)_get_osfhandle(fds_[0]));
#endif
    notcurses_options nopts{};
    nopts.loglevel = loglevel;
    nopts.flags = NCOPTION_SUPPRESS_BANNERS | NCOPTION_NO_ALTERNATE_SCREEN;
    nc_ = notcurses_init(&nopts, nullptr);
  }

  ~PipedNotcurses(){
    if(nc_){
      notcurses_stop(nc_);
    }
#ifdef __MINGW32__
    if(savedhandle_){
      SetStdHandle(STD_INPUT_HANDLE, savedhandle_);
    }
#endif
    if(savedfd_ >= 0){
      dup2(savedfd_, 0);
      close(savedfd_);
    }
    for(int fd : fds_){
      if(fd >= 0){
        close(fd);
      }
    }
  }

  struct notcurses* nc() const { return nc_; }

  bool feed(const std::string& s){
    size_t off = 0;
    while(off < s.size()){
      const auto w = write(fds_[1], s.data() + off, s.size() - off);
      if(w <= 0){
        return false;
      }
      off += static_cast<size_t>(w);
    }
    return true;
  }

  // (fork) Waits, up to about |ms| milliseconds, until the input layer has
  // read() everything fed so far: nothing is left unread in the pipe. A piece
  // fed only then arrives in a read of its own, with no timer in between: a
  // sleep is only a lower bound, and under load or timer coalescing (macOS
  // CI, where a 20ms sleep was seen to take 60-120ms) can by itself outlast
  // NCINPUT_ESCAPE_HOLD_MS. False if the pipe never drained, or can't be
  // asked.
  //
  // Windows can't be asked. The input thread idles in a synchronous
  // ReadFile() on this pipe (stop_inputlayer() in in.c), and Windows
  // serializes synchronous I/O on a file object: PeekNamedPipe() would wait
  // for that read, and the read for our next write -- a deadlock. There a
  // write goes straight into the pending read, so the reader is only given
  // a moment, spun rather than slept (a sleep is a 15.6ms tick there). No
  // proof rests on it there: the pieces these tests hold back are CSI
  // prefixes, which the win32-input-mode transcoder keeps, with no deadline,
  // until more bytes come (w32im_transcode()), so one read late is absorbed
  // all the same.
  bool drained(int ms){
#ifdef __MINGW32__
    const auto settled = std::chrono::steady_clock::now()
                         + std::chrono::milliseconds(ms < 5 ? ms : 5);
    while(std::chrono::steady_clock::now() < settled){
      std::this_thread::yield();
    }
    return true;
#else
    const auto deadline = std::chrono::steady_clock::now()
                          + std::chrono::milliseconds(ms);
    for(;;){
      int unread = 0;
      if(ioctl(fds_[0], FIONREAD, &unread) < 0){
        return false;
      }
      if(unread == 0){
        return true;
      }
      if(std::chrono::steady_clock::now() >= deadline){
        return false;
      }
      std::this_thread::yield(); // no timer: see above
    }
#endif
  }

  // the next input id within about |ms| milliseconds, or 0
  uint32_t next(int ms){
    ncinput ni;
    return next(ms, &ni);
  }

  // the same, with the whole event in *ni. (fork) Waits on notcurses_get()'s
  // deadline rather than polling between sleeps, so an event is taken the
  // moment it is delivered: a test timing a delivery measures notcurses, not
  // its own polling (a 1ms sleep was seen to take ~26ms at background QoS on
  // macOS), and the wait for nothing is bounded by |ms| however long sleeps
  // take.
  uint32_t next(int ms, ncinput* ni){
    struct timespec deadline;
    // the clock notcurses' input condvar measures deadlines against
    REQUIRE(0 == pthread_condmonotonic_gettime(&deadline));
    ns_to_timespec(timespec_to_ns(&deadline)
                   + static_cast<uint64_t>(ms) * UINT64_C(1000000), &deadline);
    return notcurses_get(nc_, &deadline, ni);
  }

  // stop the instance now rather than on destruction: notcurses_stop()'s
  // answer
  int stop(){
    const int r = notcurses_stop(nc_);
    nc_ = nullptr;
    return r;
  }

private:
  int fds_[2] = { -1, -1 };
  int savedfd_ = -1;
#ifdef __MINGW32__
  HANDLE savedhandle_ = nullptr;
#endif
  struct notcurses* nc_ = nullptr;
};

} // namespace

// (fork) Was |start| less than NCINPUT_ESCAPE_HOLD_MS ago? The timing proofs
// below all rest on this one fact: a hold lasts that long from when its first
// byte was read, which is after |start| (taken before the write), so whatever
// had happened by now happened before any hold begun since |start| could run
// out. Ask only after observing, so that the observation is covered too.
// steady_clock and the library's CLOCK_MONOTONIC advance together.
static auto inside_hold(std::chrono::steady_clock::time_point start) -> bool {
  return std::chrono::steady_clock::now() - start
         < std::chrono::milliseconds(NCINPUT_ESCAPE_HOLD_MS);
}

// (fork) Attempts at a test whose proof needs its events inside the hold
// (inside_hold()). The scheduler can stretch any span -- past the hold, with
// a starved CPU at background QoS -- and an attempt it stretched proves
// nothing either way, so it is run again on a fresh instance; the test fails
// only if no attempt ever fits.
constexpr int HOLD_WINDOW_ATTEMPTS = 10;

// (fork) Feeds |pieces| so that each reaches the input layer in a read of its
// own, the next sent as soon as the last has been read (drained()). True if
// they are known to have arrived within NCINPUT_ESCAPE_HOLD_MS: the span from
// the first write to the last piece having been read bounds the input
// layer's own, since its hold starts once the first piece has been read, and
// a replay is only ever decided once the hold is up. False if the scheduler
// stretched it past that, in which case a replay is correct too, and the
// attempt proves nothing.
static auto feed_fragmented(PipedNotcurses& p,
                            std::initializer_list<const char*> pieces) -> bool {
  const auto start = std::chrono::steady_clock::now();
  for(const char* piece : pieces){
    REQUIRE(p.feed(piece));
    REQUIRE(p.drained(3000));
  }
  return inside_hold(start);
}

TEST_CASE("Input") {
  auto nc_ = testing_notcurses();
  if(!nc_){
    return;
  }
  unsigned dimy, dimx;
  struct ncplane* n_ = notcurses_stddim_yx(nc_, &dimy, &dimx);
  REQUIRE(n_);


  CHECK(0 == notcurses_render(nc_));
  CHECK(0 == notcurses_stop(nc_));

}

// (fork) CSI 200~ and CSI 201~ become NCKEY_PASTE_BEGIN and NCKEY_PASTE_END
// around the pasted text.
TEST_CASE("BracketedPasteMarkers") {
  PipedNotcurses p;
  if(!p.nc()){
    return;
  }
  REQUIRE(p.feed("\x1b[200~ab\x1b[201~"));
  std::vector<uint32_t> got;
  for(uint32_t id ; (id = p.next(3000)) != 0 && got.size() < 16 ; ){
    got.push_back(id);
    if(id == NCKEY_PASTE_END){
      break;
    }
  }
  if(is_test_tty()){
    // bulk path: the bytes are text, and only the text is guaranteed
    size_t a = got.size(), b = got.size();
    for(size_t i = 0 ; i < got.size() ; ++i){
      if(got[i] == 'a' && a == got.size()){ a = i; }
      if(got[i] == 'b' && b == got.size()){ b = i; }
    }
    CHECK(a < got.size());
    CHECK(b < got.size());
    CHECK(a < b);
  }else{
    REQUIRE(4 == got.size());
    CHECK(NCKEY_PASTE_BEGIN == got[0]);
    CHECK('a' == got[1]);
    CHECK('b' == got[2]);
    CHECK(NCKEY_PASTE_END == got[3]);
  }
}

// (fork) A paste larger than the ncinput queue is delivered whole: the
// input thread waits for the client to drain instead of dropping.
TEST_CASE("LargePasteIsNotDropped") {
  PipedNotcurses p;
  if(!p.nc()){
    return;
  }
  constexpr int N = 20000; // more than twice the queue
  std::string paste(N, 'x');
  paste += 'Q';
  REQUIRE(p.feed(paste));
  int xs = 0;
  bool sentinel = false;
  for(uint32_t id ; (id = p.next(3000)) != 0 ; ){
    if(id == 'x'){
      ++xs;
    }else if(id == 'Q'){
      sentinel = true;
      break;
    }
  }
  CHECK(sentinel);
  CHECK(N == xs);
  ncstats stats{};
  notcurses_stats(p.nc(), &stats);
  CHECK(0 == stats.input_errors);
  CHECK(static_cast<uint64_t>(N + 1) == stats.input_events);
}

TEST_CASE("RuntimeCellGeometryReports") {
  if(is_test_tty()){ return; } // the pipe is bulk text with a POSIX /dev/tty
  {
    PipedNotcurses p;
    REQUIRE(p.nc());
    auto check_report = [&](const std::string& bytes) {
      REQUIRE(p.feed(bytes + "Z"));
      CHECK(p.next(3000) == 'Z'); // no report bytes leaked as keypresses
    };
    check_report("\x1b[6;32;14t");
    check_report("\x1b[6;40;18t");
    check_report("\x1b[6;40;18t");
    check_report("\x1b[6;0;14t");
    check_report("\x1b[6;4294967328;14t");
    CHECK(p.next(20) == 0);
  }
  // (fork) and one in two reads, on an instance of its own: an attempt the
  // scheduler stretched past the hold is run again (feed_fragmented())
  for(int attempt = 0 ; attempt < HOLD_WINDOW_ATTEMPTS ; ++attempt){
    PipedNotcurses p;
    REQUIRE(p.nc());
    if(feed_fragmented(p, { "\x1b[6;2", "4;10t" })){
      REQUIRE(p.feed("Z"));
      CHECK(p.next(3000) == 'Z');
      CHECK(p.next(20) == 0);
      return;
    }
  }
  FAIL("no attempt delivered both pieces within NCINPUT_ESCAPE_HOLD_MS");
}

// (fork) Whether an unfinished escape waits for the rest of its bytes. Two
// bytes or fewer -- Escape, Alt+[ -- never wait, so those keys are never
// delayed; three or more wait until NCINPUT_ESCAPE_HOLD_MS after they were
// first seen, however many more bytes of the sequence arrive meanwhile.
TEST_CASE("PartialEscapeHoldPolicy") {
  constexpr uint64_t ms = 1000000;
  const uint64_t t0 = 5000 * ms;
  const uint64_t expiry = t0 + NCINPUT_ESCAPE_HOLD_MS * ms;
  uint64_t deadline = 0;
  CHECK_FALSE(ncinput_hold_escape(0, &deadline, t0));
  CHECK_FALSE(ncinput_hold_escape(1, &deadline, t0));
  CHECK_FALSE(ncinput_hold_escape(2, &deadline, t0));
  CHECK(0 == deadline);
  CHECK(ncinput_hold_escape(3, &deadline, t0));
  CHECK(expiry == deadline);
  CHECK(ncinput_hold_escape(9, &deadline, expiry - 1));
  CHECK(expiry == deadline); // more of the sequence doesn't extend the hold
  CHECK_FALSE(ncinput_hold_escape(9, &deadline, expiry));
  CHECK_FALSE(ncinput_hold_escape(9, &deadline, expiry + 1));
  CHECK(expiry == deadline); // the caller clears it once it gives up
  uint64_t noclock = 0;
  CHECK_FALSE(ncinput_hold_escape(9, &noclock, 0)); // no clock, no hold
  CHECK(0 == noclock);
}

// (fork) A report split over three reads is still absorbed whole, if the
// pieces arrive within NCINPUT_ESCAPE_HOLD_MS. An attempt the scheduler
// stretched past that is run again (feed_fragmented()).
TEST_CASE("ReportFragmentedAcrossReads") {
  for(int attempt = 0 ; attempt < HOLD_WINDOW_ATTEMPTS ; ++attempt){
    PipedNotcurses p;
    REQUIRE(p.nc());
    const bool inside = feed_fragmented(p, { "\x1b[6", ";24", ";10t" });
    REQUIRE(p.feed("Z"));
    if(is_test_tty()){
      // bulk path: the pipe is text, so all of it arrives as keys, in order,
      // however long it took
      for(const char c : std::string("\x1b[6;24;10tZ")){
        CHECK(p.next(3000) == static_cast<uint32_t>(c));
      }
      CHECK(p.next(20) == 0);
      return;
    }
    if(inside){
      CHECK(p.next(3000) == 'Z');
      CHECK(p.next(20) == 0);
      return;
    }
  }
  FAIL("no attempt delivered the pieces within NCINPUT_ESCAPE_HOLD_MS");
}

// (fork) A piece whose remainder never comes is held for
// NCINPUT_ESCAPE_HOLD_MS, and then replayed as the keys it is: nothing is
// lost, and nothing waits longer than that.
TEST_CASE("UnfinishedEscapeIsReplayedAfterHold") {
  PipedNotcurses p;
  REQUIRE(p.nc());
  const auto start = std::chrono::steady_clock::now();
  REQUIRE(p.feed("\x1b[6;2"));
  CHECK(p.next(3000) == NCKEY_ESC);
  if(!is_test_tty()){ // the bulk path (see above) has no escapes to hold
    CHECK(std::chrono::steady_clock::now() - start >=
          std::chrono::milliseconds(NCINPUT_ESCAPE_HOLD_MS));
  }
  for(const char c : std::string("[6;2")){
    CHECK(p.next(3000) == static_cast<uint32_t>(c));
  }
  CHECK(p.next(20) == 0);
}

// (fork) A key typed while a piece is held does not wait for the hold to
// expire: it cannot continue the sequence, so the piece and the key are
// delivered together, in the order they came. Those are the keys a hold that
// ran out would give too, so it is when they come that tells the two apart:
// a replay seen while the piece's hold could not yet have run out
// (inside_hold()) was ended by the key.
TEST_CASE("KeyTypedDuringHoldEndsIt") {
  for(int attempt = 0 ; attempt < HOLD_WINDOW_ATTEMPTS ; ++attempt){
    PipedNotcurses p;
    REQUIRE(p.nc());
    const auto start = std::chrono::steady_clock::now();
    REQUIRE(p.feed("\x1b[6;2"));
    REQUIRE(p.drained(3000)); // read: the key comes in a read of its own
    if(is_test_tty()){
      // bulk path: no escapes are held; everything arrives as it was sent
      REQUIRE(p.feed("x"));
      for(const char c : std::string("\x1b[6;2x")){
        CHECK(p.next(3000) == static_cast<uint32_t>(c));
      }
      CHECK(p.next(20) == 0);
      return;
    }
    if(p.next(0) != 0){
      // replayed before the key was sent: wrong while its hold still ran, and
      // otherwise an attempt the scheduler stretched past it
      CHECK_FALSE(inside_hold(start));
      continue;
    }
    REQUIRE(p.feed("x"));
    const uint32_t first = p.next(3000);
    const bool ended = inside_hold(start); // before its hold could run out
    CHECK(NCKEY_ESC == first);
    for(const char c : std::string("[6;2x")){
      CHECK(p.next(3000) == static_cast<uint32_t>(c));
    }
    CHECK(p.next(20) == 0);
    if(ended){
      return;
    }
  }
  FAIL("no attempt saw the replay before NCINPUT_ESCAPE_HOLD_MS was up");
}

// (fork) After a piece has been replayed as keys, the next escape is walked
// from its own first byte: Alt+x arrives as one key, at once (not held, as a
// stale walk of three bytes or more once made it), and so does Up. The
// automaton's count of bytes walked used to survive the replay, which
// misparsed both, and failed an assertion in debug builds. "At once" is a
// key seen while a hold begun by its own bytes could not yet have run out
// (inside_hold()).
TEST_CASE("EscapeAfterReplayIsWalkedAfresh") {
  for(int attempt = 0 ; attempt < HOLD_WINDOW_ATTEMPTS ; ++attempt){
    PipedNotcurses p;
    REQUIRE(p.nc());
    REQUIRE(p.feed("\x1b[1;"));
    if(is_test_tty()){
      // bulk path: no escapes are walked; everything arrives as it was sent
      REQUIRE(p.feed("\x1bx\x1b[A"));
      for(const char c : std::string("\x1b[1;\x1bx\x1b[A")){
        CHECK(p.next(3000) == static_cast<uint32_t>(c));
      }
      CHECK(p.next(20) == 0);
      return;
    }
    for(const char c : std::string("\x1b[1;")){ // held, then replayed
      CHECK(p.next(3000) == static_cast<uint32_t>(c));
    }
    ncinput ni;
    auto start = std::chrono::steady_clock::now();
    REQUIRE(p.feed("\x1bx"));
    CHECK(p.next(3000, &ni) == 'x');
    bool prompt = inside_hold(start);
    CHECK(ncinput_alt_p(&ni));
    CHECK(p.next(20) == 0); // one key, not Escape and then x
    start = std::chrono::steady_clock::now();
    REQUIRE(p.feed("\x1b[A"));
    CHECK(p.next(3000, &ni) == NCKEY_UP);
    prompt = inside_hold(start) && prompt;
    CHECK(p.next(20) == 0);
    if(prompt){
      return;
    }
  }
  FAIL("no attempt saw both keys before NCINPUT_ESCAPE_HOLD_MS was up");
}

#ifndef __MINGW32__
// (fork) The child half of PipedStdinStopsPromptly, run by the tester as
// "--piped-terminal-child <pty> <locked|open> <loglevel>": a session of its
// own whose controlling terminal is that pty, on its stdout, with its stdin
// the pipe the parent set up. The terminal is then a second input source
// (termfd), read by the input thread alongside stdin. "locked" makes the
// pty's device node unopenable first (mode 000), as after su(1) to another
// user:
// notcurses must then fall back on /dev/tty, which macOS's poll() can't
// watch (get_tty_input_fd()). Answers 0 if the two bytes written down the
// pipe and the key typed at the terminal arrived, and notcurses_stop()
// returned promptly; nonzero otherwise.
auto piped_terminal_child(const char* slave, bool locked) -> int {
  if(setsid() < 0){
    return 10;
  }
  const int fd = open(slave, O_RDWR);
  if(fd < 0){
    return 11;
  }
  if(ioctl(fd, TIOCSCTTY, 0) < 0){ // needed on the BSDs; harmless on Linux
    return 12;
  }
  struct winsize ws{};
  ws.ws_row = 24;
  ws.ws_col = 80;
  if(ioctl(fd, TIOCSWINSZ, &ws) < 0){
    return 18;
  }
  if(locked && fchmod(fd, 0) < 0){ // descriptors already open keep working
    return 19;
  }
  if(dup2(fd, STDOUT_FILENO) < 0){
    return 13;
  }
  if(fd != STDOUT_FILENO){
    close(fd);
  }
  notcurses_options opts{};
  opts.loglevel = loglevel; // the parent's, passed down
  opts.flags = NCOPTION_SUPPRESS_BANNERS | NCOPTION_NO_ALTERNATE_SCREEN
               | NCOPTION_NO_QUIT_SIGHANDLERS;
  auto nc = notcurses_init(&opts, nullptr);
  if(nc == nullptr){
    return 14;
  }
  // with stdin distinct from the terminal, the pipe's bytes are text, and so
  // is a key typed at the terminal. On macOS the input thread used to sleep
  // in a read() of /dev/tty -- poll() can't watch that device -- and never
  // got to the pipe; and the typed key, replayed from the terminal's buffer,
  // was consumed twice over (process_escapes()), failing an assertion.
  std::string got;
  for(int i = 0 ; i < 3000 && got.size() < 3 ; ++i){
    ncinput ni;
    const uint32_t id = notcurses_get_nblock(nc, &ni);
    if(id == 'a' || id == 'b' || id == 'z'){
      got += static_cast<char>(id);
    }else if(id == 0){
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }
  std::sort(got.begin(), got.end()); // the two sources race
  int ret = got == "abz" ? 0 : 15;
  // the input thread back in its wait, nothing to read anywhere
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  const auto start = std::chrono::steady_clock::now();
  if(notcurses_stop(nc) && ret == 0){
    ret = 16;
  }
  if(std::chrono::steady_clock::now() - start >=
     std::chrono::milliseconds(FDREADER_STOP_MS) && ret == 0){
    ret = 17;
  }
  if(locked){
    fchmod(STDOUT_FILENO, 0620); // as grantpt() left it
  }
  return ret;
}

// Runs piped_terminal_child() under a fresh pty, playing the terminal: it
// answers the queries notcurses waits on (the cursor report, and DA1, which
// ends interrogation), types a 'z' once interrogation is over, and drains
// everything else. A child that never exits -- notcurses_stop() hung in its
// join -- is killed and fails the test.
static void run_piped_terminal_child(bool locked){
  const int master = posix_openpt(O_RDWR | O_NOCTTY);
  REQUIRE(0 <= master);
  REQUIRE(0 == grantpt(master));
  REQUIRE(0 == unlockpt(master));
  const char* name = ptsname(master);
  REQUIRE(name);
  const std::string slave = name;
  int in[2];
  REQUIRE(0 == pipe(in));
  REQUIRE(2 == write(in[1], "ab", 2)); // the write end stays open: no EOF
  posix_spawn_file_actions_t fa;
  REQUIRE(0 == posix_spawn_file_actions_init(&fa));
  CHECK(0 == posix_spawn_file_actions_adddup2(&fa, in[0], STDIN_FILENO));
  CHECK(0 == posix_spawn_file_actions_addclose(&fa, in[0]));
  CHECK(0 == posix_spawn_file_actions_addclose(&fa, in[1]));
  CHECK(0 == posix_spawn_file_actions_addclose(&fa, master));
  const std::string level = std::to_string(static_cast<int>(loglevel));
  const char* argv[] = { tester_path(), "--piped-terminal-child", slave.c_str(),
                         locked ? "locked" : "open", level.c_str(), nullptr, };
  pid_t pid = -1;
  const int sr = posix_spawnp(&pid, argv[0], &fa, nullptr,
                              const_cast<char* const*>(argv), environ);
  posix_spawn_file_actions_destroy(&fa);
  close(in[0]);
  REQUIRE(0 == sr);
  std::string pending; // unanswered output, held across reads
  bool typed = false;
  int status = 0;
  bool exited = false;
  const auto start = std::chrono::steady_clock::now();
  while(std::chrono::steady_clock::now() - start < std::chrono::seconds(20)){
    if(waitpid(pid, &status, WNOHANG) == pid){
      exited = true;
      break;
    }
    struct pollfd pfd = { master, POLLIN, 0, };
    if(poll(&pfd, 1, 10) <= 0){
      continue;
    }
    char buf[4096];
    const ssize_t r = read(master, buf, sizeof(buf));
    if(r <= 0){ // EIO once the child's side has gone; waitpid() says when
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      continue;
    }
    pending.append(buf, static_cast<size_t>(r));
    for(;;){ // answer the queries in the order they were asked
      const auto cpr = pending.find("\x1b[6n");
      const auto da1 = pending.find("\x1b[c");
      if(cpr == std::string::npos && da1 == std::string::npos){
        break;
      }
      if(cpr < da1){
        CHECK(6 == write(master, "\x1b[1;1R", 6));
        pending.erase(0, cpr + 4);
      }else{
        CHECK(10 == write(master, "\x1b[?62;22c", 10));
        pending.erase(0, da1 + 3);
        if(!typed){ // the terminal is raw by now: the key goes straight up
          CHECK(1 == write(master, "z", 1));
          typed = true;
        }
      }
    }
    if(pending.size() > 3){ // keep only what could begin a query
      pending.erase(0, pending.size() - 3);
    }
  }
  if(!exited){
    kill(pid, SIGKILL);
    waitpid(pid, &status, 0);
  }
  CHECK(exited); // otherwise notcurses_stop() never returned
  const int signaled = WIFSIGNALED(status) ? WTERMSIG(status) : 0;
  CHECK(0 == signaled);
  const int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  CHECK(0 == code); // see piped_terminal_child() for what each code means
  CHECK(0 == close(in[1]));
  CHECK(0 == close(master));
}
#endif

// (fork) With stdin a pipe that is open and quiet, notcurses_stop() returns
// promptly: the input thread must never be anywhere a stop can't reach it.
// In-process this exercises stdin (on Windows, a blocking ReadFile() that
// stop_inputlayer() has to cancel), plus the controlling terminal where
// there is one. On POSIX a child is then run with a terminal of its own
// (above), so that the terminal is always exercised as a second input:
// stdin distinct from a terminal is how notcurses ends up reading both.
// Windows has no such second source -- the console is stdin itself.
TEST_CASE("PipedStdinStopsPromptly") {
  {
    PipedNotcurses p;
    REQUIRE(p.nc());
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const auto start = std::chrono::steady_clock::now();
    CHECK(0 == p.stop());
    CHECK(std::chrono::steady_clock::now() - start <
          std::chrono::milliseconds(FDREADER_STOP_MS));
  }
#ifndef __MINGW32__
  SUBCASE("TerminalDevice") {
    run_piped_terminal_child(false);
  }
  // the device node unopenable, so the terminal is only to be had through
  // /dev/tty: on macOS, watched with select() (get_tty_input_fd())
  SUBCASE("TerminalViaDevTty") {
    run_piped_terminal_child(true);
  }
#endif
}
