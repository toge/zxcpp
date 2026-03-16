#ifndef __ZXCPP_HPP__
#define __ZXCPP_HPP__

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <limits>
#include <span>
#include <vector>

extern "C" {
#include "zxc_buffer.h"
#include "zxc_error.h"
#include "zxc_sans_io.h"
}

namespace zxcpp {

enum class Error : std::uint8_t { CompressionFailed, DecompressionFailed, InvalidBufferSize, ChecksumMismatch };

// 圧縮処理
[[nodiscard]]
inline auto compress(std::span<std::uint8_t const> const src, int const level = 3, bool const checksum = false) -> std::expected<std::vector<std::uint8_t>, Error> {
  auto const max_dst_size = zxc_compress_bound(src.size());
  auto       dst = std::vector<uint8_t>(max_dst_size);
  auto const compressed_size = zxc_compress(src.data(), src.size(), dst.data(), dst.size(), level, checksum ? 1 : 0);

  if (compressed_size < 0) {
    return std::unexpected(Error::CompressionFailed);
  }

  dst.resize(compressed_size);
  return dst;
}

[[nodiscard]]
inline auto compress(std::span<std::int8_t const> const src, int const level = 3, bool const checksum = false) {
  auto const byte_span = std::span{reinterpret_cast<std::uint8_t const*>(src.data()), src.size()};
  return compress(byte_span, level, checksum);
}

[[nodiscard]]
inline auto compress(std::span<std::byte const> const src, int const level = 3, bool const checksum = false) {
  auto const byte_span = std::span{reinterpret_cast<std::uint8_t const*>(src.data()), src.size()};
  return compress(byte_span, level, checksum);
}

// 展開処理
[[nodiscard]]
inline auto decompress(std::span<std::uint8_t const> const src, bool checksum = false) -> std::expected<std::vector<std::uint8_t>, Error> {
  auto const original_size = zxc_get_decompressed_size(src.data(), src.size());
  auto dst = std::vector<std::uint8_t>(original_size);

  auto const result = zxc_decompress(src.data(), src.size(), dst.data(), dst.size(), checksum ? 1 : 0);

  if (result < 0) {
    return std::unexpected(Error::DecompressionFailed);
  }

  return dst;
}

[[nodiscard]]
inline auto decompress(std::span<std::int8_t const> const src, bool checksum = false) {
  auto const byte_span = std::span{reinterpret_cast<std::uint8_t const*>(src.data()), src.size()};
  return decompress(byte_span, checksum);
}

[[nodiscard]]
inline auto decompress(std::span<std::byte const> const src, bool checksum = false) {
  auto const byte_span = std::span{reinterpret_cast<std::uint8_t const*>(src.data()), src.size()};
  return decompress(byte_span, checksum);
}

enum class StreamState : std::uint8_t { Ok, NeedMoreInput, OutputBufferFull, Completed };

struct StreamResult {
  std::size_t input_consumed = 0;
  std::size_t output_produced = 0;
  StreamState state = StreamState::Ok;
};

namespace detail {

constexpr std::size_t kFrameHeaderSize = sizeof(std::uint32_t) * 2;

inline auto write_u32_le(std::vector<std::uint8_t>& dst, std::uint32_t value) -> void {
  dst.push_back(static_cast<std::uint8_t>(value & 0xFFu));
  dst.push_back(static_cast<std::uint8_t>((value >> 8u) & 0xFFu));
  dst.push_back(static_cast<std::uint8_t>((value >> 16u) & 0xFFu));
  dst.push_back(static_cast<std::uint8_t>((value >> 24u) & 0xFFu));
}

[[nodiscard]] inline auto read_u32_le(std::span<std::uint8_t const> src) -> std::uint32_t {
  return static_cast<std::uint32_t>(src[0]) |
         (static_cast<std::uint32_t>(src[1]) << 8u) |
         (static_cast<std::uint32_t>(src[2]) << 16u) |
         (static_cast<std::uint32_t>(src[3]) << 24u);
}

inline auto append_frame(std::vector<std::uint8_t>& dst,
                         std::uint32_t const original_size,
                         std::span<std::uint8_t const> const payload) -> void {
  dst.reserve(dst.size() + kFrameHeaderSize + payload.size());
  write_u32_le(dst, original_size);
  write_u32_le(dst, static_cast<std::uint32_t>(payload.size()));
  dst.insert(dst.end(), payload.begin(), payload.end());
}

inline auto erase_prefix(std::vector<std::uint8_t>& buffer, std::size_t const n) -> void {
  if (n == 0) {
    return;
  }
  if (n >= buffer.size()) {
    buffer.clear();
    return;
  }
  buffer.erase(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(n));
}

[[nodiscard]] inline auto drain_pending(std::vector<std::uint8_t>& pending,
                                        std::size_t& offset,
                                        std::span<std::uint8_t> const out) -> std::size_t {
  auto const available = pending.size() - offset;
  auto const writable = std::min(available, out.size());
  if (writable != 0) {
    std::copy_n(pending.data() + static_cast<std::ptrdiff_t>(offset), writable, out.data());
    offset += writable;
  }

  if (offset == pending.size()) {
    pending.clear();
    offset = 0;
  }

  return writable;
}

}  // namespace detail

class StreamCompressor {
public:
  struct Options {
    std::size_t chunk_size = static_cast<std::size_t>(64u) * 1024u;
    int level = 3;
    bool checksum = false;
  };

