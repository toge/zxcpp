#include "catch2/catch_all.hpp"

#include <algorithm>
#include <array>
#include <string>
#include <vector>

#include "zxcpp.hpp"

using namespace zxcpp;

TEST_CASE("simple compress/decompress") {
  auto const data = std::string_view("Quick brown fox...");
  auto const src  = std::span{reinterpret_cast<std::uint8_t const*>(data.data()), data.size()};

  if (auto res = zxcpp::compress(src); not res) {
    FAIL("Fail to compress data");
  } else {
    auto const& compressed = res.value();
    if (auto res = zxcpp::decompress(compressed); not res) {
      FAIL("Fail to decompress data");
    } else {
      auto const& decompressed = res.value();

      REQUIRE(std::ranges::equal(src, decompressed));
    }
  }
}

TEST_CASE("simple compress") {
  auto const data = std::string_view("Quick brown fox...");
  auto const src  = std::span{reinterpret_cast<std::uint8_t const*>(data.data()), data.size()};

  if (auto res = zxcpp::compress(src); not res) {
    FAIL("Fail to compress data");
  }
}

TEST_CASE("simple decompress (failure)") {
  auto const& compressed = std::array<std::uint8_t, 11>{"dummy data"};
  if (auto res = zxcpp::decompress(compressed); res) {
    FAIL("Fail to decompress data");
  }
}

TEST_CASE("streaming compressor/decompressor works incrementally") {
  auto data = std::string{};
  for (int i = 0; i < 300; ++i) {
    data += "Quick brown fox jumps over the lazy dog. ";
  }
  auto const src = std::span{reinterpret_cast<std::uint8_t const*>(data.data()), data.size()};

  auto compressor = zxcpp::StreamCompressor{};

  auto compressed_stream = std::vector<std::uint8_t>{};
  auto cbuf = std::array<std::uint8_t, 19>{};
  auto saw_compress_output_full = false;

  auto src_pos = std::size_t{0};
  while (src_pos < src.size()) {
    auto const chunk_size = std::min<std::size_t>(23, src.size() - src_pos);
    auto const in_chunk = src.subspan(src_pos, chunk_size);
    auto input_sent = false;

    while (true) {
      auto const in = input_sent ? std::span<std::uint8_t const>{} : in_chunk;
      auto const res = compressor.update(in, cbuf);
      REQUIRE(res.has_value());

      if (!input_sent) {
        REQUIRE(res->input_consumed == in_chunk.size());
      } else {
        REQUIRE(res->input_consumed == 0);
      }

      src_pos += res->input_consumed;
      input_sent = true;

      compressed_stream.insert(compressed_stream.end(), cbuf.begin(),
                               cbuf.begin() + static_cast<std::ptrdiff_t>(res->output_produced));

      if (res->state == StreamState::OutputBufferFull) {
        saw_compress_output_full = true;
        continue;
      }
      break;
    }
  }

  compressor.finish();
  while (true) {
    auto const res = compressor.update({}, cbuf);
    REQUIRE(res.has_value());
    compressed_stream.insert(compressed_stream.end(), cbuf.begin(),
                             cbuf.begin() + static_cast<std::ptrdiff_t>(res->output_produced));
    if (res->state == StreamState::Completed) {
      break;
    }
    REQUIRE(res->state == StreamState::OutputBufferFull);
  }

  REQUIRE(saw_compress_output_full);

  auto decompressor = zxcpp::StreamDecompressor{};
  auto restored = std::vector<std::uint8_t>{};
  auto dbuf = std::array<std::uint8_t, 17>{};
  auto saw_decompress_output_full = false;

  auto compressed_pos = std::size_t{0};
  while (true) {
    auto in = std::span<std::uint8_t const>{};
    if (compressed_pos < compressed_stream.size()) {
      auto const chunk_size = std::min<std::size_t>(29, compressed_stream.size() - compressed_pos);
      in = std::span<std::uint8_t const>{compressed_stream.data() + compressed_pos, chunk_size};
    }

    auto const res = decompressor.update(in, dbuf);
    REQUIRE(res.has_value());
    REQUIRE(res->input_consumed == in.size());
    compressed_pos += res->input_consumed;

    restored.insert(restored.end(), dbuf.begin(),
                    dbuf.begin() + static_cast<std::ptrdiff_t>(res->output_produced));

    if (res->state == StreamState::OutputBufferFull) {
      saw_decompress_output_full = true;
      continue;
    }

    if (res->state == StreamState::Completed) {
      break;
    }

    if (compressed_pos >= compressed_stream.size() && res->state == StreamState::NeedMoreInput) {
      FAIL("decompressor requests more input after all compressed bytes are consumed");
    }
  }

  REQUIRE(saw_decompress_output_full);
  REQUIRE(std::ranges::equal(src, restored));

  auto const tail = decompressor.update({}, dbuf);
  REQUIRE(tail.has_value());
  REQUIRE(tail->output_produced == 0);
  REQUIRE(tail->state == StreamState::Completed);
}
