# zxcpp

`zxcpp` は、`zxc` 圧縮ライブラリを C++ から使いやすくするための軽量ラッパーです。
`std::span` と `std::expected` を使って、圧縮・展開処理を型安全かつシンプルに扱えます。

## 特徴

- ヘッダオンリー（`zxcpp.hpp`）
- `std::span` 入力対応
  - `std::span<const std::uint8_t>`
  - `std::span<const std::int8_t>`
  - `std::span<const std::byte>`
- 戻り値は `std::expected<std::vector<std::uint8_t>, zxcpp::Error>`
- `zxc` のチェックサム付き圧縮・展開に対応
- 段階処理 API（`StreamCompressor` / `StreamDecompressor`）を提供

## エラー型

`zxcpp::Error` には以下が定義されています。

- `CompressionFailed`
- `DecompressionFailed`
- `InvalidBufferSize`
- `ChecksumMismatch`

## 必要環境

- CMake 3.25 以上
- C++23 以上（環境によっては C++26 を自動選択）
- 依存ライブラリ
  - `zxc`
  - `Catch2`（テスト時）

依存関係は `vcpkg.json` に定義されています。

## ビルド

このリポジトリにはビルド用スクリプトが含まれます。

- Linux 向け: `build.sh`
- MinGW クロスビルド向け: `build_mingw.sh`
- Win64 クロスビルド向け: `build_win64.sh`

### Linux でのビルド

標準ビルド:

```sh
bash build.sh
```

> 補足: `build.sh` は `vcpkg` ツールチェインを使用します。

## テスト

テストのビルド後、以下で実行できます。

```sh
bash test.sh
```

`test.sh` は `build/` ディレクトリで `ctest -V` を実行します。

## 使い方

### 単純な圧縮・展開

```cpp
#include <cstdint>
#include <span>
#include <string_view>
#include "zxcpp.hpp"

int main() {
  std::string_view text = "Quick brown fox...";
  auto src = std::span{reinterpret_cast<std::uint8_t const*>(text.data()), text.size()};

  auto compressed = zxcpp::compress(src, 3, false);
  if (!compressed) {
    return 1;
  }

  auto decompressed = zxcpp::decompress(compressed.value(), false);
  if (!decompressed) {
    return 1;
  }

  return 0;
}
```

### ストリーミング API（段階処理）

`zxc_cctx_t` を内部で保持するクラスとして、以下を提供します。

- `zxcpp::StreamCompressor`
- `zxcpp::StreamDecompressor`

各 `update(input, output)` 呼び出しの戻り値は `std::expected<StreamResult, Error>` です。

- `StreamResult::input_consumed`: 今回消費した入力バイト数
- `StreamResult::output_produced`: 今回出力したバイト数
- `StreamResult::state`: 処理状態

`StreamState` の主な意味:

- `Ok`: 正常に処理（まだ継続可能）
- `NeedMoreInput`: 入力不足（続きの入力が必要）
- `OutputBufferFull`: 出力バッファが満杯（同じ入力位置で再呼び出し）
- `Completed`: 完了

#### 圧縮の流れ

1. `StreamCompressor::update()` を繰り返し呼ぶ
2. 出力先バッファが満杯なら `OutputBufferFull`
3. 入力をすべて渡したら `finish()` を呼ぶ
4. `Completed` になるまで `update({}, out)` を呼ぶ

#### 伸長の流れ

1. `StreamDecompressor::update()` を繰り返し呼ぶ
2. 出力先バッファが満杯なら `OutputBufferFull`
3. 圧縮データ終端に到達すると `Completed`
4. `Completed` 後の `update({}, out)` は **0 byte 出力**

#### 例

```cpp
#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>
#include "zxcpp.hpp"

int main() {
  std::string_view text = "streaming sample data ...";
  auto src = std::span{reinterpret_cast<std::uint8_t const*>(text.data()), text.size()};

  zxcpp::StreamCompressor compressor;
  std::array<std::uint8_t, 32> out{};
  std::vector<std::uint8_t> compressed;

  // 入力をチャンクで渡す
  std::size_t pos = 0;
  while (pos < src.size()) {
    auto chunk = src.subspan(pos, std::min<std::size_t>(8, src.size() - pos));
    bool sent = false;
    while (true) {
      auto in = sent ? std::span<std::uint8_t const>{} : chunk;
      auto r = compressor.update(in, out);
      if (!r) return 1;
      pos += r->input_consumed;
      sent = true;
      compressed.insert(compressed.end(), out.begin(), out.begin() + static_cast<std::ptrdiff_t>(r->output_produced));
      if (r->state != zxcpp::StreamState::OutputBufferFull) break;
    }
  }

  compressor.finish();
  while (true) {
    auto r = compressor.update({}, out);
    if (!r) return 1;
    compressed.insert(compressed.end(), out.begin(), out.begin() + static_cast<std::ptrdiff_t>(r->output_produced));
    if (r->state == zxcpp::StreamState::Completed) break;
  }

  // ここから伸長
  zxcpp::StreamDecompressor decompressor;
  std::vector<std::uint8_t> restored;
  std::size_t cpos = 0;

  while (true) {
    auto in = std::span<std::uint8_t const>{};
    if (cpos < compressed.size()) {
      auto n = std::min<std::size_t>(10, compressed.size() - cpos);
      in = std::span<std::uint8_t const>{compressed.data() + cpos, n};
    }

    auto r = decompressor.update(in, out);
    if (!r) return 1;
    cpos += r->input_consumed;
    restored.insert(restored.end(), out.begin(), out.begin() + static_cast<std::ptrdiff_t>(r->output_produced));

    if (r->state == zxcpp::StreamState::OutputBufferFull) {
      continue;
    }
    if (r->state == zxcpp::StreamState::Completed) {
      break;
    }
  }

  // 完了後 update({}, out) は 0 byte 出力
  auto tail = decompressor.update({}, out);
  if (!tail) return 1;
  if (tail->output_produced != 0) return 1;

  // 復元一致確認
  if (!std::ranges::equal(src, restored)) return 1;

  return 0;
}
```

#### 互換性に関する注意

この段階処理 API は、`zxcpp` 内部でチャンク境界情報を持つフレーミングを行います。
そのため、`StreamCompressor` の出力は one-shot の `zxcpp::decompress()` や `zxc` の通常ストリーム API へそのまま渡す前提ではありません。
対になる `StreamDecompressor` で復元してください。

## テスト構成メモ

このプロジェクトのテストは Catch2 を使用しています。

- `test/main.cpp` で `CATCH_CONFIG_MAIN` を定義
- `test/CMakeLists.txt` では `Catch2::Catch2` と `Catch2::Catch2WithMain` をリンク

## ライセンス

MIT License
