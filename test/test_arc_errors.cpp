#include "catch2/catch_all.hpp"

#include <string>
#include <vector>

#include "zxarc.hpp"

TEST_CASE("arc errors: empty archive roundtrip") {
  auto arc = zxarc::create({});
  REQUIRE(arc.has_value());
  auto entries = zxarc::list(arc.value());
  REQUIRE(entries.has_value());
  REQUIRE(entries->empty());
}

TEST_CASE("arc errors: unknown entry") {
  auto arc = zxarc::create({{.name = "a.txt", .data = {'a'}}});
  REQUIRE(arc.has_value());
  auto out = zxarc::extract(arc.value(), "nope.txt");
  REQUIRE(!out.has_value());
  REQUIRE(out.error() == zxarc::Error::EntryNotFound);
}

TEST_CASE("arc errors: truncated footer") {
  auto arc = zxarc::create({{.name = "a.txt", .data = {'a'}}});
  REQUIRE(arc.has_value());
  auto bad = arc.value();
  bad.resize(bad.size() / 2); // 末尾破壊
  auto entries = zxarc::list(bad);
  REQUIRE(!entries.has_value());
}

TEST_CASE("arc errors: garbage input") {
  auto bad = std::vector<std::uint8_t>{1, 2, 3, 4, 5};
  REQUIRE(!zxarc::list(bad).has_value());
  REQUIRE(!zxarc::extract(bad, "a").has_value());
}

TEST_CASE("arc errors: duplicate names rejected") {
  auto arc = zxarc::create({
      {.name = "a.txt", .data = {'1'}},
      {.name = "a.txt", .data = {'2'}},
  });
  REQUIRE(!arc.has_value());
  REQUIRE(arc.error() == zxarc::Error::DuplicateEntry);
}
