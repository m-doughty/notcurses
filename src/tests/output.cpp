#include "main.h"
#include <string>

TEST_CASE("Output") {
  auto nc_ = testing_notcurses();
  if(!nc_){
    return;
  }
  ncplane* n_ = notcurses_stdplane(nc_);
  REQUIRE(n_);

  SUBCASE("RenderToBufferBorrowed") {
    CHECK(10 == ncplane_putstr_yx(n_, 0, 0, "buffer-one"));
    CHECK(0 == ncpile_render(n_));
    char* buf = nullptr;
    size_t buflen = 0;
    REQUIRE(0 == ncpile_render_to_buffer(n_, &buf, &buflen));
    REQUIRE(nullptr != buf);
    CHECK(0 < buflen);
    CHECK(std::string::npos != std::string(buf, buflen).find("buffer-one"));

    char* borrowed = buf;
    CHECK(10 == ncplane_putstr_yx(n_, 0, 0, "buffer-two"));
    CHECK(0 == ncpile_render(n_));
    REQUIRE(0 == ncpile_render_to_buffer(n_, &buf, &buflen));
    REQUIRE(nullptr != buf);
    CHECK(borrowed == buf);
    CHECK(0 < buflen);
    CHECK(std::string::npos != std::string(buf, buflen).find("buffer-two"));
    CHECK(std::string::npos == std::string(buf, buflen).find("buffer-one"));

    REQUIRE(0 == ncpile_render_to_buffer(n_, &buf, &buflen));
    REQUIRE(nullptr != buf);
    CHECK(borrowed == buf);
    CHECK(0 < buflen);
    CHECK(std::string::npos != std::string(buf, buflen).find("buffer-two"));
  }

  // we ought be able to fill up the plane using any alignment, even if we
  // spill over
  SUBCASE("AlignLeftFull") {
    struct ncplane_options nopts{};
    nopts.rows = 1;
    nopts.cols = 10;
    auto n = ncplane_create(n_, &nopts);
    REQUIRE(n);
    CHECK(-10 == ncplane_putstr_aligned(n, 0, NCALIGN_LEFT, "0123456789L"));
    CHECK(0 == notcurses_render(nc_));
    for(unsigned x = 0 ; x < ncplane_dim_x(n) ; ++x){
      auto egc = notcurses_at_yx(nc_, 0, x, nullptr, nullptr);
      char expected[2] = {0};
      REQUIRE(egc);
      expected[0] = x + '0';
      CHECK(0 == strcmp(egc, expected));
      free(egc);
    }
    CHECK(0 == ncplane_destroy(n));
  }

  // we ought be able to fill up the plane using any alignment, even if we
  // spill over
  SUBCASE("AlignRightFull") {
    struct ncplane_options nopts{};
    nopts.rows = 1;
    nopts.cols = 10;
    auto n = ncplane_create(n_, &nopts);
    REQUIRE(n);
    CHECK(-10 == ncplane_putstr_aligned(n, 0, NCALIGN_RIGHT, "0123456789L"));
    CHECK(0 == notcurses_render(nc_));
    for(unsigned x = 0 ; x < ncplane_dim_x(n) ; ++x){
      auto egc = notcurses_at_yx(nc_, 0, x, nullptr, nullptr);
      char expected[2] = {0};
      REQUIRE(egc);
      expected[0] = x + '0';
      CHECK(0 == strcmp(egc, expected));
      free(egc);
    }
    CHECK(0 == ncplane_destroy(n));
  }

  CHECK(0 == notcurses_stop(nc_));

}
