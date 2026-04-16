#include "catch2/catch_all.hpp"
#include "zxcpp.hpp"
#include <string>
#include <string_view>
#include <vector>
#include <array>
#include <span>

using namespace zxcpp;

TEST_CASE("compress accepts string_view") {
    std::string_view sv = "Hello, Concepts!";
    auto res = compress(sv);
    REQUIRE(res.has_value());
}

TEST_CASE("compress accepts std::string") {
    std::string s = "Hello, Concepts!";
    auto res = compress(s);
    REQUIRE(res.has_value());
}

TEST_CASE("compress accepts vector<char>") {
    std::vector<char> v = {'H', 'e', 'l', 'l', 'o'};
    auto res = compress(v);
    REQUIRE(res.has_value());
}

TEST_CASE("compress accepts vector<std::byte>") {
    std::vector<std::byte> v = {std::byte{'H'}, std::byte{'e'}};
    auto res = compress(v);
    REQUIRE(res.has_value());
}

TEST_CASE("compress accepts array<uint8_t, 5>") {
    std::array<std::uint8_t, 5> a = {1, 2, 3, 4, 5};
    auto res = compress(a);
    REQUIRE(res.has_value());
}

TEST_CASE("compress/decompress roundtrip with string_view") {
    std::string_view sv = "Roundtrip test with string_view";
    auto compressed = compress(sv);
    REQUIRE(compressed.has_value());
    
    auto decompressed = decompress(compressed.value());
    REQUIRE(decompressed.has_value());
    
    std::string_view result(reinterpret_cast<char const*>(decompressed.value().data()), decompressed.value().size());
    REQUIRE(sv == result);
}

TEST_CASE("StreamCompressor::update accepts string_view") {
    StreamCompressor compressor;
    std::string_view sv = "Streaming with string_view";
    std::uint8_t out_buf[1024];
    std::span<std::uint8_t> output{out_buf};
    
    auto res = compressor.update(sv, output);
    REQUIRE(res.has_value());
    REQUIRE(res->input_consumed == sv.size());
}
