#include "main.h"
#include <set>
#include <vector>
#include <cstring>

// Whole code points: decoding, measuring and encoding one (egcpool.h's
// nc_mbrtoc32(), nc_c32width(), nc_c32rtomb()), and the blitters' wchar_t
// glyph tables (internal.h's nc_wtable_at(), nc_wtable_rfind()).
//
// Every expectation here is the same on every platform. Where wchar_t is
// 16 bits (MinGW) each one failed before those helpers existed: the UCRT
// decoded every code point beyond the BMP to U+FFFD, ncport.h stubbed
// wcwidth() to 1, and the sextant and octant tables -- surrogate pairs in a
// 16-bit wchar_t -- were indexed and searched one code unit at a time.

namespace {

// The first code point of each non-blank cell in row y, left to right.
auto row_glyphs(ncplane* n, unsigned y) -> std::vector<uint32_t> {
  std::vector<uint32_t> glyphs;
  unsigned dimx = ncplane_dim_x(n);
  for(unsigned x = 0 ; x < dimx ; ++x){
    char* egc = ncplane_at_yx(n, y, x, nullptr, nullptr);
    REQUIRE(egc);
    uint32_t cp = 0;
    if(*egc && nc_mbrtoc32(&cp, egc, strlen(egc)) != (size_t)-1 && cp != ' '){
      glyphs.push_back(cp);
    }
    free(egc);
  }
  return glyphs;
}

// Draw every pair of levels a two-column-per-cell blitter can show, one pair
// per cell, on a one-row plot whose range makes each sample value its own
// level. Answers the glyphs drawn.
auto plot_every_level(ncplane* parent, ncblitter_e gridtype, unsigned states)
     -> std::set<uint32_t> {
  ncplane_options nopts{};
  nopts.y = 1;
  nopts.rows = 1;
  nopts.cols = states * states;
  auto ncp = ncplane_create(parent, &nopts);
  REQUIRE(ncp);
  ncplot_options popts{};
  popts.gridtype = gridtype;
  popts.flags = NCPLOT_OPTION_NODEGRADE; // the glyph set under test, or fail
  // range 0..states over one row of `states` levels: an interval of exactly 1
  auto p = ncuplot_create(ncp, &popts, 0, states);
  REQUIRE(p);
  uint64_t x = 0;
  for(unsigned l = 0 ; l < states ; ++l){
    for(unsigned r = 0 ; r < states ; ++r){
      CHECK(0 == ncuplot_add_sample(p, x++, l));
      CHECK(0 == ncuplot_add_sample(p, x++, r));
    }
  }
  auto drawn = row_glyphs(ncuplot_plane(p), 0);
  ncuplot_destroy(p);
  return std::set<uint32_t>(drawn.begin(), drawn.end());
}

}

