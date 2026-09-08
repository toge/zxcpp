#ifndef __ZXARC_HPP__
#define __ZXARC_HPP__

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <filesystem>
#include <fstream>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

extern "C" {
#include "zxc_buffer.h"
#include "zxc_error.h"
#include "zxc_opts.h"
#include "zxc_seekable.h"
}

namespace zxarc {

/**
 * @brief 1バイト単位の連続したメモリ領域を表すコンセプト
 */
template <typename T>
concept ByteRange = std::ranges::contiguous_range<T>
    && std::is_trivially_copyable_v<std::ranges::range_value_t<T>>
    && sizeof(std::ranges::range_value_t<T>) == 1;

/**
 * @brief アーカイバのエラー種別
 */
enum class Error : std::uint8_t {
  CompressionFailed,   ///< 圧縮に失敗した
  DecompressionFailed, ///< 展開に失敗した
  BadArchive,          ///< アーカイブ形式が不正
  BadFooter,           ///< フッタの検証に失敗した
  EntryNotFound,       ///< 指定名のエントリが存在しない
  DuplicateEntry,      ///< 同名エントリが重複している
  InvalidArgument,     ///< 引数が不正 (空名・長すぎる名前等)
  IoError,             ///< ファイル入出力に失敗した
  SeekableError,       ///< seekable ハンドルの open/range に失敗した
  CrcMismatch,         ///< エントリ CRC が不一致
};

/**
 * @brief エラー種別を文字列化する
 * @param e エラー種別
 * @return 人間可読な文字列
 */
[[nodiscard]] inline auto error_name(Error const e) noexcept -> std::string_view {
  switch (e) {
    case Error::CompressionFailed: return "CompressionFailed";
    case Error::DecompressionFailed: return "DecompressionFailed";
    case Error::BadArchive: return "BadArchive";
    case Error::BadFooter: return "BadFooter";
    case Error::EntryNotFound: return "EntryNotFound";
    case Error::DuplicateEntry: return "DuplicateEntry";
    case Error::InvalidArgument: return "InvalidArgument";
    case Error::IoError: return "IoError";
    case Error::SeekableError: return "SeekableError";
    case Error::CrcMismatch: return "CrcMismatch";
  }
  return "Unknown";
}

/**
 * @brief zxc の生エラーコードを文字列化する
 * @param code zxc が返した負のエラーコード
 * @return zxc_error_name() の結果
 */
[[nodiscard]] inline auto zxc_error_str(int const code) noexcept -> std::string_view {
  auto const* p = zxc_error_name(code);
  return p != nullptr ? std::string_view{p} : std::string_view{"ZXC_UNKNOWN_ERROR"};
}

/**
 * @brief 作成オプション
 */
struct Options {
  int level = 3;             ///< 圧縮レベル (1-7)
  bool checksum = true;      ///< zxc チェックサムを有効化
  std::size_t block_size = 0; ///< zxc ブロックサイズ (0=既定512KB、2の冪4KB-2MB)
};

/**
 * @brief 格納する1ファイル分のデータ
 */
struct FileData {
  std::string name;                 ///< アーカイブ内名 (UTF-8、ディレクトリは末尾'/')
  std::vector<std::uint8_t> data;   ///< 生バイト列
  std::uint64_t mtime = 0;          ///< 更新時刻 (任意、0=未設定)
  std::uint32_t mode = 0644;        ///< パーミッション (任意)
};

/**
 * @brief エントリ情報 (list の戻り値)
 */
struct EntryInfo {
  std::string name;          ///< アーカイブ内名
  std::uint64_t offset = 0;  ///< 連結生データ中のオフセット
  std::uint64_t size = 0;    ///< 生サイズ
  std::uint32_t crc = 0;     ///< 生データの CRC32 (IEEE)
  std::uint64_t mtime = 0;   ///< 更新時刻
  std::uint32_t mode = 0644; ///< パーミッション
};

// ponytail: 自前 CRC32 (IEEE)。zlib 依存を追加するほどではない
/** @brief CRC32 (IEEE) テーブルを生成する */
[[nodiscard]] inline auto crc32_table() noexcept -> std::span<std::uint32_t const> {
  static auto const table = [] {
    auto t = std::array<std::uint32_t, 256>{};
    for (auto i = 0u; i < 256u; ++i) {
      auto c = static_cast<std::uint32_t>(i);
      for (auto k = 0; k < 8; ++k) {
        c = (c & 1u) != 0u ? 0xEDB88320u ^ (c >> 1u) : c >> 1u;
      }
      t[i] = c;
    }
    return t;
  }();
  return {table.data(), table.size()};
}

/**
 * @brief バイト列の CRC32 (IEEE) を計算する
 * @param data 入力データ
 * @param size バイト数
 * @return CRC32 値
 */
[[nodiscard]] inline auto crc32(void const* const data, std::size_t const size) noexcept -> std::uint32_t {
  auto crc = 0xFFFFFFFFu;
  auto const* p = static_cast<std::uint8_t const*>(data);
  auto const t = crc32_table();
  for (auto i = std::size_t{0}; i < size; ++i) {
    crc = t[(crc ^ p[i]) & 0xFFu] ^ (crc >> 8u);
  }
  return crc ^ 0xFFFFFFFFu;
}

namespace detail {
inline auto constexpr kCdMagic = std::uint32_t{0x52415A58};     // "ZXAR"
inline auto constexpr kFooterMagic = std::uint32_t{0x45414A58}; // "ZXAE" — ponytail: CD と EOCD で magic を分離し誤検出を防ぐ
inline auto constexpr kVersion = std::uint16_t{1};
inline auto constexpr kFooterSize = std::size_t{36}; // magic4 + off8 + size8 + total8 + flags4 + crc4

/** @brief LE で u16 を書き込む */
inline auto store16(std::vector<std::uint8_t>& out, std::uint16_t const v) -> void {
  out.push_back(static_cast<std::uint8_t>(v & 0xFFu));
  out.push_back(static_cast<std::uint8_t>((v >> 8u) & 0xFFu));
}

/** @brief LE で u32 を書き込む */
inline auto store32(std::vector<std::uint8_t>& out, std::uint32_t const v) -> void {
  for (auto i = 0; i < 4; ++i) {
    out.push_back(static_cast<std::uint8_t>((v >> (8u * i)) & 0xFFu));
  }
}

/** @brief LE で u64 を書き込む */
inline auto store64(std::vector<std::uint8_t>& out, std::uint64_t const v) -> void {
  for (auto i = 0; i < 8; ++i) {
    out.push_back(static_cast<std::uint8_t>((v >> (8u * i)) & 0xFFu));
  }
}

/** @brief LE で u16 を読む */
[[nodiscard]] inline auto load16(std::uint8_t const* const p) noexcept -> std::uint16_t {
  return static_cast<std::uint16_t>(p[0] | (static_cast<std::uint16_t>(p[1]) << 8u));
}

/** @brief LE で u32 を読む */
[[nodiscard]] inline auto load32(std::uint8_t const* const p) noexcept -> std::uint32_t {
  auto v = std::uint32_t{0};
  for (auto i = 0; i < 4; ++i) {
    v |= static_cast<std::uint32_t>(p[i]) << (8u * i);
  }
  return v;
}

/** @brief LE で u64 を読む */
[[nodiscard]] inline auto load64(std::uint8_t const* const p) noexcept -> std::uint64_t {
  auto v = std::uint64_t{0};
  for (auto i = 0; i < 8; ++i) {
    v |= static_cast<std::uint64_t>(p[i]) << (8u * i);
  }
  return v;
}

/** @brief フッタの展開形 */
struct Footer {
  std::uint64_t cd_offset = 0;   ///< 圧縮フレームサイズ (=CD開始位置)
  std::uint64_t cd_size = 0;     ///< CD バイト数
  std::uint64_t total_decomp = 0; ///< 連結生データの総サイズ
  std::uint32_t flags = 0;       ///< bit0: zxc checksum 有効
};

/**
 * @brief 末尾フッタをパース・検証する
 * @param arc アーカイブ全体
 * @return フッタ、またはエラー
 */
template <ByteRange R>
[[nodiscard]] inline auto parse_footer(R const& arc) -> std::expected<Footer, Error> {
  auto const n = std::ranges::size(arc);
  if (n < kFooterSize) {
    return std::unexpected(Error::BadFooter);
  }
  auto const* const base = reinterpret_cast<std::uint8_t const*>(std::ranges::data(arc));
  auto const* const f = base + (n - kFooterSize);
  if (load32(f) != kFooterMagic) {
    return std::unexpected(Error::BadFooter);
  }
  // フッタ CRC は先頭32B に対する CRC32
  auto const want = load32(f + 32);
  if (crc32(f, 32) != want) {
    return std::unexpected(Error::BadFooter);
  }
  auto out = Footer{
      .cd_offset = load64(f + 4),
      .cd_size = load64(f + 12),
      .total_decomp = load64(f + 20),
      .flags = load32(f + 28),
  };
  if (out.cd_offset + out.cd_size + kFooterSize != n) {
    return std::unexpected(Error::BadFooter);
  }
  return out;
}

/**
 * @brief CD をパースする (フッタ検証済み前提)
 * @param arc アーカイブ全体
 * @param footer パース済みフッタ
 * @return エントリ一覧、またはエラー
 */
template <ByteRange R>
[[nodiscard]] inline auto parse_cd(R const& arc, Footer const& footer) -> std::expected<std::vector<EntryInfo>, Error> {
  auto const* const base = reinterpret_cast<std::uint8_t const*>(std::ranges::data(arc));
  auto const* const cd = base + footer.cd_offset;
  if (footer.cd_size < 10) {
    return std::unexpected(Error::BadArchive);
  }
  if (load32(cd) != kCdMagic) {
    return std::unexpected(Error::BadArchive);
  }
  if (load16(cd + 4) != kVersion) {
    return std::unexpected(Error::BadArchive);
  }
  auto const count = load32(cd + 6);
  auto entries = std::vector<EntryInfo>{};
  entries.reserve(count);
  auto pos = std::size_t{10};
  for (auto i = std::uint32_t{0}; i < count; ++i) {
    // 固定部 30B (name_len2 + off8 + size8 + crc4 + mtime8 ... ではなく下記順)
    if (pos + 2 > footer.cd_size) {
      return std::unexpected(Error::BadArchive);
    }
    auto const name_len = load16(cd + pos);
    pos += 2;
    if (pos + name_len + 8 + 8 + 4 + 8 + 4 > footer.cd_size) {
      return std::unexpected(Error::BadArchive);
    }
    auto name = std::string{reinterpret_cast<char const*>(cd + pos), name_len};
    pos += name_len;
    auto e = EntryInfo{
        .name = std::move(name),
        .offset = load64(cd + pos),
        .size = load64(cd + pos + 8),
        .crc = load32(cd + pos + 16),
        .mtime = load64(cd + pos + 20),
        .mode = load32(cd + pos + 28),
    };
    pos += 32;
    // 範囲の妥当性: 連結生データ内に収まること
    if (e.offset + e.size < e.offset || e.offset + e.size > footer.total_decomp) {
      return std::unexpected(Error::BadArchive);
    }
    entries.push_back(std::move(e));
  }
  if (pos != footer.cd_size) {
    return std::unexpected(Error::BadArchive);
  }
  return entries;
}

/**
 * @brief 連結生データを seekable 単一フレームとして圧縮する
 * @param raw 連結生データ
 * @param opt 作成オプション
 * @return 圧縮フレーム、またはエラー
 */
[[nodiscard]] inline auto compress_frame(std::span<std::uint8_t const> const raw, Options const& opt)
    -> std::expected<std::vector<std::uint8_t>, Error> {
  auto copt = zxc_compress_opts_t{};
  copt.level = opt.level;
  copt.checksum_enabled = opt.checksum ? 1 : 0;
  copt.seekable = 1; // 途中解凍に必須のため固定
  if (opt.block_size != 0) {
    copt.block_size = opt.block_size;
  }
  auto const bound = static_cast<std::size_t>(zxc_compress_bound(raw.size()));
  auto dst = std::vector<std::uint8_t>(bound);
  auto const* const src = raw.empty() ? dst.data() : raw.data(); // 空入力でも有効なポインタを渡す
  auto const r = zxc_compress(src, raw.size(), dst.data(), dst.size(), &copt);
  if (r < 0) {
    return std::unexpected(Error::CompressionFailed);
  }
  dst.resize(static_cast<std::size_t>(r));
  return dst;
}

} // namespace detail

/**
 * @brief 複数ファイルを単一圧縮ストリームのアーカイブにまとめる
 * @param files 格納ファイル一覧 (順序保持、同名不可)
 * @param opt 圧縮オプション
 * @return アーカイブバイト列、またはエラー
 */
[[nodiscard]] inline auto create(std::vector<FileData> const& files, Options const& opt = {})
    -> std::expected<std::vector<std::uint8_t>, Error> {
  // 同名チェック
  for (auto i = std::size_t{0}; i < files.size(); ++i) {
    if (files[i].name.empty() || files[i].name.size() > 0xFFFF) {
      return std::unexpected(Error::InvalidArgument);
    }
    for (auto j = i + 1; j < files.size(); ++j) {
      if (files[i].name == files[j].name) {
        return std::unexpected(Error::DuplicateEntry);
      }
    }
  }

  // 連結生データを構築し、各エントリのオフセットを確定する
  auto total = std::size_t{0};
  for (auto const& f : files) {
    total += f.data.size();
  }
  auto raw = std::vector<std::uint8_t>{};
  raw.reserve(total);
  auto entries = std::vector<EntryInfo>{};
  entries.reserve(files.size());
  for (auto const& f : files) {
    auto const off = static_cast<std::uint64_t>(raw.size());
    raw.insert(raw.end(), f.data.begin(), f.data.end());
    entries.push_back(EntryInfo{
        .name = f.name,
        .offset = off,
        .size = static_cast<std::uint64_t>(f.data.size()),
        .crc = crc32(f.data.data(), f.data.size()),
        .mtime = f.mtime,
        .mode = f.mode,
    });
  }

  auto frame = detail::compress_frame({raw.data(), raw.size()}, opt);
  if (!frame) {
    return std::unexpected(frame.error());
  }

  // CD を構築する (非圧縮)
  auto cd = std::vector<std::uint8_t>{};
  detail::store32(cd, detail::kCdMagic);
  detail::store16(cd, detail::kVersion);
  detail::store32(cd, static_cast<std::uint32_t>(entries.size()));
  for (auto const& e : entries) {
    detail::store16(cd, static_cast<std::uint16_t>(e.name.size()));
    cd.insert(cd.end(), e.name.begin(), e.name.end());
    detail::store64(cd, e.offset);
    detail::store64(cd, e.size);
    detail::store32(cd, e.crc);
    detail::store64(cd, e.mtime);
    detail::store32(cd, e.mode);
  }

  // アーカイブ = frame + CD + footer(36B)
  auto arc = std::vector<std::uint8_t>{};
  arc.reserve(frame->size() + cd.size() + detail::kFooterSize);
  arc.insert(arc.end(), frame->begin(), frame->end());
  auto const cd_offset = static_cast<std::uint64_t>(frame->size());
  arc.insert(arc.end(), cd.begin(), cd.end());

  auto footer = std::vector<std::uint8_t>{};
  detail::store32(footer, detail::kFooterMagic);
  detail::store64(footer, cd_offset);
  detail::store64(footer, static_cast<std::uint64_t>(cd.size()));
  detail::store64(footer, static_cast<std::uint64_t>(raw.size()));
  detail::store32(footer, opt.checksum ? 1u : 0u);
  detail::store32(footer, crc32(footer.data(), footer.size()));
  arc.insert(arc.end(), footer.begin(), footer.end());
  return arc;
}

/**
 * @brief アーカイブ内のエントリ一覧を取得する (解凍なし)
 * @param archive アーカイブバイト列
 * @return エントリ情報一覧、またはエラー
 */
template <ByteRange R = std::span<std::uint8_t const>>
[[nodiscard]] inline auto list(R const& archive) -> std::expected<std::vector<EntryInfo>, Error> {
  auto footer = detail::parse_footer(archive);
  if (!footer) {
    return std::unexpected(footer.error());
  }
  return detail::parse_cd(archive, footer.value());
}

/**
 * @brief フレーム全体を一括展開する (内部用)
 * @param frame 圧縮フレーム
 * @param total 期待する展開サイズ
 * @param checksum チェックサム検証の有無
 * @return 展開データ、またはエラー
 */
[[nodiscard]] inline auto decompress_full(
    std::span<std::uint8_t const> const frame, std::uint64_t const total, bool const checksum)
    -> std::expected<std::vector<std::uint8_t>, Error> {
  auto out = std::vector<std::uint8_t>(static_cast<std::size_t>(total));
  if (total == 0) {
    return out;
  }
  auto dopt = zxc_decompress_opts_t{};
  dopt.checksum_enabled = checksum ? 1 : 0;
  auto const r = zxc_decompress(frame.data(), frame.size(), out.data(), out.size(), &dopt);
  if (r < 0) {
    return std::unexpected(Error::DecompressionFailed);
  }
  out.resize(static_cast<std::size_t>(r));
  return out;
}

/**
 * @brief 指定名のファイルを1つ取り出す (該当範囲のみ復号)
 * @param archive アーカイブバイト列
 * @param name エントリ名
 * @return 生バイト列、またはエラー
 */
template <ByteRange R = std::span<std::uint8_t const>>
[[nodiscard]] inline auto extract(R const& archive, std::string_view const name)
    -> std::expected<std::vector<std::uint8_t>, Error> {
  auto const base = reinterpret_cast<std::uint8_t const*>(std::ranges::data(archive));
  auto footer = detail::parse_footer(archive);
  if (!footer) {
    return std::unexpected(footer.error());
  }
  auto entries = detail::parse_cd(archive, footer.value());
  if (!entries) {
    return std::unexpected(entries.error());
  }
  auto const* found = static_cast<EntryInfo const*>(nullptr);
  for (auto const& e : entries.value()) {
    if (e.name == name) {
      found = &e;
      break;
    }
  }
  if (found == nullptr) {
    return std::unexpected(Error::EntryNotFound);
  }
  if (found->size == 0) {
    return std::vector<std::uint8_t>{};
  }
  // zxc seekable で該当範囲のみ復号する
  auto* const h = zxc_seekable_open(base, static_cast<std::size_t>(footer->cd_offset));
  if (h == nullptr) {
    return std::unexpected(Error::SeekableError);
  }
  auto out = std::vector<std::uint8_t>(static_cast<std::size_t>(found->size));
  auto const r = zxc_seekable_decompress_range(
      h, out.data(), out.size(), found->offset, static_cast<std::size_t>(found->size));
  zxc_seekable_free(h);
  if (r < 0) {
    return std::unexpected(Error::DecompressionFailed);
  }
  out.resize(static_cast<std::size_t>(r));
  if (crc32(out.data(), out.size()) != found->crc) {
    return std::unexpected(Error::CrcMismatch);
  }
  return out;
}

/**
 * @brief 全ファイルを展開する (全体を1回復号して分割)
 * @param archive アーカイブバイト列
 * @return ファイル一覧、またはエラー
 */
template <ByteRange R = std::span<std::uint8_t const>>
[[nodiscard]] inline auto extract_all(R const& archive) -> std::expected<std::vector<FileData>, Error> {
  auto const base = reinterpret_cast<std::uint8_t const*>(std::ranges::data(archive));
  auto footer = detail::parse_footer(archive);
  if (!footer) {
    return std::unexpected(footer.error());
  }
  auto entries = detail::parse_cd(archive, footer.value());
  if (!entries) {
    return std::unexpected(entries.error());
  }
  // 全体を1回復号する (範囲復号の繰返しより高速)
  auto full = decompress_full(
      {base, static_cast<std::size_t>(footer->cd_offset)},
      footer->total_decomp,
      (footer->flags & 1u) != 0u);
  if (!full) {
    return std::unexpected(full.error());
  }
  auto out = std::vector<FileData>{};
  out.reserve(entries->size());
  for (auto const& e : entries.value()) {
    auto fd = FileData{.name = e.name, .data = {}, .mtime = e.mtime, .mode = e.mode};
    fd.data.assign(
        full->begin() + static_cast<std::ptrdiff_t>(e.offset),
        full->begin() + static_cast<std::ptrdiff_t>(e.offset + e.size));
    if (crc32(fd.data.data(), fd.data.size()) != e.crc) {
      return std::unexpected(Error::CrcMismatch);
    }
    out.push_back(std::move(fd));
  }
  return out;
}

/**
 * @brief 既存アーカイブにファイルを追加する (全体再構築、同名は置換)
 * @param archive 既存アーカイブ
 * @param files 追加ファイル一覧
 * @param opt 圧縮オプション (再圧縮に使用)
 * @return 新しいアーカイブ、またはエラー
 */
template <ByteRange R = std::span<std::uint8_t const>>
[[nodiscard]] inline auto add(R const& archive, std::vector<FileData> const& files, Options const& opt = {})
    -> std::expected<std::vector<std::uint8_t>, Error> {
  auto cur = extract_all(archive);
  if (!cur) {
    return std::unexpected(cur.error());
  }
  for (auto const& f : files) {
    if (f.name.empty() || f.name.size() > 0xFFFF) {
      return std::unexpected(Error::InvalidArgument);
    }
    auto replaced = false;
    for (auto& e : cur.value()) {
      if (e.name == f.name) {
        e = f;
        replaced = true;
        break;
      }
    }
    if (!replaced) {
      cur->push_back(f);
    }
  }
  return create(cur.value(), opt);
}

/**
 * @brief 既存アーカイブからファイルを削除する (全体再構築)
 * @param archive 既存アーカイブ
 * @param names 削除するエントリ名一覧 (存在しない名は無視)
 * @param opt 圧縮オプション (再圧縮に使用)
 * @return 新しいアーカイブ、またはエラー
 */
template <ByteRange R = std::span<std::uint8_t const>>
[[nodiscard]] inline auto remove(R const& archive, std::vector<std::string> const& names, Options const& opt = {})
    -> std::expected<std::vector<std::uint8_t>, Error> {
  auto cur = extract_all(archive);
  if (!cur) {
    return std::unexpected(cur.error());
  }
  auto kept = std::vector<FileData>{};
  kept.reserve(cur->size());
  for (auto& e : cur.value()) {
    auto drop = false;
    for (auto const& n : names) {
      if (e.name == n) {
        drop = true;
        break;
      }
    }
    if (!drop) {
      kept.push_back(std::move(e));
    }
  }
  return create(kept, opt);
}

/**
 * @brief アーカイブをファイルに保存する
 * @param path 保存先パス
 * @param archive アーカイブバイト列
 * @return 成功/失敗
 */
template <ByteRange R = std::span<std::uint8_t const>>
[[nodiscard]] inline auto save_archive(std::filesystem::path const& path, R const& archive)
    -> std::expected<void, Error> {
  auto ec = std::error_code{};
  if (path.has_parent_path()) {
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
      return std::unexpected(Error::IoError);
    }
  }
  auto ofs = std::ofstream{path, std::ios::binary | std::ios::trunc};
  if (!ofs) {
    return std::unexpected(Error::IoError);
  }
  auto const* const data = reinterpret_cast<char const*>(std::ranges::data(archive));
  auto const size = static_cast<std::streamsize>(std::ranges::size(archive));
  ofs.write(data, size);
  if (!ofs) {
    return std::unexpected(Error::IoError);
  }
  return {};
}

