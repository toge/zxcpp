#include "catch2/catch_all.hpp"

#include <string>
#include <vector>

#include "zxarc.hpp"

TEST_CASE("arc roundtrip: create->list->extract") {
  auto files = std::vector<zxarc::FileData>{
      {.name = "hello.txt", .data = {'h', 'i'}},
      {.name = "empty.bin", .data = {}},
      {.name = "dir/nested.txt", .data = {'a', 'b', 'c'}},
  };
  auto arc = zxarc::create(files);
  REQUIRE(arc.has_value());

  auto entries = zxarc::list(arc.value());
  REQUIRE(entries.has_value());
  REQUIRE(entries->size() == 3);
  REQUIRE((*entries)[0].name == "hello.txt");
  REQUIRE((*entries)[1].name == "empty.bin");

  auto out = zxarc::extract(arc.value(), "hello.txt");
  REQUIRE(out.has_value());
  REQUIRE(out.value() == std::vector<std::uint8_t>{'h', 'i'});

  auto empty = zxarc::extract(arc.value(), "empty.bin");
  REQUIRE(empty.has_value());
  REQUIRE(empty->empty());

  auto all = zxarc::extract_all(arc.value());
  REQUIRE(all.has_value());
  REQUIRE(all->size() == 3);
  REQUIRE((*all)[2].data == std::vector<std::uint8_t>{'a', 'b', 'c'});
}

TEST_CASE("arc roundtrip: single-frame seekable extraction") {
  // 大きめの先頭ファイルの後のエントリを先頭から全復号なしに取り出せること
  auto big = std::vector<std::uint8_t>(256 * 1024, 'x');
  auto files = std::vector<zxarc::FileData>{
      {.name = "big.bin", .data = big},
      {.name = "tail.txt", .data = {'t', 'a', 'i', 'l'}},
  };
  auto arc = zxarc::create(files, {.level = 1});
  REQUIRE(arc.has_value());
  auto tail = zxarc::extract(arc.value(), "tail.txt");
  REQUIRE(tail.has_value());
  REQUIRE(tail.value() == std::vector<std::uint8_t>{'t', 'a', 'i', 'l'});
}
