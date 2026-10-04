#ifndef NOTCURSES_EGCPOOL
#define NOTCURSES_EGCPOOL

#include <wchar.h>
#include <errno.h>
#include <stdio.h>
#include <limits.h>
#include <stdint.h>
#include <wctype.h>
#include <stddef.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unigbrk.h>
#include <unictype.h>
#ifdef __MINGW32__
#include <unistr.h>
#include <uniwidth.h>
#endif
#include "notcurses/notcurses.h"
#include "compat/compat.h"
#include "logging.h"

#ifdef __cplusplus
extern "C" {
#endif

// Whole code points. These stand in for mbrtowc(), wcwidth(), wcrtomb(),
// iswspace() and iswcntrl() wherever one code point is decoded, measured,
// encoded or classified. Off MinGW each is exactly that C library call. On
// MinGW wchar_t (and wint_t) is 16 bits: the UCRT decodes every code point
// beyond the BMP -- emoji, sextants, octants, NCKEY_* -- to U+FFFD, a
// wchar_t cannot hold one to encode, and there is no wcwidth() at all
// (ncport.h stubs it to 1, so every glyph measured one column). There,
// libunistring decodes, encodes and measures.

// Decode one code point from at most 'n' bytes of UTF-8 at 's', never reading
// past a NUL. Answers the bytes consumed, 0 for a NUL (setting *cp to 0), or
// (size_t)-1 for an invalid or truncated sequence.
static inline size_t
nc_mbrtoc32(uint32_t* cp, const char* s, size_t n){
#ifdef __MINGW32__
  if(n == 0){
    return (size_t)-1;
  }
  if(*s == '\0'){
    *cp = 0;
    return 0;
  }
  ucs4_t uc;
  const int r = u8_mbtoucr(&uc, (const uint8_t*)s, strnlen(s, n));
  if(r < 0){ // -1: invalid, -2: truncated
    return (size_t)-1;
  }
  *cp = uc;
  return r;
#else
  mbstate_t mbs;
  memset(&mbs, 0, sizeof(mbs));
  wchar_t w;
  const size_t r = mbrtowc(&w, s, n, &mbs);
  if(r == (size_t)-1 || r == (size_t)-2){
    return (size_t)-1;
  }
  *cp = (uint32_t)w;
  return r;
#endif
}

// Columns one code point occupies: positive, 0 for a zero-width one, or -1
// for one that is not printable (a control character).
static inline int
nc_c32width(uint32_t cp){
#ifdef __MINGW32__
  return uc_width(cp, "UTF-8");
#else
  return wcwidth((wchar_t)cp);
#endif
}

// Encode one code point into 'out', which must hold MB_LEN_MAX bytes. Answers
// the bytes written (no NUL is appended), or (size_t)-1 if it cannot be
// encoded.
static inline size_t
nc_c32rtomb(char* out, uint32_t cp){
#ifdef __MINGW32__
  const int r = u8_uctomb((uint8_t*)out, cp, MB_LEN_MAX);
  return r < 0 ? (size_t)-1 : (size_t)r;
#else
  mbstate_t mbs;
  memset(&mbs, 0, sizeof(mbs));
  return wcrtomb(out, (wchar_t)cp, &mbs);
#endif
}

// iswspace() and iswcntrl() for a whole code point. Neither class has a
// member beyond the BMP, which is all a MinGW wint_t can name.
static inline bool
nc_c32space(uint32_t cp){
#ifdef __MINGW32__
  return cp <= 0xffff && iswspace((wint_t)cp);
#else
  return iswspace((wint_t)cp);
#endif
}

static inline bool
nc_c32cntrl(uint32_t cp){
#ifdef __MINGW32__
  return cp <= 0xffff && iswcntrl((wint_t)cp);
#else
  return iswcntrl((wint_t)cp);
#endif
}

// an nccell only provides storage for up to 4 bytes of an EGC. if there's
// anything more than that, it's spilled into the egcpool, and the nccell
// records the offset. when an nccell is released, the egcpool memory it
// owned is zeroed out, and made usable by another nccell.

typedef struct egcpool {
  char* pool;         // ringbuffer of attached extension storage
  int poolsize;       // total number of bytes in pool
  int poolused;       // bytes actively used, grow when this gets too large
  int poolwrite;      // next place to *look for* a place to write
} egcpool;

#define POOL_MINIMUM_ALLOC BUFSIZ
#define POOL_MAXIMUM_BYTES (1 << 24) // max 16MiB (assumes 32 bits)

static inline void
egcpool_init(egcpool* p){
  p->pool = NULL;
  p->poolsize = 0;
  p->poolwrite = 0;
  p->poolused = 0;
}

static inline int
egcpool_grow(egcpool* pool, int len){
  int newsize = pool->poolsize * 2;
  if(newsize < pool->poolsize){
    return -1; // pernicious overflow (see also POOL_MAXIMUM_BYTES check below)
  }
  if(newsize < POOL_MINIMUM_ALLOC){
    newsize = POOL_MINIMUM_ALLOC;
  }
  while(len > newsize - pool->poolsize){ // ensure we make enough space
    if(newsize * 2 < newsize){
      return -1;
    }
    newsize *= 2;
  }
  if(newsize > POOL_MAXIMUM_BYTES){
    return -1;
  }
  // nasty cast here because c++ source might include this header :/
  char* tmp = (char*)realloc(pool->pool, newsize);
  if(tmp == NULL){
    return -1;
  }
  pool->pool = tmp;
  memset(pool->pool + pool->poolsize, 0, newsize - pool->poolsize);
  pool->poolsize = newsize;
  return 0;
}

// get the expected length of the encoded codepoint from the first byte of a
// utf-8 character. if the byte is illegal as a first byte, 1 is returned.
// Table 3.1B, Legal UTF8 Byte Sequences, Corrigendum #1: UTF-8 Shortest Form.
// subsequent ("continuation") bytes must start with the bit pattern 10.
static inline size_t
utf8_codepoint_length(unsigned char c){
  if(c <= 0x7f){        // 0x000000...0x00007f
    return 1;
  }else if(c <= 0xc1){  // illegal continuation byte
    return 1;
  }else if(c <= 0xdf){  // 0x000080...0x0007ff
    return 2;
  }else if(c <= 0xef){  // 0x000800...0x00ffff
    return 3;
  }else if(c <= 0xf4){  // c <= 0xf4, 0x100000...0x10ffff
    return 4;
  }else{                // illegal first byte
    return 1;
  }
}

// Eat an EGC from the UTF-8 string input, counting bytes and columns. We use
// libunistring's uc_is_grapheme_break() to segment EGCs. Writes the number of
// columns to '*colcount'. Returns the number of bytes consumed, not including
// any NUL terminator. Neither the number of bytes nor columns is necessarily
// equal to the number of decoded code points. Such are the ways of Unicode.
// uc_is_grapheme_break() wants UTF-32, and so does measuring: decode whole
// code points with nc_mbrtoc32(), never through a 16-bit wchar_t.
static inline int
utf8_egc_len(const char* gcluster, int* colcount){
  size_t ret = 0;
  *colcount = 0;
  int r;
  uint32_t wc, prevw = 0;
  bool injoin = false;
  do{
    const size_t s = nc_mbrtoc32(&wc, gcluster, MB_LEN_MAX);
    if(s == (size_t)-1){
      // FIXME probably ought escape this somehow
      logerror("invalid UTF8: %s", gcluster);
      return -1;
    }
    r = (int)s;
    // A NUL terminator contributes neither bytes nor columns. The loop's
    // `while(r)` already stops here, but only after the width logic below has
    // run on wc == 0. That is harmless where wcwidth(0) is 0, as POSIX
    // requires -- but Windows answers it negatively while iswcntrl(0) is
    // false, so the fallback assigned it a width of 1 and ncstrwidth("")
    // reported one column instead of none.
    if(r == 0){
      break;
    }
    if(prevw && !injoin && uc_is_grapheme_break(prevw, wc)){
      break; // starts a new EGC, exit and do not claim
    }
    int cols;
    if(uc_is_property_variation_selector(wc)){ // ends EGC
      ret += r;
      break;
    }else if(wc == 0x200d || injoin){ // ZWJ is iswcntrl, so check it first
      injoin = true;
      cols = 0;
    }else{
      cols = nc_c32width(wc);
      if(cols < 0){
        injoin = false;
        if(nc_c32space(wc)){ // newline or tab
          *colcount = 1;
          return ret + 1;
        }
        cols = 1;
        if(nc_c32cntrl(wc)){
          logerror("prohibited or invalid unicode: 0x%08x", (unsigned)wc);
          return -1;
        }
      }
    }
    if(*colcount == 0){
      *colcount += cols;
    }
    ret += r;
    gcluster += r;
    if(!prevw){
      prevw = wc;
    }
  }while(r);
  // FIXME what if injoin is set? incomplete EGC!
  return ret;
}

// stash away the provided UTF8, NUL-terminated grapheme cluster. the cluster
// should not be less than 2 bytes (such a cluster should be directly stored in
// the cell). returns -1 on error, and otherwise a non-negative offset. 'ulen'
// must be the number of bytes to lift from egc (utf8_egc_len()).
__attribute__ ((nonnull (1, 2)))
int egcpool_stash(egcpool* pool, const char* egc, size_t ulen);

// remove the egc from the pool. start at offset, and zero out everything until
// we find a zero (our own NUL terminator). remove that number of bytes from
// the usedcount.
static inline void
egcpool_release(egcpool* pool, int offset){
  size_t freed = 1; // account for free(d) NUL terminator
  while(pool->pool[offset]){
    pool->pool[offset] = '\0';
    ++freed;
    ++offset;
    assert(offset < pool->poolsize);
  }
  pool->poolused -= freed;
  // FIXME ought we update pool->poolwrite?
}

static inline void
egcpool_dump(egcpool* pool){
  free(pool->pool);
  egcpool_init(pool);
}

// get the offset into the egcpool for this cell's EGC. returns meaningless and
// unsafe results if called on a simple cell.
static inline uint32_t
cell_egc_idx(const nccell* c){
  return (htole(c->gcluster) & 0x00fffffflu);
}

// Is the cell a spilled (more than 4 byte) UTF8 EGC?
static inline bool
cell_extended_p(const nccell* c){
  return (htole(c->gcluster) & 0xff000000ul) == 0x01000000ul;
}

// Is the cell simple (a UTF8-encoded EGC of four bytes or fewer)?
static inline bool
cell_simple_p(const nccell* c){
  return !cell_extended_p(c);
}

// only applies to complex cells, do not use on simple cells
__attribute__ ((__returns_nonnull__)) static inline const char*
egcpool_extended_gcluster(const egcpool* pool, const nccell* c) {
  assert(cell_extended_p(c));
  uint32_t idx = cell_egc_idx(c);
  return pool->pool + idx;
}

// Duplicate the contents of EGCpool 'src' onto another, wiping out any prior
// contents in 'dst'.
static inline int
egcpool_dup(egcpool* dst, const egcpool* src){
  if(src->pool){
    char* tmp;
    if((tmp = (char*)realloc(dst->pool, src->poolsize)) == NULL){
      return -1;
    }
    dst->pool = tmp;
    memcpy(dst->pool, src->pool, src->poolsize);
  }
  dst->poolsize = src->poolsize;
  dst->poolused = src->poolused;
  dst->poolwrite = src->poolwrite;
  return 0;
}

#ifdef __cplusplus
}
#endif

#endif
