#include "main.h"
#include "lib/cellgeometry.h"
#include <string>

TEST_CASE("CellGeometryReports") {
  unsigned kind = 99, y = 98, x = 97;
  auto parse = [&](const std::string& s) {
    return cellgeometry_parse(reinterpret_cast<const unsigned char*>(s.data()),
                              s.size(), &kind, &y, &x);
  };
  CHECK(parse("\x1b[6;32;14t"));
  CHECK(kind == 6); CHECK(y == 32); CHECK(x == 14);
  CHECK(parse("\x1b[4;768;1120t"));
  CHECK(kind == 4); CHECK(y == 768); CHECK(x == 1120);
  for(const auto& s : {"\x1b[6;0;14t", "\x1b[6;32;0t", "\x1b[6;;14t",
       "\x1b[6;4294967328;14t", "\x1b[6;65536;14t", "\x1b[6;32;14",
       "\x1b[6;32;14tx", "\x1b[6;-32;14t", "\x1b[6;32;14;2t"}) {
    CHECK_FALSE(parse(s));
    CHECK(kind == 4); CHECK(y == 768); CHECK(x == 1120);
  }
  const std::string reply = "\x1b[6;32;14t";
  for(size_t len = 0; len < reply.size(); ++len) {
    CHECK_FALSE(parse(reply.substr(0, len)));
  }
}

TEST_CASE("CellGeometryQueryBudget") {
  cellgeometry g{};
  CHECK(cellgeometry_query_due(&g, 0) == 1);
  CHECK(cellgeometry_query_due(&g, 100000000) == 0);
  cellgeometry_observe(&g, 32, 14);
  CHECK(g.y == 32); CHECK(g.x == 14);
  CHECK(cellgeometry_query_due(&g, 249999999) == 0);
  CHECK(cellgeometry_query_due(&g, 250000000) == 1);
  CHECK(cellgeometry_query_due(&g, 1249999999) == 0);
  CHECK(cellgeometry_query_due(&g, 1250000000) == 1);
  CHECK(cellgeometry_query_due(&g, 2250000000) == 1);
  CHECK(cellgeometry_query_due(&g, 3250000000) == 2);
  CHECK(g.y == 32); CHECK(g.x == 14);
  CHECK(cellgeometry_query_due(&g, 33249999999) == 0);
  CHECK(cellgeometry_query_due(&g, 33250000000) == 1);
  CHECK(cellgeometry_query_due(&g, 34250000000) == 0);
  cellgeometry_observe(&g, 40, 18); // late recovery is accepted
  CHECK(g.y == 40); CHECK(g.x == 18); CHECK(g.timeouts == 0);
  CHECK(cellgeometry_query_due(&g, 34500000000) == 1);
}
