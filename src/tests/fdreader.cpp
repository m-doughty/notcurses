#include "main.h"
#include "lib/fdreader.h"
#include <atomic>
#include <random>
#include <thread>

// (fork) The reader/destroyer handoff an ncfdplane uses where a destroy has
// to cancel the reader's read() (Windows; see stop_reader() in fd.c). The
// protocol itself is portable, so it is tested here on every platform: the
// races below are exactly the ones the Windows reader and destroyer run.

TEST_CASE("FdReaderHandoff") {
  fdreader r;
  fdreader_init(&r);

  SUBCASE("ReadsAreDeliveredUntilStopped") {
    CHECK_FALSE(fdreader_reading(&r));
    REQUIRE(fdreader_enter_read(&r));
    CHECK(fdreader_reading(&r));
    CHECK(FDREADER_DELIVER == fdreader_leave_read(&r));
    CHECK_FALSE(fdreader_reading(&r)); // a callback is never a cancel target
    REQUIRE(fdreader_enter_read(&r));
    fdreader_stop(&r);
    CHECK(fdreader_stopping(&r));
    CHECK(fdreader_reading(&r)); // the read in progress is a cancel target
    CHECK(FDREADER_STOP == fdreader_leave_read(&r)); // its result is dropped
    CHECK_FALSE(fdreader_enter_read(&r)); // and there is no further read
    CHECK_FALSE(fdreader_reading(&r));
  }

  SUBCASE("StopBeforeFirstRead") {
    fdreader_stop(&r);
    CHECK_FALSE(fdreader_enter_read(&r));
    CHECK_FALSE(fdreader_reading(&r));
    CHECK_FALSE(fdreader_abandon(&r)); // not reading: the destroyer joins
  }

  SUBCASE("AbandonedReaderOwnsThePlane") {
    REQUIRE(fdreader_enter_read(&r));
    fdreader_stop(&r);
    CHECK(fdreader_abandon(&r));
    CHECK_FALSE(fdreader_reading(&r)); // no more cancels are aimed at it
    CHECK_FALSE(fdreader_abandon(&r)); // and it can be given away only once
    CHECK(FDREADER_OWNED == fdreader_leave_read(&r));
    CHECK_FALSE(fdreader_enter_read(&r));
  }

  SUBCASE("ReaderInCallbackIsNeverAbandoned") {
    REQUIRE(fdreader_enter_read(&r));
    CHECK(FDREADER_DELIVER == fdreader_leave_read(&r));
    fdreader_stop(&r); // the destroy begins while the callback runs
    CHECK_FALSE(fdreader_abandon(&r));
    CHECK_FALSE(fdreader_enter_read(&r)); // the callback returns; it leaves
  }

  SUBCASE("ReinitClearsEverything") {
    REQUIRE(fdreader_enter_read(&r));
    fdreader_stop(&r);
    REQUIRE(fdreader_abandon(&r));
    fdreader_init(&r);
    CHECK_FALSE(fdreader_stopping(&r));
    CHECK_FALSE(fdreader_reading(&r));
    CHECK(fdreader_enter_read(&r));
  }
}

namespace {

// Spins on |f| until it is true; after a while, yielding too, so that a
// machine with fewer cores than threads still makes progress.
template<typename F>
void spin_until(F f){
  for(unsigned spins = 0 ; !f() ; ++spins){
    if(spins > 4096){
      std::this_thread::yield();
    }
  }
}

void jitter(unsigned n){
  volatile unsigned sink = 0; // keeps the loop from being optimized away
  for(unsigned i = 0 ; i < n ; ++i){
    sink = i;
  }
  (void)sink;
}

// Runs |a| here and |b| on a second thread, released together, |rounds|
// times, with |setup| before and |check| after each round. Each side waits a
// random few hundred iterations once released, so that both orders -- and
// genuine overlap -- turn up rather than whichever thread happens to see the
// release first winning every round.
template<typename Setup, typename A, typename B, typename Check>
void race(int rounds, Setup setup, A a, B b, Check check){
  std::atomic<int> ready{0}, go{0}, done{0};
  std::minstd_rand rng(0x6e63);
  unsigned delaya = 0, delayb = 0;
  std::thread tb([&]{
    for(int i = 1 ; i <= rounds ; ++i){
      ready.store(i);
      spin_until([&]{ return go.load() == i; });
      jitter(delayb);
      b();
      done.store(i);
    }
  });
  for(int i = 1 ; i <= rounds ; ++i){
    setup();
    delaya = rng() % 400;
    delayb = rng() % 400;
    spin_until([&]{ return ready.load() == i; });
    go.store(i);
    jitter(delaya);
    a();
    spin_until([&]{ return done.load() == i; });
    check();
  }
  tb.join();
}

}

// Whichever of the reader leaving its read and the destroyer giving up on it
// comes first, exactly one of them ends up owning the plane.
TEST_CASE("FdReaderAbandonRaceHasOneOwner") {
  constexpr int rounds = 20000;
  fdreader r;
  fdreader_after after = FDREADER_DELIVER;
  bool abandoned = false;
  int owned = 0, joined = 0;
  race(rounds,
       [&]{ fdreader_init(&r); CHECK(fdreader_enter_read(&r)); fdreader_stop(&r); },
       [&]{ after = fdreader_leave_read(&r); },
       [&]{ abandoned = fdreader_abandon(&r); },
       [&]{
         CHECK(abandoned == (after == FDREADER_OWNED));
         CHECK(after != FDREADER_DELIVER); // stopped first: nothing delivered
         ++(abandoned ? owned : joined);
       });
  CHECK(rounds == owned + joined);
  // both orders are expected; not seeing one means the race was not run,
  // not that the protocol failed, so it is reported rather than failed
  WARN(0 < owned);
  WARN(0 < joined);
}

// The destroyer asks for a stop and then looks to see whether to cancel; the
// reader leaves its read and then looks to see whether to deliver. Never
// both "deliver" and "cancel": a cancel must not land in a callback.
TEST_CASE("FdReaderCancelNeverHitsACallback") {
  constexpr int rounds = 20000;
  fdreader r;
  fdreader_after after = FDREADER_DELIVER;
  bool cancelled = false;
  int delivered = 0, stopped = 0;
  race(rounds,
       [&]{ fdreader_init(&r); CHECK(fdreader_enter_read(&r)); },
       [&]{ after = fdreader_leave_read(&r); },
       [&]{ fdreader_stop(&r); cancelled = fdreader_reading(&r); },
       [&]{
         CHECK_FALSE((after == FDREADER_DELIVER && cancelled));
         CHECK(after != FDREADER_OWNED);
         ++(after == FDREADER_DELIVER ? delivered : stopped);
       });
  CHECK(rounds == delivered + stopped);
  WARN(0 < delivered); // as above: both orders are expected
  WARN(0 < stopped);
}
