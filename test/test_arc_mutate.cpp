#include "catch2/catch_all.hpp"

#include <filesystem>
#include <string>
#include <vector>

#include "zxarc.hpp"

TEST_CASE("arc mutate: add files") {
  auto arc = zxarc::create({{.name = "a.txt", .data = {'a'}}});
  REQUIRE(arc.has_value());

  auto added = zxarc::add(arc.value(), {{.name = "b.txt", .data = {'b', 'b'}}});
  REQUIRE(added.has_value());

  auto entries = zxarc::list(added.value());
  REQUIRE(entries.has_value());
  REQUIRE(entries->size() == 2);

  auto b = zxarc::extract(added.value(), "b.txt");
  REQUIRE(b.has_value());
  REQUIRE(b.value() == std::vector<std::uint8_t>{'b', 'b'});
  // 既存データ不変
  auto a = zxarc::extract(added.value(), "a.txt");
  REQUIRE(a.has_value());
  REQUIRE(a.value() == std::vector<std::uint8_t>{'a'});
}

TEST_CASE("arc mutate: remove files") {
  auto arc = zxarc::create({
      {.name = "a.txt", .data = {'a'}},
      {.name = "b.txt", .data = {'b'}},
      {.name = "c.txt", .data = {'c'}},
  });
  REQUIRE(arc.has_value());

  auto removed = zxarc::remove(arc.value(), {"b.txt"});
  REQUIRE(removed.has_value());

  auto entries = zxarc::list(removed.value());
  REQUIRE(entries.has_value());
  REQUIRE(entries->size() == 2);

  auto missing = zxarc::extract(removed.value(), "b.txt");
  REQUIRE(!missing.has_value());
  REQUIRE(missing.error() == zxarc::Error::EntryNotFound);

  auto a = zxarc::extract(removed.value(), "a.txt");
  REQUIRE(a.has_value());
  REQUIRE(a.value() == std::vector<std::uint8_t>{'a'});
}

TEST_CASE("arc mutate: add replaces same name") {
  auto arc = zxarc::create({{.name = "a.txt", .data = {'1'}}});
  REQUIRE(arc.has_value());
  auto added = zxarc::add(arc.value(), {{.name = "a.txt", .data = {'2'}}});
  REQUIRE(added.has_value());
  auto entries = zxarc::list(added.value());
  REQUIRE(entries.has_value());
  REQUIRE(entries->size() == 1);
  auto a = zxarc::extract(added.value(), "a.txt");
  REQUIRE(a.has_value());
  REQUIRE(a.value() == std::vector<std::uint8_t>{'2'});
}

TEST_CASE("arc file io: save/load/extract_to_file/create_from_files") {
  auto const dir = std::filesystem::temp_directory_path() / "zxarc_test_io";
  std::filesystem::remove_all(dir);
  auto arc = zxarc::create({{.name = "a.txt", .data = {'x', 'y'}}});
  REQUIRE(arc.has_value());

  auto const arc_path = dir / "out.zxa";
  REQUIRE(zxarc::save_archive(arc_path, arc.value()).has_value());
  auto loaded = zxarc::load_archive(arc_path);
  REQUIRE(loaded.has_value());
  REQUIRE(loaded.value() == arc.value());

  auto const out_path = dir / "sub" / "a.txt";
  REQUIRE(zxarc::extract_to_file(loaded.value(), "a.txt", out_path).has_value());
  REQUIRE(std::filesystem::exists(out_path));

  auto rebuilt = zxarc::create_from_files({{out_path, "a.txt"}});
  REQUIRE(rebuilt.has_value());
  auto entries = zxarc::list(rebuilt.value());
  REQUIRE(entries.has_value());
  REQUIRE(entries->size() == 1);
  std::filesystem::remove_all(dir);
}
