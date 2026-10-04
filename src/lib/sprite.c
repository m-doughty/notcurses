#include "internal.h"
#include "visual-details.h"
#include <stdatomic.h>

static atomic_uint_fast32_t sprixelid_nonce;

void sprixel_debug(const sprixel* s, FILE* out){
  fprintf(out, "sprixel %d (%p) %" PRIu64 "B %dx%d (%dx%d) @%d/%d state: %d\n",
          s->id, s, s->glyph.used, s->dimy, s->dimx, s->pixy, s->pixx,
          s->n ? s->n->absy : 0, s->n ? s->n->absx : 0,
          s->invalidated);
  if(s->n){
    int idx = 0;
    for(unsigned y = 0 ; y < s->dimy ; ++y){
      for(unsigned x = 0 ; x < s->dimx ; ++x){
        fprintf(out, "%d", s->n->tam[idx].state);
        ++idx;
      }
      fprintf(out, "\n");
    }
    idx = 0;
    for(unsigned y = 0 ; y < s->dimy ; ++y){
      for(unsigned x = 0 ; x < s->dimx ; ++x){
        if(s->n->tam[idx].state == SPRIXCELL_ANNIHILATED){
          if(s->n->tam[idx].auxvector){
            fprintf(out, "%03d] %p\n", idx, s->n->tam[idx].auxvector);
          }else{
            fprintf(out, "%03d] missing!\n", idx);
          }
        }
        ++idx;
      }
    }
  }
}

// doesn't splice us out of any lists, just frees
void sprixel_free(sprixel* s){
  if(s){
    loginfo("destroying sprixel %u", s->id);
    if(s->n){
      s->n->sprite = NULL;
    }
    sixelmap_free(s->smap);
    free(s->needs_refresh);
    fbuf_free(&s->glyph);
    free(s);
  }
}

sprixel* sprixel_recycle(ncplane* n){
  assert(n->sprite);
  const notcurses* nc = ncplane_notcurses_const(n);
  if(nc->tcache.pixel_implementation >= NCPIXEL_KITTY_STATIC){
    sprixel* hides = n->sprite;
    int dimy = hides->dimy;
    int dimx = hides->dimx;
    sprixel_hide(hides);
    return sprixel_alloc(n, dimy, dimx);
  }
  sixelmap_free(n->sprite->smap);
  n->sprite->smap = NULL;
  return n->sprite;
}

// store the original (absolute) coordinates from which we moved, so that
// we can invalidate them in sprite_draw().
void sprixel_movefrom(sprixel* s, int y, int x){
  if(s->invalidated != SPRIXEL_HIDE && s->invalidated != SPRIXEL_UNSEEN){
    if(s->invalidated != SPRIXEL_MOVED){
    // FIXME if we're Sixel, we need to effect any wipes that were run
    // (we normally don't because redisplaying sixel doesn't change
    // what's there--you can't "write transparency"). this is probably
    // best done by conditionally reblitting the sixel(?).
//fprintf(stderr, "SETTING TO MOVE: %d/%d was: %d\n", y, x, s->invalidated);
      s->invalidated = SPRIXEL_MOVED;
      s->movedfromy = y;
      s->movedfromx = x;
    }
  }
}

void sprixel_hide(sprixel* s){
  if(ncplane_pile(s->n) == NULL){ // ncdirect case; destroy now
    sprixel_free(s);
    return;
  }
  // otherwise, it'll be killed in the next rendering cycle.
  if(s->invalidated != SPRIXEL_HIDE){
    loginfo("marking sprixel %u hidden", s->id);
    // a MOVED sprixel has not been redrawn since its plane moved, so its
    // pixels are still on the terminal at movedfrom, not at the plane's
    // current position. keep the origin the move recorded, or the scrub
    // (sixel's only means of removal) damages the wrong cells and the
    // old graphic survives wherever the new frame's cells match the old.
    if(s->invalidated != SPRIXEL_MOVED){
      s->movedfromy = ncplane_abs_y(s->n);
      s->movedfromx = ncplane_abs_x(s->n);
    }
    s->invalidated = SPRIXEL_HIDE;
    // guard; might have already been replaced
    if(s->n){
      s->n->sprite = NULL;
      s->n = NULL;
    }
  }
}

// y and x are absolute coordinates.
void sprixel_invalidate(sprixel* s, int y, int x){
//fprintf(stderr, "INVALIDATING AT %d/%d\n", y, x);
  if(s->invalidated == SPRIXEL_QUIESCENT && s->n){
    int localy = y - s->n->absy;
    int localx = x - s->n->absx;
//fprintf(stderr, "INVALIDATING AT %d/%d (%d/%d) TAM: %d\n", y, x, localy, localx, s->n->tam[localy * s->dimx + localx].state);
    if(s->n->tam[localy * s->dimx + localx].state != SPRIXCELL_TRANSPARENT &&
       s->n->tam[localy * s->dimx + localx].state != SPRIXCELL_ANNIHILATED &&
       s->n->tam[localy * s->dimx + localx].state != SPRIXCELL_ANNIHILATED_TRANS){
      s->invalidated = SPRIXEL_INVALIDATED;
    }
  }
}

