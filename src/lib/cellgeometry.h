#ifndef NOTCURSES_CELLGEOMETRY
#define NOTCURSES_CELLGEOMETRY

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <limits.h>

// Pure query state; callers synchronize access. No allocation, borrowed data,
// or request IDs (XTWINOPS reports contain none). A complete valid reply is
// the latest available observation, including replies arriving after timeout.
typedef struct cellgeometry {
  unsigned y, x;
  uint64_t next_query_ns, deadline_ns;
  unsigned timeouts;
  bool outstanding;
} cellgeometry;

// Parse the matched CSI ... t without unsigned wrap or accepting empty fields.
// The input need not be NUL terminated. Successful output is all-or-nothing.
static inline bool
cellgeometry_parse(const unsigned char* data, size_t len,
                   unsigned* kind, unsigned* y, unsigned* x){
  if(len < 8 || data[0] != 0x1b || data[1] != '[' || data[len - 1] != 't'){
    return false;
  }
  unsigned v[3] = {0};
  size_t pos = 2;
  for(unsigned i = 0; i < 3; ++i){
    size_t start = pos;
    while(pos < len && data[pos] >= '0' && data[pos] <= '9'){
      unsigned digit = data[pos++] - '0';
      if(v[i] > (UINT_MAX - digit) / 10){ return false; }
      v[i] = v[i] * 10 + digit;
    }
    if(pos == start || pos >= len || data[pos++] != (i == 2 ? 't' : ';')){
      return false;
    }
  }
  if(pos != len || !v[1] || !v[2] || v[1] > INT_MAX || v[2] > INT_MAX){
    return false;
  }
  // Match the representable range of native terminal pixel geometry.
  if(v[0] == 6 && (v[1] > UINT16_MAX || v[2] > UINT16_MAX)){ return false; }
  *kind = v[0]; *y = v[1]; *x = v[2];
  return true;
}

static inline void
cellgeometry_observe(cellgeometry* g, unsigned y, unsigned x){
  if(g->timeouts >= 3){ g->next_query_ns = 0; }
  g->y = y;
  g->x = x;
  g->outstanding = false;
  g->timeouts = 0;
}

// 4 Hz maximum, one outstanding query, 1 s response deadline. Three missed
// replies back off to one probe per 30 s; a later valid reply restores 4 Hz.
// Returns 2 on entering backoff so the caller can log the capability gap once.
static inline int
cellgeometry_query_due(cellgeometry* g, uint64_t now){
  if(g->outstanding){
    if(now < g->deadline_ns){ return 0; }
    g->outstanding = false;
    if(g->timeouts < 3){ ++g->timeouts; }
    if(g->timeouts == 3){
      g->next_query_ns = now + UINT64_C(30000000000);
      // Keep a separate saturated state so we don't postpone every poll.
      g->timeouts = 4;
      return 2;
    }
  }
  if(now < g->next_query_ns){ return 0; }
  g->outstanding = true;
  g->deadline_ns = now + UINT64_C(1000000000);
  g->next_query_ns = now + (g->timeouts >= 3
      ? UINT64_C(30000000000) : UINT64_C(250000000));
  return 1;
}

#endif
