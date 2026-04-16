#include "catch2/catch_all.hpp"
#include "zxcpp.hpp"

#include <vector>
#include <string>
#include <algorithm>

using namespace zxcpp;

TEST_CASE("StreamCompressor is movable") {
  auto compressor = StreamCompressor{};
  auto moved_to = std::move(compressor);

  SECTION("Original object is invalidated") {
    std::array<std::uint8_t, 100> out{};
    auto res = compressor.update(std::span<std::uint8_t const>{}, out);
    REQUIRE(!res.has_value());
    REQUIRE(res.error() == Error::CompressionFailed);
  }

  SECTION("Moved-to object works correctly") {
    std::string data = "Movable test data for StreamCompressor";
    auto src = std::span{reinterpret_cast<std::uint8_t const*>(data.data()), data.size()};
    
    std::vector<std::uint8_t> compressed;
    std::array<std::uint8_t, 1024> cbuf{};
    
    auto res = moved_to.update(src, cbuf);
    REQUIRE(res.has_value());
    compressed.insert(compressed.end(), cbuf.begin(), cbuf.begin() + res->output_produced);
    
    moved_to.finish();
    while (!moved_to.is_completed()) {
      auto res_f = moved_to.update({}, cbuf);
      REQUIRE(res_f.has_value());
      compressed.insert(compressed.end(), cbuf.begin(), cbuf.begin() + res_f->output_produced);
      if (res_f->state == StreamState::Completed) break;
    }

    auto decompressor = StreamDecompressor{};
    std::vector<std::uint8_t> restored;
    std::array<std::uint8_t, 1024> dbuf{};
    
    std::size_t pos = 0;
    while (!decompressor.is_completed()) {
      auto in_size = std::min<std::size_t>(compressed.size() - pos, 100);
      auto res_d = decompressor.update(std::span{compressed.data() + pos, in_size}, dbuf);
      REQUIRE(res_d.has_value());
      restored.insert(restored.end(), dbuf.begin(), dbuf.begin() + res_d->output_produced);
      pos += res_d->input_consumed;
      if (res_d->state == StreamState::Completed) break;
    }

    REQUIRE(restored.size() == data.size());
    REQUIRE(std::equal(data.begin(), data.end(), restored.begin()));
  }
}

TEST_CASE("StreamDecompressor is movable") {
  std::string data = "Movable test data for StreamDecompressor";
  std::vector<std::uint8_t> compressed;
  {
    StreamCompressor compressor;
    std::array<std::uint8_t, 1024> cbuf{};
    auto res = compressor.update(std::span{reinterpret_cast<std::uint8_t const*>(data.data()), data.size()}, cbuf);
    REQUIRE(res.has_value());
    compressed.insert(compressed.end(), cbuf.begin(), cbuf.begin() + res->output_produced);
    compressor.finish();
    while (!compressor.is_completed()) {
      auto res_f = compressor.update({}, cbuf);
      REQUIRE(res_f.has_value());
      compressed.insert(compressed.end(), cbuf.begin(), cbuf.begin() + res_f->output_produced);
    }
  }

  auto decompressor = StreamDecompressor{};
  auto moved_to = std::move(decompressor);

  SECTION("Original object is invalidated") {
    std::array<std::uint8_t, 100> out{};
    auto res = decompressor.update(std::span<std::uint8_t const>{}, out);
    REQUIRE(!res.has_value());
    REQUIRE(res.error() == Error::DecompressionFailed);
  }

  SECTION("Moved-to object works correctly") {
    std::array<std::uint8_t, 1024> dbuf{};
    std::vector<std::uint8_t> restored;
    
    auto res = moved_to.update(compressed, dbuf);
    REQUIRE(res.has_value());
    restored.insert(restored.end(), dbuf.begin(), dbuf.begin() + res->output_produced);
    
    while (!moved_to.is_completed()) {
      auto res_f = moved_to.update({}, dbuf);
      REQUIRE(res_f.has_value());
      restored.insert(restored.end(), dbuf.begin(), dbuf.begin() + res_f->output_produced);
      if (res_f->state == StreamState::Completed) break;
    }

    REQUIRE(restored.size() == data.size());
    REQUIRE(std::equal(data.begin(), data.end(), restored.begin()));
  }
}

TEST_CASE("StreamCompressor move assignment") {
  SECTION("Roundtrip check after move assignment") {
    auto compressor = StreamCompressor{};
    auto moved_to = StreamCompressor{};
    moved_to = std::move(compressor);

    std::string data = "Move assignment test data";
    auto src = std::span{reinterpret_cast<std::uint8_t const*>(data.data()), data.size()};
    std::array<std::uint8_t, 1024> cbuf{};
    
    auto res = moved_to.update(src, cbuf);
    REQUIRE(res.has_value());
    
    // Original should be invalidated
    auto res_orig = compressor.update(src, cbuf);
    REQUIRE(!res_orig.has_value());
  }

  SECTION("Self-assignment is safe") {
    auto compressor = StreamCompressor{};
    // Use a pointer or a reference to avoid compiler warnings about self-assignment
    auto* p = &compressor;
    *p = std::move(*p);
    
    std::string data = "Self assignment test data";
    auto src = std::span{reinterpret_cast<std::uint8_t const*>(data.data()), data.size()};
    std::array<std::uint8_t, 1024> cbuf{};
    
    auto res = compressor.update(src, cbuf);
    REQUIRE(res.has_value());
  }
}

TEST_CASE("StreamCompressor in std::vector") {
  std::vector<StreamCompressor> compressors;
  compressors.emplace_back(StreamCompressor::Options{.level = 1});
  compressors.emplace_back(StreamCompressor::Options{.level = 5});
  
  REQUIRE(compressors.size() == 2);
  
  std::string data = "Vector test data";
  std::array<std::uint8_t, 1024> cbuf{};
  
  for (auto& c : compressors) {
    auto res = c.update(std::span{reinterpret_cast<std::uint8_t const*>(data.data()), data.size()}, cbuf);
    REQUIRE(res.has_value());
  }
}
