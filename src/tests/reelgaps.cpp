#include "main.h"

// ahci-1
int t1_tablet_cb(struct nctablet* t, bool drawfromtop){
  REQUIRE(nullptr != t);
  (void)drawfromtop;
  return 6;
}

// mpt3sas-0
int t2_tablet_cb(struct nctablet* t, bool drawfromtop){
  REQUIRE(nullptr != t);
  (void)drawfromtop;
  return 24;
}

// ahci-0
int t3_tablet_cb(struct nctablet* t, bool drawfromtop){
  REQUIRE(nullptr != t);
  (void)drawfromtop;
  return 6;
}

// virtual
int t4_tablet_cb(struct nctablet* t, bool drawfromtop){
  REQUIRE(nullptr != t);
  (void)drawfromtop;
  return 17;
}

// nvme-0
int t5_tablet_cb(struct nctablet* t, bool drawfromtop){
  REQUIRE(nullptr != t);
  (void)drawfromtop;
  return 3;
}

// nvme-1
int t6_tablet_cb(struct nctablet* t, bool drawfromtop){
  REQUIRE(nullptr != t);
  (void)drawfromtop;
  return 3;
}

// nvme-2
int t7_tablet_cb(struct nctablet* t, bool drawfromtop){
  REQUIRE(nullptr != t);
  (void)drawfromtop;
  return 3;
}

// xhci_pci-0
int t8_tablet_cb(struct nctablet* t, bool drawfromtop){
  REQUIRE(nullptr != t);
  (void)drawfromtop;
  return 5;
}

// https://github.com/dankamongmen/notcurses/issues/901: six tablets of
// assorted heights, the focus walked past the last of them, on a reel bound
// to |n| (which the reel then owns: ncreel_destroy() destroys it). Each
// step is rendered if |render|; the reel lays itself out regardless.
static void
reel_gapping(struct notcurses* nc, struct ncplane* n, bool render){
  auto rendered = [&]{ return render ? notcurses_render(nc) : 0; };
  ncreel_options r{};
  r.bordermask = 0xf;
  r.tabletchan = NCCHANNELS_INITIALIZER(0, 0xb0, 0xb0, 0, 0, 0);
  r.focusedchan = NCCHANNELS_INITIALIZER(0xff, 0xff, 0xff, 0, 0, 0);
  struct ncreel* nr = ncreel_create(n, &r);
  REQUIRE(nr);
  CHECK_EQ(0, ncreel_redraw(nr));
  CHECK_EQ(0, rendered());
  CHECK(ncreel_validate(nr));
  auto t1 = ncreel_add(nr, NULL, NULL, t1_tablet_cb, NULL);
  CHECK(nullptr != t1);
  CHECK(ncreel_validate(nr));
  CHECK_EQ(0, rendered());
  auto t2 = ncreel_add(nr, NULL, NULL, t2_tablet_cb, NULL);
  CHECK(nullptr != t2);
  CHECK(ncreel_validate(nr));
  CHECK_EQ(0, rendered());
  auto t3 = ncreel_add(nr, NULL, NULL, t3_tablet_cb, NULL);
  CHECK(nullptr != t3);
  CHECK(ncreel_validate(nr));
  CHECK_EQ(0, rendered());
  auto t4 = ncreel_add(nr, NULL, NULL, t4_tablet_cb, NULL);
  CHECK(nullptr != t4);
  CHECK(ncreel_validate(nr));
  CHECK_EQ(0, rendered());
  auto t5 = ncreel_add(nr, NULL, NULL, t5_tablet_cb, NULL);
  CHECK(nullptr != t5);
  CHECK(ncreel_validate(nr));
  CHECK_EQ(0, rendered());
  auto t6 = ncreel_add(nr, NULL, NULL, t6_tablet_cb, NULL);
  CHECK(nullptr != t6);
  CHECK(ncreel_validate(nr));
  CHECK_EQ(0, rendered());
  ncreel_next(nr); // move to t2
  CHECK(ncreel_validate(nr));
  CHECK_EQ(0, rendered());
  ncreel_next(nr); // move to t3
  CHECK(ncreel_validate(nr));
  CHECK_EQ(0, rendered());
  ncreel_next(nr); // move to t4
  CHECK(ncreel_validate(nr));
  CHECK_EQ(0, rendered());
  ncreel_next(nr); // move to t5
  CHECK(ncreel_validate(nr));
  CHECK_EQ(0, rendered());
  ncreel_next(nr); // move to t6
  CHECK(ncreel_validate(nr));
  CHECK_EQ(0, rendered());
  ncreel_next(nr); // move to t7
  CHECK(ncreel_validate(nr));
  CHECK_EQ(0, rendered());
  ncreel_next(nr); // move to t8
  CHECK(ncreel_validate(nr));
  CHECK_EQ(0, rendered());
  // (fork) the reel is ours to destroy: left to notcurses_stop(), its plane
  // goes, but tablets without planes of their own leak
  ncreel_destroy(nr);
}

TEST_CASE("ReelGaps") {
  auto nc_ = testing_notcurses();
  if(!nc_){
    return;
  }
  struct ncplane* n_ = notcurses_stdplane(nc_);
  REQUIRE(n_);

  SUBCASE("ReelsGapping") {
    reel_gapping(nc_, n_, true);
  }

  // (fork) The same at every height from 1 to 72 rows, whatever the
  // terminal's own: trimming the tablets that overhang the reel freed one
  // and then wrote to it, at some heights only (34, 36 and 40 rows among
  // them), so that the result of this test depended on the terminal's size.
  // That is all layout (ncreel_redraw()); rendering a thousand frames on
  // top only makes the test slow on a slow terminal.
  SUBCASE("ReelsGappingAtEveryHeight") {
    for(unsigned rows = 1 ; rows <= 72 ; ++rows){
      CAPTURE(rows);
      struct ncplane_options nopts{};
      nopts.rows = rows;
      nopts.cols = 80;
      auto n = ncplane_create(n_, &nopts);
      REQUIRE(n);
      reel_gapping(nc_, n, false); // destroys n with the reel
    }
    CHECK(0 == notcurses_render(nc_));
  }

  CHECK(0 == notcurses_stop(nc_));
}
