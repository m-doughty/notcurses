#include <doctest/doctest.h>
#include "lib/internal.h"

TEST_CASE("SixelScrubClipsNegativeOrigin") {
  crender cells[9]{};
  ncpile pile{};
  pile.dimy = 3;
  pile.dimx = 3;
  pile.crender = cells;
  sprixel sprite{};
  sprite.movedfromy = -1;
  sprite.movedfromx = -1;
  sprite.dimy = 3;
  sprite.dimx = 3;
  sprite.invalidated = SPRIXEL_HIDE;
  CHECK(1 == sixel_scrub(&pile, &sprite));
  for(int y = 0; y < 3; ++y){
    for(int x = 0; x < 3; ++x){
      CHECK(bool(cells[y * 3 + x].s.damaged) == (y < 2 && x < 2));
    }
  }
}
