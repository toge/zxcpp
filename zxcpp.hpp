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
#include <type_traits>
#include <vector>

extern "C" {
#include "zxc_buffer.h"
#include "zxc_error.h"
#include "zxc_opts.h"
#include "zxc_pstream.h"
}

namespace zxcpp {

/**
 * @brief 1バイト単位の連続したメモリ領域を表すコンセプト
 * @tparam T 判定対象の型
 */
template <typename T>
concept ByteRange = std::ranges::contiguous_range<T>
    && std::is_trivially_copyable_v<std::ranges::range_value_t<T>>
    && sizeof(std::ranges::range_value_t<T>) == 1;

static_assert(ByteRange<std::string_view>);
static_assert(ByteRange<std::string>);
static_assert(ByteRange<std::vector<std::uint8_t>>);
static_assert(ByteRange<std::span<std::byte const>>);
static_assert(!ByteRange<std::vector<int>>);
static_assert(!ByteRange<std::vector<std::uint16_t>>);

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
[[nodiscard]] inline auto compress(R const& src, int const level = 3, bool const checksum = false)
    -> std::expected<std::vector<std::uint8_t>, Error> {
  auto const src_data = reinterpret_cast<std::uint8_t const*>(std::ranges::data(src));
  auto const src_size = std::ranges::size(src);
  auto       dst      = std::vector<std::uint8_t>(compress_bound(src_size));

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
[[nodiscard]] inline auto decompress(R const& src, bool const checksum = false)
    -> std::expected<std::vector<std::uint8_t>, Error> {
  auto const src_data      = reinterpret_cast<std::uint8_t const*>(std::ranges::data(src));
  auto const src_size      = std::ranges::size(src);
  auto const original_size = zxc_get_decompressed_size(src_data, src_size);
  auto       dst           = std::vector<std::uint8_t>(original_size);

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

/**
 * @brief ストリーム圧縮を行うクラス
 * @details zxc の push streaming API (zxc_cstream) を使用して、
 *          入力データをチャンク単位で圧縮します。
 */
class StreamCompressor {
public:
  /**
   * @brief 圧縮オプション
   */
  struct Options {
    std::size_t chunk_size = static_cast<std::size_t>(4u) * 1024u; ///< チャンクサイズ (ブロックサイズ)
    int level = 3;                                                 ///< 圧縮レベル (1-6)
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
    auto opt = zxc_compress_opts_t{};
    opt.block_size = chunk_size_;
    opt.level = level_;
    opt.checksum_enabled = checksum_ ? 1 : 0;
    cs_ = zxc_cstream_create(&opt);
  }

  StreamCompressor(StreamCompressor const&) = delete;
  auto operator=(StreamCompressor const&) -> StreamCompressor& = delete;

  StreamCompressor(StreamCompressor&& other) noexcept
      : chunk_size_{other.chunk_size_},
        level_{other.level_},
        checksum_{other.checksum_},
        finishing_{other.finishing_},
        completed_{other.completed_},
        pending_output_{std::move(other.pending_output_)},
        pending_output_offset_{other.pending_output_offset_},
        cs_{other.cs_} {
    other.cs_ = nullptr;
  }

  auto operator=(StreamCompressor&& other) noexcept -> StreamCompressor& {
    if (this == &other) {
      return *this;
    }
    zxc_cstream_free(cs_);
    chunk_size_             = other.chunk_size_;
    level_                  = other.level_;
    checksum_               = other.checksum_;
    finishing_              = other.finishing_;
    completed_              = other.completed_;
    pending_output_         = std::move(other.pending_output_);
    pending_output_offset_  = other.pending_output_offset_;
    cs_                     = other.cs_;
    other.cs_               = nullptr;
    return *this;
  }

  ~StreamCompressor() {
    zxc_cstream_free(cs_);
  }

  /**
   * @brief データを追加して圧縮処理を継続する
   * @tparam R 入力データの型 (ByteRangeを満たすこと)
   * @param input 圧縮する入力データ
   * @param output 圧縮結果を書き込むバッファ
   * @return 処理結果 (消費した入力サイズ、生成した出力サイズ、状態)、またはエラー
   */
  template <ByteRange R = std::span<std::uint8_t const>>
  [[nodiscard]] auto update(R const& input, std::span<std::uint8_t> const output)
      -> std::expected<StreamResult, Error> {
    if (cs_ == nullptr) {
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

    // pending がある場合は drain する
    auto drained = std::size_t{0};
    if (!pending_output_.empty()) {
      drained = drain_pending(output);
      if (drained == output.size()) {
        return StreamResult{0, drained, StreamState::OutputBufferFull};
      }
    }

    // 出力バッファに余裕があれば新しい入力を処理
    auto consumed = std::size_t{0};
    if (input_size > 0) {
      auto out_sub = output.subspan(drained);
      auto in = zxc_inbuf_t{input_data, input_size, 0};
      auto out = zxc_outbuf_t{out_sub.data(), out_sub.size(), 0};

      auto const result = zxc_cstream_compress(cs_, &out, &in);
      if (result < 0) {
        return std::unexpected(Error::CompressionFailed);
      }

      consumed = in.pos;

      if (result > 0) {
        // 出力バッファが满了、新しいデータを pending に保存
        // caller には drain した分だけ返す (新しいデータは次回の drain で返す)
        pending_output_.assign(out_sub.data(), out_sub.data() + out.pos);
        pending_output_offset_ = 0;
        return StreamResult{consumed, drained, StreamState::OutputBufferFull};
      }

      return StreamResult{consumed, drained + out.pos, StreamState::Ok};
    }

    if (finishing_ && !completed_) {
      auto out_sub = output.subspan(drained);
      auto out = zxc_outbuf_t{out_sub.data(), out_sub.size(), 0};

      auto const result = zxc_cstream_end(cs_, &out);
      if (result < 0) {
        return std::unexpected(Error::CompressionFailed);
      }

      if (result > 0) {
        pending_output_.assign(out_sub.data(), out_sub.data() + out.pos);
        pending_output_offset_ = 0;
        return StreamResult{0, drained, StreamState::OutputBufferFull};
      }

      completed_ = true;
      auto const total = drained + out.pos;
      if (total > 0) {
        return StreamResult{0, total, StreamState::Ok};
      }
      return StreamResult{0, 0, StreamState::Completed};
    }

    if (drained > 0) {
      return StreamResult{0, drained, StreamState::Ok};
    }

    if (finishing_) {
      completed_ = true;
      return StreamResult{0, 0, StreamState::Completed};
    }

    return StreamResult{0, 0, StreamState::NeedMoreInput};
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
  auto drain_pending(std::span<std::uint8_t> const out) noexcept -> std::size_t {
    auto const available = pending_output_.size() - pending_output_offset_;
    auto const writable = std::min(available, out.size());
    if (writable != 0) {
      std::copy_n(pending_output_.data() + static_cast<std::ptrdiff_t>(pending_output_offset_), writable, out.data());
      pending_output_offset_ += writable;
    }

    if (pending_output_offset_ == pending_output_.size()) {
      pending_output_.clear();
      pending_output_offset_ = 0;
    }

    return writable;
  }

  std::size_t chunk_size_ = 4u * 1024u;
  int level_ = 3;
  bool checksum_ = false;

  bool finishing_ = false;
  bool completed_ = false;

  std::vector<std::uint8_t> pending_output_{};
  std::size_t pending_output_offset_ = 0;

  zxc_cstream* cs_ = nullptr;
};

/**
 * @brief ストリーム展開を行うクラス
 * @details zxc の push streaming API (zxc_dstream) を使用して、
 *          圧縮データを逐次展開します。
 */
class StreamDecompressor {
public:
  struct Options {
    std::size_t chunk_size = static_cast<std::size_t>(4u) * 1024u;
    bool checksum = false;
  };

  StreamDecompressor()
      : StreamDecompressor(Options{}) {}

  explicit StreamDecompressor(Options const& options)
      : checksum_{options.checksum} {
    auto opt = zxc_decompress_opts_t{};
    opt.checksum_enabled = checksum_ ? 1 : 0;
    ds_ = zxc_dstream_create(&opt);
  }

  StreamDecompressor(StreamDecompressor const&) = delete;
  auto operator=(StreamDecompressor const&) -> StreamDecompressor& = delete;

  StreamDecompressor(StreamDecompressor&& other) noexcept
      : checksum_{other.checksum_},
        completed_{other.completed_},
        pending_output_{std::move(other.pending_output_)},
        pending_output_offset_{other.pending_output_offset_},
        ds_{other.ds_} {
    other.ds_ = nullptr;
  }

  auto operator=(StreamDecompressor&& other) noexcept -> StreamDecompressor& {
    if (this == &other) {
      return *this;
    }
    zxc_dstream_free(ds_);
    checksum_                = other.checksum_;
    completed_               = other.completed_;
    pending_output_          = std::move(other.pending_output_);
    pending_output_offset_   = other.pending_output_offset_;
    ds_                      = other.ds_;
    other.ds_                = nullptr;
    return *this;
  }

  ~StreamDecompressor() {
    zxc_dstream_free(ds_);
  }

  /**
   * @brief データを追加して展開処理を継続する
   * @tparam R 入力データの型 (ByteRangeを満たすこと)
   * @param input 展開する入力データ
   * @param output 展開結果を書き込むバッファ
   * @return 処理結果 (消費した入力サイズ、生成した出力サイズ、状態)、またはエラー
   */
  template <ByteRange R = std::span<std::uint8_t const>>
  [[nodiscard]] auto update(R const& input, std::span<std::uint8_t> const output)
      -> std::expected<StreamResult, Error> {
    if (ds_ == nullptr) {
      return std::unexpected(Error::DecompressionFailed);
    }

    auto const input_size = std::ranges::size(input);
    auto const input_data = reinterpret_cast<std::uint8_t const*>(std::ranges::data(input));

    if (completed_ && input_size > 0) {
      return std::unexpected(Error::DecompressionFailed);
    }

    if (completed_) {
      return StreamResult{0, 0, StreamState::Completed};
    }

    // pending がある場合は drain する
    auto drained = std::size_t{0};
    if (!pending_output_.empty()) {
      drained = drain_pending(output);
      if (drained == output.size()) {
        return StreamResult{0, drained, StreamState::OutputBufferFull};
      }
    }

    // 出力バッファに余裕があれば新しい入力を処理
    auto consumed = std::size_t{0};
    if (input_size > 0) {
      auto out_sub = output.subspan(drained);
      auto in = zxc_inbuf_t{input_data, input_size, 0};
      auto out = zxc_outbuf_t{out_sub.data(), out_sub.size(), 0};

      auto const result = zxc_dstream_decompress(ds_, &out, &in);
      if (result < 0) {
        return std::unexpected(Error::DecompressionFailed);
      }

      consumed = in.pos;

      if (zxc_dstream_finished(ds_)) {
        completed_ = true;
        return StreamResult{consumed, drained + out.pos, StreamState::Completed};
      }

      if (result > 0) {
        pending_output_.assign(out_sub.data(), out_sub.data() + out.pos);
        pending_output_offset_ = 0;
        return StreamResult{consumed, drained, StreamState::OutputBufferFull};
      }

      if (consumed > 0 || out.pos > 0 || drained > 0) {
        return StreamResult{consumed, drained + out.pos, StreamState::Ok};
      }

      return StreamResult{0, 0, StreamState::NeedMoreInput};
    }

    if (drained > 0) {
      return StreamResult{0, drained, StreamState::Ok};
    }

    return StreamResult{0, 0, StreamState::NeedMoreInput};
  }

  /**
   * @brief 全てのデータの展開が完了したかを確認する
   * @return 完了している場合は true
   */
  [[nodiscard]] auto is_completed() const noexcept -> bool { return completed_; }

private:
  auto drain_pending(std::span<std::uint8_t> const out) noexcept -> std::size_t {
    auto const available = pending_output_.size() - pending_output_offset_;
    auto const writable = std::min(available, out.size());
    if (writable != 0) {
      std::copy_n(pending_output_.data() + static_cast<std::ptrdiff_t>(pending_output_offset_), writable, out.data());
      pending_output_offset_ += writable;
    }

    if (pending_output_offset_ == pending_output_.size()) {
      pending_output_.clear();
      pending_output_offset_ = 0;
    }

    return writable;
  }

  bool checksum_ = false;
  bool completed_ = false;

  std::vector<std::uint8_t> pending_output_{};
  std::size_t pending_output_offset_ = 0;

  zxc_dstream* ds_ = nullptr;
};

}  // namespace zxcpp

#endif  // __ZXCPP_HPP__