sprixel* sprixel_alloc(ncplane* n, int dimy, int dimx){
  sprixel* ret = malloc(sizeof(sprixel));
  if(ret == NULL){
    return NULL;
  }
  memset(ret, 0, sizeof(*ret));
  if(fbuf_init(&ret->glyph)){
    free(ret);
    return NULL;
  }
  ret->n = n;
  ret->dimy = dimy;
  ret->dimx = dimx;
  if(ncplane_pile(n)){
    ret->cellpxy = ncplane_pile(n)->cellpxy;
    ret->cellpxx = ncplane_pile(n)->cellpxx;
  }
  ret->id = ++sprixelid_nonce;
  ret->needs_refresh = NULL;
  if(ret->id >= 0x1000000){
    ret->id = 1;
    sprixelid_nonce = 1;
  }
//fprintf(stderr, "LOOKING AT %p (p->n = %p)\n", ret, ret->n);
  if(ncplane_pile(ret->n)){ // rendered mode
    ncpile* np = ncplane_pile(ret->n);
    if( (ret->next = np->sprixelcache) ){
      ret->next->prev = ret;
    }
    np->sprixelcache = ret;
    ret->prev = NULL;
//fprintf(stderr, "%p %p %p\n", nc->sprixelcache, ret, nc->sprixelcache->next);
  }else{ // ncdirect case
    ret->next = ret->prev = NULL;
  }
  return ret;
}

// |pixy| and |pixx| are the output pixel geometry (i.e. |pixy| must be a
// multiple of 6 for sixel). output coverage ought already have been loaded.
// takes ownership of 's' on success. frees any existing glyph.
int sprixel_load(sprixel* spx, fbuf* f, unsigned pixy, unsigned pixx,
                 int parse_start, sprixel_e state){
  assert(spx->n);
  if(&spx->glyph != f){
    fbuf_free(&spx->glyph);
    memcpy(&spx->glyph, f, sizeof(*f));
  }
  spx->invalidated = state;
  spx->pixx = pixx;
  spx->pixy = pixy;
  spx->parse_start = parse_start;
  return 0;
}

// returns 1 if already annihilated, 0 if we successfully annihilated the cell,
// or -1 if we could not annihilate the cell (i.e. we're sixel).
int sprite_wipe(const notcurses* nc, sprixel* s, int ycell, int xcell){
  assert(s->n);
  int idx = s->dimx * ycell + xcell;
  if(s->n->tam[idx].state == SPRIXCELL_TRANSPARENT){
    // need to make a transparent auxvec, because a reload will force us to
    // update said auxvec, but needn't actually change the glyph. auxvec will
    // be entirely 0s coming from pixel_trans_auxvec().
    if(s->n->tam[idx].auxvector == NULL){
      if(nc->tcache.pixel_trans_auxvec){
        s->n->tam[idx].auxvector = nc->tcache.pixel_trans_auxvec(ncplane_pile(s->n));
        if(s->n->tam[idx].auxvector == NULL){
          return -1;
        }
      }
    }
    // no need to update to INVALIDATED; no redraw is necessary
    s->n->tam[idx].state = SPRIXCELL_ANNIHILATED_TRANS;
    return 1;
  }
  if(s->n->tam[idx].state == SPRIXCELL_ANNIHILATED_TRANS ||
     s->n->tam[idx].state == SPRIXCELL_ANNIHILATED){
//fprintf(stderr, "CACHED WIPE %d %d/%d\n", s->id, ycell, xcell);
    return 0;
  }
  logdebug("wiping %p %d %d/%d", s->n->tam, idx, ycell, xcell);
  int r = nc->tcache.pixel_wipe(s, ycell, xcell);
//fprintf(stderr, "WIPED %d %d/%d ret=%d\n", s->id, ycell, xcell, r);
  // mark the cell as annihilated whether we actually scrubbed it or not,
  // so that we use this fact should we move to another frame
  s->n->tam[idx].state = SPRIXCELL_ANNIHILATED;
  assert(s->n->tam[idx].auxvector);
  return r;
}

int sprite_clear_all(const tinfo* t, fbuf* f){
  if(t->pixel_clear_all == NULL){
    return 0;
  }
  return t->pixel_clear_all(f);
}