/**
 * @brief アーカイブをファイルから読み込む
 * @param path 読込元パス
 * @return アーカイブバイト列、またはエラー
 */
[[nodiscard]] inline auto load_archive(std::filesystem::path const& path)
    -> std::expected<std::vector<std::uint8_t>, Error> {
  auto ifs = std::ifstream{path, std::ios::binary | std::ios::ate};
  if (!ifs) {
    return std::unexpected(Error::IoError);
  }
  auto const size = ifs.tellg();
  if (size < 0) {
    return std::unexpected(Error::IoError);
  }
  ifs.seekg(0);
  auto buf = std::vector<std::uint8_t>(static_cast<std::size_t>(size));
  if (!buf.empty()) {
    ifs.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
    if (!ifs) {
      return std::unexpected(Error::IoError);
    }
  }
  return buf;
}

/**
 * @brief 指定エントリをファイルに取り出す
 * @param archive アーカイブバイト列
 * @param name エントリ名
 * @param path 保存先パス (親ディレクトリは自動作成)
 * @return 成功/失敗
 */
template <ByteRange R = std::span<std::uint8_t const>>
[[nodiscard]] inline auto extract_to_file(R const& archive, std::string_view const name, std::filesystem::path const& path)
    -> std::expected<void, Error> {
  auto data = extract(archive, name);
  if (!data) {
    return std::unexpected(data.error());
  }
  return save_archive(path, data.value());
}

/**
 * @brief 実ファイル群からアーカイブを作成する
 * @param files (実パス, アーカイブ内名) の一覧
 * @param opt 圧縮オプション
 * @return アーカイブバイト列、またはエラー
 */
[[nodiscard]] inline auto create_from_files(
    std::vector<std::pair<std::filesystem::path, std::string>> const& files, Options const& opt = {})
    -> std::expected<std::vector<std::uint8_t>, Error> {
  auto fds = std::vector<FileData>{};
  fds.reserve(files.size());
  for (auto const& [path, name] : files) {
    auto loaded = load_archive(path);
    if (!loaded) {
      return std::unexpected(loaded.error());
    }
    fds.push_back(FileData{.name = name, .data = std::move(loaded.value())});
  }
  return create(fds, opt);
}

} // namespace zxarc

#endif // __ZXARC_HPP__