  StreamCompressor()
      : StreamCompressor(Options{}) {}

  explicit StreamCompressor(Options const& options)
      : level_{options.level}, checksum_{options.checksum} {
    initialized_ = zxc_cctx_init(&ctx_, options.chunk_size, 1, level_, checksum_ ? 1 : 0) == ZXC_OK;
  }

  StreamCompressor(StreamCompressor const&) = delete;
  auto operator=(StreamCompressor const&) -> StreamCompressor& = delete;
  StreamCompressor(StreamCompressor&&) = delete;
  auto operator=(StreamCompressor&&) -> StreamCompressor& = delete;

  ~StreamCompressor() {
    if (initialized_) {
      zxc_cctx_free(&ctx_);
    }
  }

  [[nodiscard]] auto update(std::span<std::uint8_t const> const input,
                            std::span<std::uint8_t> const output)
      -> std::expected<StreamResult, Error> {
    if (!initialized_) {
      return std::unexpected(Error::CompressionFailed);
    }

    if (completed_) {
      return StreamResult{0, 0, StreamState::Completed};
    }

    if (finishing_ && !input.empty()) {
      return std::unexpected(Error::CompressionFailed);
    }

    auto consumed = std::size_t{0};

    if (!input.empty()) {
      auto const bound64 = zxc_compress_bound(input.size());
      if (bound64 > std::numeric_limits<std::size_t>::max()) {
        return std::unexpected(Error::InvalidBufferSize);
      }
      if (input.size() > std::numeric_limits<std::uint32_t>::max()) {
        return std::unexpected(Error::InvalidBufferSize);
      }

      auto encoded = std::vector<std::uint8_t>(static_cast<std::size_t>(bound64));
      auto const compressed_size = zxc_compress(input.data(), input.size(), encoded.data(),
                                                encoded.size(), level_, checksum_ ? 1 : 0);
      if (compressed_size < 0) {
        return std::unexpected(Error::CompressionFailed);
      }

      auto const compressed_u64 = static_cast<std::uint64_t>(compressed_size);
      if (compressed_u64 > std::numeric_limits<std::uint32_t>::max()) {
        return std::unexpected(Error::InvalidBufferSize);
      }

      encoded.resize(static_cast<std::size_t>(compressed_size));
      detail::append_frame(pending_output_, static_cast<std::uint32_t>(input.size()), encoded);
      consumed = input.size();
    }

    if (finishing_ && !finish_marker_enqueued_) {
      detail::append_frame(pending_output_, 0, std::span<std::uint8_t const>{});
      finish_marker_enqueued_ = true;
    }

    auto const produced = detail::drain_pending(pending_output_, pending_output_offset_, output);

    if (!pending_output_.empty()) {
      return StreamResult{consumed, produced, StreamState::OutputBufferFull};
    }

    if (finishing_ && finish_marker_enqueued_) {
      completed_ = true;
      return StreamResult{consumed, produced, StreamState::Completed};
    }

    if (consumed == 0 && produced == 0) {
      return StreamResult{0, 0, StreamState::NeedMoreInput};
    }

    return StreamResult{consumed, produced, StreamState::Ok};
  }

