#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include "lib/internal.h"
#include <string>

ncloglevel_e loglevel = NCLOGLEVEL_SILENT;

static notcurses* testing_notcurses(){
  notcurses_options opts{};
  opts.flags = NCOPTION_SUPPRESS_BANNERS | NCOPTION_NO_ALTERNATE_SCREEN;
  return notcurses_core_init(&opts, nullptr);
}

TEST_CASE("SprixelGeometryOwnership") {
  auto nc = testing_notcurses();
  REQUIRE(nc);
  auto parent = notcurses_stdplane(nc);
  auto pile = ncplane_pile(parent);
  pile->cellpxy = 6;
  pile->cellpxx = 4;
  nc->tcache.pixel_implementation = NCPIXEL_SIXEL;
  ncplane_options opts{};
  opts.rows = opts.cols = 4;
  auto plane = ncplane_create(parent, &opts);
  REQUIRE(plane);
  auto s = sprixel_alloc(plane, 4, 4);
  REQUIRE(s);
  plane->sprite = s;
  plane->tam = create_tam(4, 4);
  REQUIRE(plane->tam);
  s->pixy = 24;
  s->pixx = 16;
  for(unsigned y = 0; y < 4; ++y){
    for(unsigned x = 0; x < 4; ++x){
      plane->tam[y * 4 + x].state = y < 2 && x >= 2
        ? SPRIXCELL_TRANSPARENT : SPRIXCELL_OPAQUE_SIXEL;
    }
  }
  plane->tam[8].state = SPRIXCELL_MIXED_SIXEL;

  SUBCASE("FreshBlitKeepsCoverageAndPendingDamage") {
    auto tam = plane->tam;
    s->needs_refresh = (unsigned char*)calloc(16, 1);
    REQUIRE(s->needs_refresh);
    s->needs_refresh[15] = 1;
    auto refresh = s->needs_refresh;
    CHECK(0 == sprixel_rescale(s, 6, 4));
    CHECK(plane->tam == tam);
    CHECK(s->needs_refresh == refresh);
    CHECK(plane->tam[0].state == SPRIXCELL_OPAQUE_SIXEL);
    CHECK(plane->tam[2].state == SPRIXCELL_TRANSPARENT);
    CHECK(plane->tam[8].state == SPRIXCELL_MIXED_SIXEL);
  }

  SUBCASE("RetainedImageRebuildsUsingOwnedStride") {
    plane->tam[0].state = SPRIXCELL_ANNIHILATED;
    plane->tam[0].auxvector = calloc(6 * 4, 2);
    REQUIRE(plane->tam[0].auxvector);
    nc->tcache.pixel_rebuild = [](sprixel* image, int y, int x, uint8_t* aux){
      CHECK(image->cellpxy == 6);
      CHECK(image->cellpxx == 4);
      CHECK(aux[6 * 4 * 2 - 1] == 0);
      image->n->tam[y * image->dimx + x].state = SPRIXCELL_OPAQUE_SIXEL;
      return 1;
    };
    pile->cellpxy = 12;
    pile->cellpxx = 8;
    s->needs_refresh = (unsigned char*)calloc(16, 1);
    REQUIRE(s->needs_refresh);
    CHECK(0 == sprixel_rescale(s, 12, 8));
    CHECK(s->n == plane);
    CHECK(plane->sprite == s);
    CHECK(s->invalidated == SPRIXEL_INVALIDATED);
    CHECK(s->cellpxy == 12);
    CHECK(s->cellpxx == 8);
    CHECK(ncplane_dim_y(plane) == 2);
    CHECK(ncplane_dim_x(plane) == 2);
    CHECK(s->needs_refresh == nullptr);
    CHECK(plane->tam[0].state == SPRIXCELL_OPAQUE_SIXEL);
    CHECK(plane->tam[1].state == SPRIXCELL_TRANSPARENT);
    CHECK(plane->tam[2].state == SPRIXCELL_MIXED_SIXEL);
    CHECK(plane->tam[3].state == SPRIXCELL_OPAQUE_SIXEL);
    CHECK(nc->physical_geometry_changed);
  }

  SUBCASE("InvalidGeometryLeavesImageIntact") {
    auto tam = plane->tam;
    CHECK(-1 == sprixel_rescale(s, 0, 8));
    CHECK(-1 == sprixel_rescale(s, 12, 0));
    CHECK(plane->tam == tam);
    CHECK(s->n == plane);
    CHECK(ncplane_dim_y(plane) == 4);
  }
  SUBCASE("RebuildFailurePreservesOwnedBuffers") {
    auto tam = plane->tam;
    tam[0].state = SPRIXCELL_ANNIHILATED;
    tam[0].auxvector = calloc(6 * 4, 2);
    REQUIRE(tam[0].auxvector);
    auto aux = tam[0].auxvector;
    nc->tcache.pixel_rebuild = [](sprixel*, int, int, uint8_t*){ return -1; };
    CHECK(-1 == sprixel_rescale(s, 12, 8));
    CHECK(plane->tam == tam);
    CHECK(tam[0].auxvector == aux);
    CHECK(s->cellpxy == 6);
    CHECK(s->cellpxx == 4);
    CHECK(ncplane_dim_y(plane) == 4);
    CHECK(plane->sprite == s);
  }
  CHECK(0 == ncplane_destroy(plane));
  CHECK(0 == notcurses_stop(nc));
}

TEST_CASE("GeometryRepaintSurvivesPollUntilRasterization") {
  auto nc = testing_notcurses();
  REQUIRE(nc);
  auto plane = notcurses_stdplane(nc);
  FILE* terminal = nc->ttyfp;
  FILE* capture = tmpfile();
  REQUIRE(capture);
  nc->ttyfp = capture;
  CHECK(1 == ncplane_set_base(plane, "X", 0, 0));
  CHECK(0 == notcurses_render(nc));
  nc->physical_geometry_changed = true;
  unsigned r, c, cy, cx;
  CHECK(0 == notcurses_poll_geometry(nc, &r, &c, &cy, &cx));
  CHECK(0 == notcurses_poll_geometry(nc, &r, &c, &cy, &cx));
  CHECK(nc->physical_geometry_changed);
  auto before = nc->stats.s.cellemissions;
  CHECK(0 == notcurses_render(nc));
  CHECK(nc->stats.s.cellemissions - before == (uint64_t)r * c);
  CHECK_FALSE(nc->physical_geometry_changed);
  before = nc->stats.s.cellemissions;
  CHECK(0 == notcurses_render(nc));
  CHECK(nc->stats.s.cellemissions == before);
  fflush(capture);
  rewind(capture);
  std::string bytes;
  char chunk[4096];
  size_t count;
  while((count = fread(chunk, 1, sizeof(chunk), capture))){ bytes.append(chunk, count); }
  auto clear = bytes.find("\x1b[2J");
  CHECK(clear != std::string::npos);
  CHECK(bytes.find("\x1b[2J", clear + 1) == std::string::npos);
  CHECK(bytes.find(std::string(c, 'X'), clear) != std::string::npos);
  nc->ttyfp = terminal;
  fclose(capture);
  CHECK(0 == notcurses_stop(nc));
}