// we don't want to seed the process-wide prng, but we do want to stir in a
// bit of randomness for our purposes. we probably ought just use platform-
// specific APIs, but for now, throw the timestamp in there, lame FIXME.
int sprite_init(tinfo* t, int fd){
  struct timeval tv;
  gettimeofday(&tv, NULL);
  int stir = (tv.tv_sec >> 3) ^ tv.tv_usec;
  sprixelid_nonce = (rand() ^ stir) % 0xffffffu;
  if(t->pixel_init == NULL){
    return 0;
  }
  return t->pixel_init(t, fd);
}

int sprixel_rescale(sprixel* spx, unsigned ncellpxy, unsigned ncellpxx){
  assert(spx->n);
  if(!ncellpxy || !ncellpxx || !spx->cellpxy || !spx->cellpxx ||
     spx->pixy <= 0 || spx->pixx <= 0){
    return -1;
  }
  // A client can re-blit between the geometry poll and render. Such an image
  // already owns a correct TAM; rebuilding it would erase its coverage.
  if(spx->cellpxy == ncellpxy && spx->cellpxx == ncellpxx){
    return 0;
  }
  loginfo("rescaling -> %ux%u", ncellpxy, ncellpxx);
  unsigned nrows = (spx->pixy - 1u) / ncellpxy + 1u;
  unsigned ncols = (spx->pixx - 1u) / ncellpxx + 1u;
  if(nrows > INT_MAX / ncols ||
     (size_t)nrows * ncols > SIZE_MAX / sizeof(tament)){
    return -1;
  }
  tament* ntam = create_tam(nrows, ncols);
  if(ntam == NULL){
    return -1;
  }
  for(unsigned y = 0 ; y < spx->dimy ; ++y){
    for(unsigned x = 0 ; x < spx->dimx ; ++x){
      // Rebuild with the geometry which allocated each auxiliary vector,
      // even though the destination pile already advertises the new size.
      if(sprite_rebuild(ncplane_notcurses(spx->n), spx, y, x) < 0){
        free(ntam);
        return -1;
      }
    }
  }
  // Project old coverage conservatively. Only a cell covered entirely by
  // opaque old cells may suppress text; mixed cells retain normal repaint.
  const bool sixel = ncplane_notcurses(spx->n)->tcache.pixel_implementation < NCPIXEL_KITTY_STATIC;
  for(unsigned y = 0 ; y < nrows ; ++y){
    const uint64_t y0 = (uint64_t)y * ncellpxy;
    const uint64_t y1 = y0 + ncellpxy;
    for(unsigned x = 0 ; x < ncols ; ++x){
      const uint64_t x0 = (uint64_t)x * ncellpxx;
      const uint64_t x1 = x0 + ncellpxx;
      bool opaque = y1 <= (unsigned)spx->pixy && x1 <= (unsigned)spx->pixx;
      bool transparent = true;
      opaque &= y1 <= (uint64_t)spx->dimy * spx->cellpxy &&
                x1 <= (uint64_t)spx->dimx * spx->cellpxx;
      for(uint64_t oy = y0 / spx->cellpxy ; oy < spx->dimy && oy <= (y1 - 1) / spx->cellpxy ; ++oy){
        for(uint64_t ox = x0 / spx->cellpxx ; ox < spx->dimx && ox <= (x1 - 1) / spx->cellpxx ; ++ox){
          sprixcell_e state = spx->n->tam[oy * spx->dimx + ox].state;
          transparent &= state == SPRIXCELL_TRANSPARENT;
          opaque &= state == SPRIXCELL_OPAQUE_SIXEL || state == SPRIXCELL_OPAQUE_KITTY;
        }
      }
      ntam[y * ncols + x].state = transparent ? SPRIXCELL_TRANSPARENT
        : opaque ? (sixel ? SPRIXCELL_OPAQUE_SIXEL : SPRIXCELL_OPAQUE_KITTY)
        : (sixel ? SPRIXCELL_MIXED_SIXEL : SPRIXCELL_MIXED_KITTY);
    }
  }
  ncplane* n = spx->n;
  tament* oldtam = n->tam;
  n->tam = NULL;
  n->sprite = NULL; // resizing must not queue this live image for destruction
  int ret = ncplane_resize_internal(n, 0, 0, 0, 0, 0, 0, nrows, ncols);
  n->sprite = spx;
  if(ret && (n->leny != nrows || n->lenx != ncols)){
    n->tam = oldtam;
    free(ntam);
    return -1;
  }
  cleanup_tam(oldtam, spx->dimy, spx->dimx);
  free(oldtam);
  n->tam = ntam;
  spx->dimy = nrows;
  spx->dimx = ncols;
  spx->cellpxy = ncellpxy;
  spx->cellpxx = ncellpxx;
  free(spx->needs_refresh); // indexed by the old TAM dimensions
  spx->needs_refresh = NULL;
  spx->invalidated = SPRIXEL_INVALIDATED;
  ncplane_notcurses(n)->physical_geometry_changed = true;
  return ret;
}