  auto finish() -> void { finishing_ = true; }

  [[nodiscard]] auto is_completed() const -> bool { return completed_; }

private:
  zxc_cctx_t ctx_{};
  bool initialized_ = false;
  int level_ = 3;
  bool checksum_ = false;

  bool finishing_ = false;
  bool finish_marker_enqueued_ = false;
  bool completed_ = false;

  std::vector<std::uint8_t> pending_output_{};
  std::size_t pending_output_offset_ = 0;
};

class StreamDecompressor {
public:
  struct Options {
    std::size_t chunk_size = static_cast<std::size_t>(64u) * 1024u;
    bool checksum = false;
  };

  StreamDecompressor()
      : StreamDecompressor(Options{}) {}

  explicit StreamDecompressor(Options const& options)
      : checksum_{options.checksum} {
    initialized_ = zxc_cctx_init(&ctx_, options.chunk_size, 0, 3, checksum_ ? 1 : 0) == ZXC_OK;
  }

  StreamDecompressor(StreamDecompressor const&) = delete;
  auto operator=(StreamDecompressor const&) -> StreamDecompressor& = delete;
  StreamDecompressor(StreamDecompressor&&) = delete;
  auto operator=(StreamDecompressor&&) -> StreamDecompressor& = delete;

  ~StreamDecompressor() {
    if (initialized_) {
      zxc_cctx_free(&ctx_);
    }
  }

  [[nodiscard]] auto update(std::span<std::uint8_t const> const input,
                            std::span<std::uint8_t> const output)
      -> std::expected<StreamResult, Error> {
    if (!initialized_) {
      return std::unexpected(Error::DecompressionFailed);
    }

    if (completed_ && !input.empty()) {
      return std::unexpected(Error::DecompressionFailed);
    }

    input_buffer_.insert(input_buffer_.end(), input.begin(), input.end());
    auto const consumed = input.size();

    while (pending_output_.empty() && !completed_) {
      if (input_buffer_.size() < detail::kFrameHeaderSize) {
        break;
      }

      auto const src = std::span<std::uint8_t const>{input_buffer_};
      auto const original_size = detail::read_u32_le(src.subspan(0, sizeof(std::uint32_t)));
      auto const compressed_size = detail::read_u32_le(
          src.subspan(sizeof(std::uint32_t), sizeof(std::uint32_t)));

      auto const frame_size = detail::kFrameHeaderSize + static_cast<std::size_t>(compressed_size);
      if (input_buffer_.size() < frame_size) {
        break;
      }

      if (original_size == 0 && compressed_size == 0) {
        detail::erase_prefix(input_buffer_, detail::kFrameHeaderSize);
        completed_ = true;
        break;
      }

      auto decompressed = std::vector<std::uint8_t>(static_cast<std::size_t>(original_size));
      auto const payload = src.subspan(detail::kFrameHeaderSize, static_cast<std::size_t>(compressed_size));

      auto const ret = zxc_decompress(payload.data(), payload.size(), decompressed.data(),
                                      decompressed.size(), checksum_ ? 1 : 0);
      if (ret < 0) {
        return std::unexpected(Error::DecompressionFailed);
      }

      decompressed.resize(static_cast<std::size_t>(ret));
      pending_output_ = std::move(decompressed);
      pending_output_offset_ = 0;
      detail::erase_prefix(input_buffer_, frame_size);
    }

    auto const produced = detail::drain_pending(pending_output_, pending_output_offset_, output);

    if (!pending_output_.empty()) {
      return StreamResult{consumed, produced, StreamState::OutputBufferFull};
    }

    if (completed_) {
      return StreamResult{consumed, produced, StreamState::Completed};
    }

    if (produced == 0) {
      return StreamResult{consumed, 0, StreamState::NeedMoreInput};
    }

    return StreamResult{consumed, produced, StreamState::Ok};
  }

  [[nodiscard]] auto is_completed() const -> bool { return completed_; }

private:
  zxc_cctx_t ctx_{};
  bool initialized_ = false;
  bool checksum_ = false;
  bool completed_ = false;

  std::vector<std::uint8_t> input_buffer_{};
  std::vector<std::uint8_t> pending_output_{};
  std::size_t pending_output_offset_ = 0;
};

}  // namespace zxcpp

#endif  // __ZXCPP_HPP__
