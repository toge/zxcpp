#ifndef __ZXCPP_HPP__
#define __ZXCPP_HPP__

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <limits>
#include <ranges>
#include <span>
#include <vector>

extern "C" {
#include "zxc_buffer.h"
#include "zxc_error.h"
#include "zxc_sans_io.h"
}

namespace zxcpp {

/**
 * @brief 1バイト単位の連続したメモリ領域を表すコンセプト
 * @tparam T 判定対象の型
 */
template <typename T>
concept ByteRange = std::ranges::contiguous_range<T> && (sizeof(std::ranges::range_value_t<T>) == 1);

enum class Error : std::uint8_t { CompressionFailed, DecompressionFailed, InvalidBufferSize, ChecksumMismatch };

/**
 * @brief 圧縮後の最大サイズを見積もる
 * @param src_size 入力データサイズ
 * @return 圧縮後の最大サイズ (バイト単位)
 */
[[nodiscard]] inline auto compress_bound(std::size_t const src_size) noexcept -> std::size_t {
  return static_cast<std::size_t>(zxc_compress_bound(src_size));
}

/**
 * @brief データを既存のバッファに圧縮する
 * @param src 圧縮する入力データ
 * @param dst 圧縮結果を書き込むバッファ
 * @param level 圧縮レベル (デフォルト: 3)
 * @param checksum チェックサムを有効にするかどうか (デフォルト: false)
 * @return 書き込まれたデータのサイズ、またはエラー
 */
[[nodiscard]] inline auto compress_into(
    std::span<std::uint8_t const> const src,
    std::span<std::uint8_t> const dst,
    int const level = 3,
    bool const checksum = false) noexcept -> std::expected<std::size_t, Error> {
  if (dst.size() < compress_bound(src.size())) {
    return std::unexpected(Error::InvalidBufferSize);
  }

  auto opt = zxc_compress_opts_t{};
  opt.level = level;
  opt.checksum_enabled = checksum ? 1 : 0;

  auto const result = zxc_compress(src.data(), src.size(), dst.data(), dst.size(), &opt);
  if (result < 0) {
    return std::unexpected(Error::CompressionFailed);
  }

  return static_cast<std::size_t>(result);
}

/**
 * @brief データを既存のバッファに展開する
 * @param src 展開する入力データ
 * @param dst 展開結果を書き込むバッファ
 * @param checksum チェックサムを検証するかどうか (デフォルト: false)
 * @return 展開されたデータのサイズ、またはエラー
 */
[[nodiscard]] inline auto decompress_into(
    std::span<std::uint8_t const> const src,
    std::span<std::uint8_t> const dst,
    bool const checksum = false) noexcept -> std::expected<std::size_t, Error> {
  auto const original_size = zxc_get_decompressed_size(src.data(), src.size());
  if (dst.size() < original_size) {
    return std::unexpected(Error::InvalidBufferSize);
  }

  auto opt = zxc_decompress_opts_t{};
  opt.checksum_enabled = checksum ? 1 : 0;

  auto const result = zxc_decompress(src.data(), src.size(), dst.data(), dst.size(), &opt);
  if (result < 0) {
    return std::unexpected(Error::DecompressionFailed);
  }

  return static_cast<std::size_t>(result);
}

// 圧縮処理
/**
 * @brief データを圧縮する
 * @tparam R 入力データの型 (ByteRangeを満たすこと)
 * @param src 圧縮する入力データ
 * @param level 圧縮レベル (デフォルト: 3)
 * @param checksum チェックサムを有効にするかどうか (デフォルト: false)
 * @return 圧縮されたデータの vector、またはエラー
 */
template <ByteRange R = std::span<std::uint8_t const>>
[[nodiscard]]
inline auto compress(R&& src, int const level = 3, bool const checksum = false) -> std::expected<std::vector<std::uint8_t>, Error> {
  auto const src_data = reinterpret_cast<std::uint8_t const*>(std::ranges::data(src));
  auto const src_size = std::ranges::size(src);
  auto       dst = std::vector<std::uint8_t>(compress_bound(src_size));

  auto const res = compress_into({src_data, src_size}, dst, level, checksum);
  if (!res) {
    return std::unexpected(res.error());
  }

  dst.resize(res.value());
  return dst;
}

// 展開処理
/**
 * @brief データを展開する
 * @tparam R 入力データの型 (ByteRangeを満たすこと)
 * @param src 展開する入力データ
 * @param checksum チェックサムを検証するかどうか (デフォルト: false)
 * @return 展開されたデータの vector、またはエラー
 */
template <ByteRange R = std::span<std::uint8_t const>>
[[nodiscard]]
inline auto decompress(R&& src, bool const checksum = false) -> std::expected<std::vector<std::uint8_t>, Error> {
  auto const src_data = reinterpret_cast<std::uint8_t const*>(std::ranges::data(src));
  auto const src_size = std::ranges::size(src);
  auto const original_size = zxc_get_decompressed_size(src_data, src_size);
  auto dst = std::vector<std::uint8_t>(original_size);

  auto const res = decompress_into({src_data, src_size}, dst, checksum);
  if (!res) {
    return std::unexpected(res.error());
  }

  return dst;
}

enum class StreamState : std::uint8_t { Ok, NeedMoreInput, OutputBufferFull, Completed };

struct StreamResult {
  std::size_t input_consumed = 0;
  std::size_t output_produced = 0;
  StreamState state = StreamState::Ok;
};

namespace detail {

constexpr std::size_t kFrameHeaderSize = sizeof(std::uint32_t) * 2;

inline auto write_u32_le(std::vector<std::uint8_t>& dst, std::uint32_t const value) -> void {
  dst.push_back(static_cast<std::uint8_t>(value & 0xFFu));
  dst.push_back(static_cast<std::uint8_t>((value >> 8u) & 0xFFu));
  dst.push_back(static_cast<std::uint8_t>((value >> 16u) & 0xFFu));
  dst.push_back(static_cast<std::uint8_t>((value >> 24u) & 0xFFu));
}

[[nodiscard]] inline auto read_u32_le(std::span<std::uint8_t const> const src) noexcept -> std::uint32_t {
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

[[nodiscard]] inline auto drain_pending(std::vector<std::uint8_t>& pending,
                                        std::size_t& offset,
                                        std::span<std::uint8_t> const out) noexcept -> std::size_t {
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

/**
 * @brief ストリーム圧縮を行うクラス
 * @details 入力データを一定のチャンクサイズごとに分割し、フレームとして圧縮・エンキューします。
 * @note 現在の zxc ライブラリ(v0.9.1)の公開ヘッダ(zxc_sans_io.h, zxc_buffer.h)には、
 *       状態を維持したまま逐次圧縮を行う zxc_cctx_update() 相当の API が存在しないため、
 *       チャンク単位の one-shot 圧縮を繰り返す実装となっています。
 *       そのため、Options::chunk_size を「フレーム単位の入力分割サイズ」として利用し、
 *       update() に渡された入力がこれを超える場合は分割して処理します。
 */
class StreamCompressor {
public:
  /**
   * @brief 圧縮オプション
   */
  struct Options {
    std::size_t chunk_size = static_cast<std::size_t>(64u) * 1024u; ///< チャンクサイズ (フレーム分割単位)
    int level = 3;                                                 ///< 圧縮レベル (1-5)
    bool checksum = false;                                         ///< チェックサムを有効にするかどうか
  };

  /**
   * @brief デフォルト設定でコンプレッサを構築する
   */
  StreamCompressor()
      : StreamCompressor(Options{}) {}

  /**
   * @brief 指定されたオプションでコンプレッサを構築する
   * @param options 圧縮オプション
   */
  explicit StreamCompressor(Options const& options)
      : chunk_size_{options.chunk_size}, level_{options.level}, checksum_{options.checksum} {
    if (zxc_cctx_init(&ctx_, chunk_size_, 1, level_, checksum_ ? 1 : 0) == 0) {
      initialized_ = true;
    }
  }

  StreamCompressor(StreamCompressor const&) = delete;
  auto operator=(StreamCompressor const&) -> StreamCompressor& = delete;

  StreamCompressor(StreamCompressor&& other) noexcept
      : chunk_size_{other.chunk_size_},
        level_{other.level_},
        checksum_{other.checksum_},
        finishing_{other.finishing_},
        finish_marker_enqueued_{other.finish_marker_enqueued_},
        completed_{other.completed_},
        pending_output_{std::move(other.pending_output_)},
        pending_output_offset_{other.pending_output_offset_},
        ctx_{other.ctx_},
        initialized_{other.initialized_} {
    other.initialized_ = false;
  }

  auto operator=(StreamCompressor&& other) noexcept -> StreamCompressor& {
    if (this == &other) {
      return *this;
    }
    if (initialized_) {
      zxc_cctx_free(&ctx_);
    }
    chunk_size_             = other.chunk_size_;
    level_                  = other.level_;
    checksum_               = other.checksum_;
    finishing_              = other.finishing_;
    finish_marker_enqueued_ = other.finish_marker_enqueued_;
    completed_              = other.completed_;
    pending_output_         = std::move(other.pending_output_);
    pending_output_offset_  = other.pending_output_offset_;
    ctx_                    = other.ctx_;
    initialized_            = other.initialized_;
    other.initialized_      = false;
    return *this;
  }

  ~StreamCompressor() {
    if (initialized_) {
      zxc_cctx_free(&ctx_);
    }
  }

  /**
   * @brief データを追加して圧縮処理を継続する
   * @tparam R 入力データの型 (ByteRangeを満たすこと)
   * @param input 圧縮する入力データ
   * @param output 圧縮結果を書き込むバッファ
   * @return 処理結果 (消費した入力サイズ、生成した出力サイズ、状態)、またはエラー
   * @details 入力が chunk_size を超える場合、chunk_size ごとに複数のフレームに分割して圧縮
   */
  template <ByteRange R = std::span<std::uint8_t const>>
  [[nodiscard]] auto update(R&& input, std::span<std::uint8_t> const output)
      -> std::expected<StreamResult, Error> {
    if (!initialized_) {
      return std::unexpected(Error::CompressionFailed);
    }
    if (completed_) {
      return StreamResult{0, 0, StreamState::Completed};
    }

    auto const input_size = std::ranges::size(input);
    auto const input_data = reinterpret_cast<std::uint8_t const*>(std::ranges::data(input));

    if (finishing_ && input_size > 0) {
      return std::unexpected(Error::CompressionFailed);
    }

    auto consumed = std::size_t{0};

    // 入力がある場合、チャンク単位で分割して圧縮
    if (input_size > 0) {
      while (consumed < input_size) {
        auto const remaining = input_size - consumed;
        auto const current_chunk_size = std::min(remaining, chunk_size_);

        auto const bound64 = compress_bound(current_chunk_size);
        if (bound64 > std::numeric_limits<std::size_t>::max()) {
          return std::unexpected(Error::InvalidBufferSize);
        }
        if (current_chunk_size > std::numeric_limits<std::uint32_t>::max()) {
          return std::unexpected(Error::InvalidBufferSize);
        }

        auto encoded = std::vector<std::uint8_t>(static_cast<std::size_t>(bound64));
        auto opt = zxc_compress_opts_t{};
        opt.level = level_;
        opt.checksum_enabled = checksum_ ? 1 : 0;

        auto const compressed_size = zxc_compress(input_data + consumed, current_chunk_size,
                                                  encoded.data(), encoded.size(), &opt);
        if (compressed_size < 0) {
          return std::unexpected(Error::CompressionFailed);
        }

        encoded.resize(static_cast<std::size_t>(compressed_size));
        detail::append_frame(pending_output_, static_cast<std::uint32_t>(current_chunk_size), encoded);
        consumed += current_chunk_size;
      }
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

  /**
   * @brief 圧縮ストリームを終了させる
   * @details 以降の update() で残りのデータが書き出され、最終的に Completed 状態になります。
   */
  auto finish() noexcept -> void { finishing_ = true; }

  /**
   * @brief 全てのデータの圧縮と出力が完了したかを確認する
   * @return 完了している場合は true
   */
  [[nodiscard]] auto is_completed() const noexcept -> bool { return completed_; }

private:
  std::size_t chunk_size_ = 64u * 1024u;
  int level_ = 3;
  bool checksum_ = false;

  bool finishing_ = false;
  bool finish_marker_enqueued_ = false;
  bool completed_ = false;

  std::vector<std::uint8_t> pending_output_{};
  std::size_t pending_output_offset_ = 0;

  zxc_cctx_t ctx_{};
  bool initialized_ = false;
};

/**
 * @brief ストリーム展開を行うクラス
 * @details フレーム解析を行い、逐次展開します。
 */
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
    if (zxc_cctx_init(&ctx_, options.chunk_size, 0, 3, checksum_ ? 1 : 0) == 0) {
      initialized_ = true;
    }
  }

  StreamDecompressor(StreamDecompressor const&) = delete;
  auto operator=(StreamDecompressor const&) -> StreamDecompressor& = delete;

  StreamDecompressor(StreamDecompressor&& other) noexcept
      : checksum_{other.checksum_},
        completed_{other.completed_},
        input_buffer_{std::move(other.input_buffer_)},
        input_buffer_read_offset_{other.input_buffer_read_offset_},
        pending_output_{std::move(other.pending_output_)},
        pending_output_offset_{other.pending_output_offset_},
        ctx_{other.ctx_},
        initialized_{other.initialized_} {
    other.initialized_ = false;
  }

  auto operator=(StreamDecompressor&& other) noexcept -> StreamDecompressor& {
    if (this == &other) {
      return *this;
    }
    if (initialized_) {
      zxc_cctx_free(&ctx_);
    }
    checksum_                 = other.checksum_;
    completed_                = other.completed_;
    input_buffer_             = std::move(other.input_buffer_);
    input_buffer_read_offset_ = other.input_buffer_read_offset_;
    pending_output_           = std::move(other.pending_output_);
    pending_output_offset_    = other.pending_output_offset_;
    ctx_                      = other.ctx_;
    initialized_              = other.initialized_;
    other.initialized_        = false;
    return *this;
  }

  ~StreamDecompressor() {
    if (initialized_) {
      zxc_cctx_free(&ctx_);
    }
  }

  /**
   * @brief データを追加して展開処理を継続する
   * @tparam R 入力データの型 (ByteRangeを満たすこと)
   * @param input 展開する入力データ
   * @param output 展開結果を書き込むバッファ
   * @return 処理結果 (消費した入力サイズ、生成した出力サイズ、状態)、またはエラー
   */
  template <ByteRange R = std::span<std::uint8_t const>>
  [[nodiscard]] auto update(R&& input, std::span<std::uint8_t> const output)
      -> std::expected<StreamResult, Error> {
    if (!initialized_) {
      return std::unexpected(Error::DecompressionFailed);
    }
    auto const input_size = std::ranges::size(input);
    auto const input_data = reinterpret_cast<std::uint8_t const*>(std::ranges::data(input));

    if (completed_ && input_size > 0) {
      return std::unexpected(Error::DecompressionFailed);
    }

    input_buffer_.insert(input_buffer_.end(), input_data, input_data + input_size);
    auto const consumed = input_size;

    while (pending_output_.empty() && !completed_) {
      auto const buffer_available = input_buffer_.size() - input_buffer_read_offset_;
      if (buffer_available < detail::kFrameHeaderSize) {
        break;
      }

      auto const original_size = detail::read_u32_le(
          std::span{input_buffer_.data() + input_buffer_read_offset_, sizeof(std::uint32_t)});
      auto const compressed_size = detail::read_u32_le(
          std::span{input_buffer_.data() + input_buffer_read_offset_ + sizeof(std::uint32_t), sizeof(std::uint32_t)});

      auto const frame_size = detail::kFrameHeaderSize + static_cast<std::size_t>(compressed_size);
      if (buffer_available < frame_size) {
        break;
      }

      if (original_size == 0 && compressed_size == 0) {
        input_buffer_read_offset_ += detail::kFrameHeaderSize;
        completed_ = true;
        break;
      }

      auto decompressed = std::vector<std::uint8_t>(static_cast<std::size_t>(original_size));
      auto const payload = std::span{input_buffer_.data() + input_buffer_read_offset_ + detail::kFrameHeaderSize,
                                     static_cast<std::size_t>(compressed_size)};

      auto const res = decompress_into(payload, decompressed, checksum_);
      if (!res) {
        return std::unexpected(res.error());
      }

      decompressed.resize(res.value());
      pending_output_ = std::move(decompressed);
      pending_output_offset_ = 0;
      input_buffer_read_offset_ += frame_size;
    }

    shrink_input_buffer();

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

  /**
   * @brief 全てのデータの展開が完了したかを確認する
   * @return 完了している場合は true
   */
  [[nodiscard]] auto is_completed() const noexcept -> bool { return completed_; }

private:
  auto shrink_input_buffer() noexcept -> void {
    if (input_buffer_.empty()) {
      input_buffer_read_offset_ = 0;
      return;
    }

    if (input_buffer_read_offset_ > input_buffer_.size() * kShrinkThresholdPercent / 100) {
      input_buffer_.erase(input_buffer_.begin(),
                          input_buffer_.begin() + static_cast<std::ptrdiff_t>(input_buffer_read_offset_));
      input_buffer_read_offset_ = 0;
    }
  }

  bool checksum_ = false;
  bool completed_ = false;

  static constexpr std::size_t kShrinkThresholdPercent = 50;
  std::vector<std::uint8_t> input_buffer_{};
  std::size_t input_buffer_read_offset_ = 0;
  std::vector<std::uint8_t> pending_output_{};
  std::size_t pending_output_offset_ = 0;

  zxc_cctx_t ctx_{};
  bool initialized_ = false;
};

}  // namespace zxcpp

#endif  // __ZXCPP_HPP__
