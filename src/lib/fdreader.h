#ifndef NOTCURSES_FDREADER
#define NOTCURSES_FDREADER

#ifdef __cplusplus
extern "C" {
#endif

// internal header, not installed

#include <stdbool.h>

// (fork) The handoff between an ncfdplane's reader thread and a destroy from
// another thread. Everywhere, the destroy asks with fdreader_stop() and the
// reader, seeing fdreader_stopping(), leaves without delivering anything
// more (POSIX wakes it through a pipe in its poll(): ask_reader_to_stop() in
// fd.c). The rest is for Windows, where the reader blocks in read() and is
// stopped by cancelling that read (stop_reader() in fd.c). Pure: no I/O,
// clock or allocation, so that the protocol is tested on every platform.
//
// The reader brackets each read() with fdreader_enter_read() and
// fdreader_leave_read(). The destroyer calls fdreader_stop(), aims a cancel
// at the reader only while fdreader_reading() says it is in read(), and can
// give up on a read that will not cancel with fdreader_abandon(). Every
// operation is sequentially consistent, and two guarantees rest on that:
//
//  * A cancel never lands in a callback, where it would break the callback's
//    own I/O (a render's write to the terminal, say). A reader that leaves
//    read() after fdreader_stop() is told FDREADER_STOP, never
//    FDREADER_DELIVER; and one told FDREADER_DELIVER is not seen as reading
//    by any fdreader_reading() after the stop until it is back in read().
//  * Exactly one side frees the plane. fdreader_abandon() succeeds only on a
//    reader in read(), whose fdreader_leave_read() then answers
//    FDREADER_OWNED: the plane is the reader's to free, and the destroyer
//    must not touch it again. Otherwise the destroyer waits, joins and frees.

// How long a destroy on Windows waits for a reader blocked in a read() that
// does not cancel before abandoning it to finish on its own. Waiting on a
// callback is not bounded: the destroy then blocks until the callback
// returns, as documented (and as on POSIX), since the callback is the
// caller's code and could otherwise still be running after the destroy had
// returned.
#define FDREADER_STOP_MS 1000

typedef enum {
  FDREADER_IDLE,      // not in read(): in a callback, or between reads
  FDREADER_READING,   // in read() (or just about to be, or just out of it)
  FDREADER_ABANDONED, // given up on while reading; the reader owns the plane
} fdreader_phase;

typedef struct fdreader {
  int phase;          // an fdreader_phase; only through the functions below
  bool stopping;      // set once, by the destroyer; likewise
} fdreader;

// What the reader does after a read() returns.
typedef enum {
  FDREADER_DELIVER,   // carry on: pass the data, end of file or error on
  FDREADER_STOP,      // stopped from outside: leave, reporting nothing
  FDREADER_OWNED,     // abandoned: leave, reporting nothing, freeing the plane
} fdreader_after;

static inline void
fdreader_init(fdreader* r){
  __atomic_store_n(&r->phase, FDREADER_IDLE, __ATOMIC_SEQ_CST);
  __atomic_store_n(&r->stopping, false, __ATOMIC_SEQ_CST);
}

// Destroyer: ask the reader to stop. It leaves at its next look.
static inline void
fdreader_stop(fdreader* r){
  __atomic_store_n(&r->stopping, true, __ATOMIC_SEQ_CST);
}

// Reader: has a stop been asked for?
static inline bool
fdreader_stopping(fdreader* r){
  return __atomic_load_n(&r->stopping, __ATOMIC_SEQ_CST);
}

// Reader: about to read(). False if a stop has been asked for, in which case
// it must not read, but leave.
static inline bool
fdreader_enter_read(fdreader* r){
  if(fdreader_stopping(r)){
    return false;
  }
  __atomic_store_n(&r->phase, FDREADER_READING, __ATOMIC_SEQ_CST);
  return true;
}

// Reader: read() has returned.
static inline fdreader_after
fdreader_leave_read(fdreader* r){
  if(__atomic_exchange_n(&r->phase, FDREADER_IDLE, __ATOMIC_SEQ_CST)
     == FDREADER_ABANDONED){
    return FDREADER_OWNED;
  }
  return fdreader_stopping(r) ? FDREADER_STOP : FDREADER_DELIVER;
}

// Destroyer: is the reader in read(), where a cancel reaches it and nothing
// else?
static inline bool
fdreader_reading(fdreader* r){
  return __atomic_load_n(&r->phase, __ATOMIC_SEQ_CST) == FDREADER_READING;
}

// Destroyer: give up on a reader stuck in read(). True if it was in read():
// the plane is now the reader's, and the destroyer must not touch it again.
// False if it was not (it is in a callback, or on its way out): keep waiting.
static inline bool
fdreader_abandon(fdreader* r){
  int expected = FDREADER_READING;
  return __atomic_compare_exchange_n(&r->phase, &expected, FDREADER_ABANDONED,
                                     false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}

#ifdef __cplusplus
}
#endif

#endif