TEST_CASE("Codepoints") {
  // The glyph tables need neither a locale nor a terminal.
  SUBCASE("TableWalkAcrossPlanes") {
    const wchar_t* t = L"a\U0001FB00b\U0001CD00";
    CHECK('a' == nc_wtable_at(t, 0));
    CHECK(0x1fb00 == nc_wtable_at(t, 1));
    CHECK('b' == nc_wtable_at(t, 2));
    CHECK(0x1cd00 == nc_wtable_at(t, 3));
    CHECK(0 == nc_wtable_at(t, 4)); // the terminator
    CHECK(1 == nc_wtable_rfind(t, 0x1fb00));
    CHECK(3 == nc_wtable_rfind(t, 0x1cd00));
    CHECK(-1 == nc_wtable_rfind(t, 'z'));
  }

  SUBCASE("TableFindIsLastOccurrence") { // wcsrchr() semantics
    CHECK(2 == nc_wtable_rfind(L"x\U0001FB00x", 'x'));
  }

  SUBCASE("BlitterTablesAreWholeGlyphs") {
    auto glyphs = [](const wchar_t* t){
      std::vector<uint32_t> g;
      for(size_t i = 0 ; nc_wtable_at(t, i) ; ++i){
        g.push_back(nc_wtable_at(t, i));
      }
      return g;
    };
    // one glyph per pattern: 2^(width * height)
    CHECK(4 == glyphs(NCHALFBLOCKS).size());
    CHECK(16 == glyphs(NCQUADBLOCKS).size());
    auto sex = glyphs(NCSEXBLOCKS);
    auto oct = glyphs(NCOCTBLOCKS);
    CHECK(64 == sex.size());
    CHECK(256 == oct.size());
    for(const auto& set : {sex, oct}){
      CHECK(' ' == set.front());
      CHECK(0x2588 == set.back()); // full block
      for(auto g : set){
        CHECK(!(g >= 0xd800 && g <= 0xdfff)); // never half a surrogate pair
      }
    }
  }

  // NCSEXBLOCKS[p] is the glyph for pixel pattern p. Unicode numbers its
  // sextants (U+1FB00..U+1FB3B) by that same pattern, skipping the four that
  // older blocks already draw. U+1FB09 used to be missing, so every glyph
  // from pattern 10 on stood one pattern early.
  SUBCASE("SextantTableIsPatternOrder") {
    for(unsigned p = 0 ; p < 64 ; ++p){
      uint32_t want;
      switch(p){
        case 0: want = ' '; break;
        case 21: want = 0x258c; break; // ▌ left half
        case 42: want = 0x2590; break; // ▐ right half
        case 63: want = 0x2588; break; // █ full block
        default: want = 0x1fb00 + (p - 1) - (p > 21) - (p > 42); break;
      }
      CHECK(want == nc_wtable_at(NCSEXBLOCKS, p));
    }
  }

  auto nc_ = testing_notcurses();
  if(!nc_){
    return;
  }
  // Decoding, measuring and encoding go through the C library off MinGW,
  // and it only speaks UTF-8 in a UTF-8 locale.
  if(!notcurses_canutf8(nc_)){
    CHECK(0 == notcurses_stop(nc_));
    return;
  }
  auto n_ = notcurses_stdplane(nc_);
  REQUIRE(n_);

  SUBCASE("DecodeWholeCodepoints") {
    uint32_t cp;
    CHECK(4 == nc_mbrtoc32(&cp, "\U0001F600", 4));
    CHECK(0x1f600 == cp);
    CHECK(3 == nc_mbrtoc32(&cp, "中", 4));
    CHECK(0x4e2d == cp);
    CHECK(2 == nc_mbrtoc32(&cp, "é", 4));
    CHECK(0xe9 == cp);
    CHECK(1 == nc_mbrtoc32(&cp, "h", 4));
    CHECK('h' == cp);
    cp = 0xffffffff;
    CHECK(0 == nc_mbrtoc32(&cp, "", 4));
    CHECK(0 == cp);
    CHECK((size_t)-1 == nc_mbrtoc32(&cp, "\xc3\x28", 4)); // bad continuation
    CHECK((size_t)-1 == nc_mbrtoc32(&cp, "\xf0\x9f", 4)); // truncated by the NUL
    CHECK((size_t)-1 == nc_mbrtoc32(&cp, "\U0001F600", 2)); // truncated by n
  }

  SUBCASE("MeasureWholeCodepoints") {
    CHECK(2 == nc_c32width(0x1f600)); // emoji
    CHECK(2 == nc_c32width(0x4e2d));  // CJK
    CHECK(2 == nc_c32width(0xffe0));  // fullwidth cent sign
    CHECK(1 == nc_c32width('h'));
    CHECK(1 == nc_c32width(0xe9));
    CHECK(1 == nc_c32width(0x1fb38)); // a sextant
    CHECK(0 == nc_c32width(0x301));   // combining acute
    CHECK(0 > nc_c32width(0x07));     // BEL
  }

  SUBCASE("EncodeWholeCodepoints") {
    char mb[MB_LEN_MAX];
    CHECK(4 == nc_c32rtomb(mb, 0x1f600));
    CHECK(0 == memcmp(mb, "\U0001F600", 4));
    CHECK(3 == nc_c32rtomb(mb, 0x4e2d));
    CHECK(0 == memcmp(mb, "中", 3));
    CHECK(1 == nc_c32rtomb(mb, 'h'));
    CHECK('h' == mb[0]);
    CHECK((size_t)-1 == nc_c32rtomb(mb, 0xd800)); // a lone surrogate
  }

  // The exported wcwidth()/wcswidth() equivalents, which notcurses.h maps
  // those two names to on MinGW (its C library has neither, and its wchar_t
  // needs a surrogate pair for anything beyond the BMP).
  SUBCASE("ExportedWidths") {
    CHECK(2 == notcurses_ucs32_width(0x1f600));
    CHECK(2 == notcurses_ucs32_width(0x4e2d));
    CHECK(1 == notcurses_ucs32_width('h'));
    CHECK(0 == notcurses_ucs32_width(0x301));
    CHECK(0 > notcurses_ucs32_width(0x07));
    const wchar_t mixed[] = L"h\U0001F600中!";
    CHECK(6 == notcurses_wcswidth(mixed, sizeof(mixed) / sizeof(*mixed)));
    CHECK(1 == notcurses_wcswidth(mixed, 1));            // stops at n
    CHECK(0 == notcurses_wcswidth(L"", 8));              // and at a NUL
    CHECK(0 > notcurses_wcswidth(L"a\x07", 2));          // -1 if unprintable
    // and the names themselves, as an including program sees them
    CHECK(2 == wcwidth(0x4e2d));
    CHECK(6 == wcswidth(mixed, sizeof(mixed) / sizeof(*mixed)));
  }

  SUBCASE("StringWidths") {
    CHECK(2 == ncstrwidth("\U0001F600", nullptr, nullptr));
    CHECK(2 == ncstrwidth("中", nullptr, nullptr));
    CHECK(5 == ncstrwidth("hé\U0001F600!", nullptr, nullptr));
    CHECK(1 == ncstrwidth("é", nullptr, nullptr)); // e + combining acute
    CHECK(2 == ncstrwidth("\U0001F469‍\U0001F4BB", nullptr, nullptr)); // ZWJ
    CHECK(0 > ncstrwidth("a\x07", nullptr, nullptr));
    CHECK(0 == ncstrwidth("", nullptr, nullptr));
  }

  SUBCASE("PutAdvancesByColumns") {
    CHECK(0 < ncplane_putstr_yx(n_, 0, 0, "hé\U0001F600!"));
    unsigned y, x;
    ncplane_cursor_yx(n_, &y, &x);
    CHECK(0 == y);
    CHECK(5 == x); // h, é, the emoji's two columns, !
    char* egc = ncplane_at_yx(n_, 0, 2, nullptr, nullptr);
    REQUIRE(egc);
    CHECK(0 == strcmp(egc, "\U0001F600"));
    free(egc);
    egc = ncplane_at_yx(n_, 0, 4, nullptr, nullptr);
    REQUIRE(egc);
    CHECK(0 == strcmp(egc, "!"));
    free(egc);
  }

  // White box: claim sextants and octants for this instance, so the paths
  // that use those glyph sets run whatever the terminal (a headless one on
  // Windows offers neither, and would degrade or refuse them).
  nc_->tcache.caps.sextants = true;
  nc_->tcache.caps.octants = true;

  SUBCASE("ReadBackEverySextantAndOctant") {
    struct { ncblitter_e blit; const wchar_t* set; unsigned w, h; } sets[] = {
      { NCBLIT_3x2, NCSEXBLOCKS, 2, 3, },
      { NCBLIT_4x2, NCOCTBLOCKS, 2, 4, },
    };
    // lit pixels read back red, unlit ones blue
    REQUIRE(0 == ncplane_set_fg_rgb8(n_, 0xff, 0, 0));
    REQUIRE(0 == ncplane_set_bg_rgb8(n_, 0, 0, 0xff));
    for(const auto& s : sets){
      for(size_t idx = 0 ; nc_wtable_at(s.set, idx) ; ++idx){
        const uint32_t g = nc_wtable_at(s.set, idx);
        char mb[MB_LEN_MAX + 1];
        const size_t len = nc_c32rtomb(mb, g);
        REQUIRE((size_t)-1 != len);
        mb[len] = '\0';
        ncplane_erase(n_);
        REQUIRE(0 < ncplane_putstr_yx(n_, 0, 0, mb));
        unsigned pxy, pxx;
        uint32_t* px = ncplane_as_rgba(n_, s.blit, 0, 0, 1, 1, &pxy, &pxx);
        CHECK(px); // it found the glyph in its own set
        if(px == nullptr){
          continue;
        }
        CHECK(s.h == pxy);
        CHECK(s.w == pxx);
        // a glyph's index is its pattern (each appears once), and bit b of
        // the pattern is pixel b: left to right, then down
        CHECK((int)idx == nc_wtable_rfind(s.set, g));
        const auto pattern = (unsigned)idx;
        for(unsigned bit = 0 ; bit < s.w * s.h ; ++bit){
          const bool lit = pattern & (1u << bit);
          CHECK(0xff == ncpixel_a(px[bit]));
          CHECK((lit ? 0xffu : 0u) == ncpixel_r(px[bit]));
          CHECK((lit ? 0u : 0xffu) == ncpixel_b(px[bit]));
        }
        free(px);
      }
    }
  }

  SUBCASE("SextantPlotDrawsItsOwnGlyphs") {
    const std::set<uint32_t> want = { // blit.c's sextant plotegcs, but ' '
      0x1fb1e, 0x1fb26, 0x2590, 0x1fb0f, 0x1fb2d, 0x1fb35, 0x1fb37,
      0x1fb13, 0x1fb31, 0x1fb39, 0x1fb3b, 0x258c, 0x1fb32, 0x1fb3a, 0x2588,
    };
    CHECK(want == plot_every_level(n_, NCBLIT_3x2, 4));
  }

  SUBCASE("OctantPlotDrawsItsOwnGlyphs") {
    const std::set<uint32_t> want = { // blit.c's octant plotegcs, but ' '
      0x1cea0, 0x2597, 0x1cd96, 0x1cd91, 0x1cea3, 0x2582, 0x1cdcb, 0x1cdd3,
      0x1cdcd, 0x2596, 0x1cdbb, 0x2584, 0x1cde1, 0x1cddc, 0x1cd48, 0x1cdbf,
      0x1cdde, 0x2586, 0x1cddf, 0x258c, 0x1cdc0, 0x2599, 0x1cde4, 0x1cde0,
    };
    // before L"\0x20" became L" ", this drew a literal x, 2 and 0
    auto drawn = plot_every_level(n_, NCBLIT_4x2, 5);
    CHECK(want == drawn);
    CHECK(0 == drawn.count('x'));
  }

  CHECK(0 == notcurses_stop(nc_));
}
