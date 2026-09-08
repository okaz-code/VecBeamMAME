# HDR 設定ガイド

English: [hdr-settings.md](hdr-settings.md)

VecBeamMAME の HDR 出力の仕組みと較正方法をまとめる。ベクター CRT は黒地に細く非常に明るい線を描くため HDR と相性がよく、背景を真っ黒に保ったままビームだけがディスプレイのピーク輝度を使える。

関連: [追加パラメータ一覧](added-parameters.ja.md)（HDR / SDR presentation の節）、[起動時オプション](startup-options.ja.md)

---

## 1. 仕組み

HDR 経路はフレームを **nits 基準**のワーキングバッファへ合成し、最後の Present パスで表示方式に応じて変換する。

Windows HDR10 の ST.2084 PQ と SDR のガンマ OETF は RGB 各成分へ個別に適用する。変換後のコード値比率ではなく、ディスプレイが逆変換した後の線形 RGB 比率を Win HDR・Mac EDR・SDR で一致させるためである。色相保持と高輝度圧縮は OETF の前段にある線形領域のロールオフで行う。

- **Windows HDR10** — Rec.2020 原色＋ST.2084（PQ）、HDR10 スワップチェイン。**Windows の HDR モード有効＋d3d11/d3d12 バックエンド**が必要。コンテンツは絶対 nits を出力し、パネル側が自分のピークへトーンマップする。
- **macOS EDR** — 拡張リニア出力。`1.0` ＝その時点のディスプレイの SDR 基準白で、それを超えるビームが HDR ヘッドルームを使う。NSScreen が返すのは絶対 nit ではなくこの比率だが、**ヘッドルームは固定 100nit を基準に報告される**ので、そこから絶対 nits を復元できる（§3.5）。
- **SDR フォールバック** — HDR 無効時は同じ内容を通常バックバッファ向けにトーンマップ。絶対 nits が定義できないため、SDR は独立した正規化系（`sdr_*`）を使う。

ワーキングバッファへの書き込み:

- ベクター画は **`beam_nits`**（較正から導出、§3）で書き込まれる → フル強度の線 1 本 = `beam_nits`
- UI / アートワーク / 背景は **reference white** で書き込まれる。EDR ではその時点の SDR 基準白、Windows HDR10 では OS 報告の SDR 白

---

## 2. パラメータ

### 較正値（両プラットフォーム共通・絶対 nits）

| パラメータ | 既定 | 意味 |
|---|---:|---|
| `hdr_peak_target_nits` | 3000 | **画面上で最も明るい点**（スポーク、敵弾、爆発中心）の目標。ディスプレイのピークでクランプされる |
| `hdr_beam_target_nits` | 375 | 通常ベクター 1 本の目標 |
| `hdr_beam_floor_nits` | 250 | 通常ベクターをこれ以上暗くしない下限 |

**この 3 つが較正の実体で、すべて絶対 nits・プラットフォーム非依存**。同じ値を Mac の cfg と Windows の cfg に置ける。

### 表示側の整形

| パラメータ | 既定 | 意味 |
|---|---:|---|
| `hdr_shoulder_start` | 0.85 | ショルダーの開始位置（**ディスプレイ天井の割合**）。ここまでは線形再現、以降が漸近圧縮 |
| `hdr_sat_protect` | 0.0 | 飽和色が 1 原色だけピーク超えを要求して白飛びするのを保護（カラーのみ） |
| `hdr_glow_stability` | 1.0 | ビームピークが変わってもグロー光量を絶対 nits で安定させる |
| `hdr_headroom_override` | 0.0 | Present 時の天井を固定（× SDR 白）。0 = ディスプレイのライブ値に追従。**較正時の再現性確保用** |

### 導出値（設定しない）

| パラメータ | 内容 |
|---|---|
| `beam_peak_ratio` | `beam_nits ÷ reference white`。プラットフォームごとに自動で違う値になる |
| `hdr_rolloff_max` | `天井 ÷ ビーム`。ディスプレイが目標に届かないと目標比より小さくなる |

`hdr_peak_target_nits > 0` の間、この 2 つは**スライダーメニューに表示されない**。値は保持・保存されるが読まれない。

### SDR 経路（絶対 nits が定義できないため独立）

| パラメータ | 既定 | 意味 |
|---|---:|---|
| `sdr_beam_level` | 0.90（mono 0.72） | 通常ビームの出力レベル（× paper white） |
| `sdr_rolloff_knee` | 0.75 | SDR ショルダー開始（× white） |
| `sdr_rolloff_ceiling` | 1.00 | SDR 上限（× white） |
| `sdr_shadow_curve` | 0.95 | 中間調の再形成 |
| `bright_normal_cap` | 0.8（mono 0.6） | 通常ビームの輝度上限。n > 1 の過大駆動でのみ解放 |

### 起動オプション

| オプション | 既定 | 意味 |
|---|---|---|
| `bgfx_hdr` | 1 | HDR10/EDR を試行。利用不能なら SDR へフォールバック。0 = SDR 強制 |
| `bgfx_hdr_display_peak` | `auto` | ディスプレイピーク（nits）。`auto` / 数値 / `0`（導出せずチェイン既定を維持） |
| `bgfx_macos_edr_reference_white` | 100 | **macOS EDR のヘッドルーム 1.0 が何 nit か。** パネルピーク = potential headroom × この値 |
| `bgfx_macos_edr_calibration` | `absolute` | 較正の基準。`absolute` = 目標値は絶対 nits で輝度変更に不変。`relative` = 名目 SDR 白の倍数として読み、輝度に追従（§3.7） |
| `bgfx_hdr_paper_white` | 200 | UI 白（nits）。**HDR/EDR ではどちらも上書きされ、SDR では約分されて消えるため、実質的に効かない**（§6） |

> 起動オプションはコマンドライン/ini、その他はスライダーメニュー（または cfg）で設定する。

---

## 3. 較正モデル

較正すべきは**絶対輝度**であって、SDR 白に対する比ではない。SDR 白は Windows では OS の「SDR コンテンツの明るさ」設定、macOS では 100nit 規約であり、**同じ比が両者で別の光量を意味してしまう**（実測: 同一モニタで 2.5 × SDR 白が Windows 600nit、macOS 約 250nit）。

```
ceiling = min(hdr_peak_target_nits, パネルピーク)
beam    = clamp(ceiling × (hdr_beam_target_nits / hdr_peak_target_nits),
                hdr_beam_floor_nits, hdr_beam_target_nits)
```

**目標をピーク側に置く**理由は、実機のブラウン管が「最も明るい点」で評価されるからで、通常ベクターはその一定割合下に置く。こうするとディスプレイが明るくなれば天井とビームが一緒に上がり、**絵の比率を保ったまま全体が明るくなる**。3000nit のモニタに替えれば目標到達、それ以上は変化しない。

**床**は、目標に届かないパネルが通常ベクターまで道連れにするのを防ぐ。届かないぶんの損は過大レンジ側が被る ─ 暗い絵で比率が完璧より、正しい明るさでハイライトが詰まる方がよい、という優先順位である。

導出は毎フレーム、使用時に行う。cfg の編集、輝度変更、モニタ交換がそのまま反映される。

---

## 3.5 自動設定

既定は `-video bgfx -bgfx_hdr 1 -bgfx_hdr_display_peak auto`。Windows では BGFX 初期化前に対象モニタの Advanced Color 状態を確認し、HDR 有効かつ D3D11/D3D12 なら最初のスワップチェインから HDR10/RGB10A2 で生成する（一時的な SDR スワップチェインを Windows Auto HDR が検出する問題を避けるため）。

**パネルピークの求め方:**

| | 取得方法 |
|---|---|
| Windows | DXGI `IDXGIOutput6::GetDesc1().MaxLuminance` |
| Windows SDR 白 | `DisplayConfigGetDeviceInfo(...GET_SDR_WHITE_LEVEL)`。HDR10 の UI 白へ自動反映 |
| macOS `auto` | **potential headroom × `bgfx_macos_edr_reference_white`（既定 100）** |
| macOS 数値指定 | 指定値をそのまま使用 |

macOS の 100nit 規約は 2 台で実測して確認したもの。

```
内蔵 Liquid Retina XDR : potential 16.00x → 1600nit（公称 1600 と一致）
外付け                 : potential 14.05x → 1405nit（DXGI は 1390。EDID 丸めの範囲）
```

現在の SDR 白は `パネルピーク ÷ current headroom` で求まる。輝度スライダを上げると SDR 白が上がり current headroom が反比例で下がるので、この式は輝度変更に自動追従する。導出結果は起動時とスケール変化時にログへ出る。

```
BGFX: macOS EDR absolute scale: panel peak 1600 nits (potential headroom x reference white),
      SDR white 100 nits at current 16.00x
```

100nit 規約が成り立たないディスプレイに当たった場合は `bgfx_macos_edr_reference_white` で調整するか、`bgfx_hdr_display_peak` で直接ピークを指定する（数値指定は導出全体を上書きする）。

**Present 時の天井**は current headroom に追従する。低下はクリッピング防止のため即時、上昇は約 1 秒で平滑化する。macOS では画面輝度を動かすとこれが動くため、**較正中は輝度を固定するか `hdr_headroom_override` で天井を固定すること**。

`-verbose` で有効経路・SDR 白・headroom・導出値を確認できる。

---

## 3.7 較正の基準（absolute / relative）

`bgfx_macos_edr_calibration` で 2 つの基準を選べる。

| | `absolute`（既定） | `relative` |
|---|---|---|
| 目標値の意味 | 絶対 nits | 名目 SDR 白（`bgfx_hdr_paper_white`）の倍数 |
| パネルピーク認識 | `potential × 100`（不変） | `current × paper_white`（輝度で動く） |
| 輝度を上げると | 描画は一定、**UI だけ明るくなる** | **描画も天井も明るくなる** |
| Mac / Win の値共通化 | 成立する | 崩れる |

`relative` は絶対 nits 導出を入れる前の挙動で、導出をスキップすることで実現している。

内蔵 XDR（potential 16.00x ＝ パネル実 1600nit）での実測:

| mode | paper_white | current | パネル認識 | 天井 | beam | 実発光 |
|---|---:|---:|---:|---:|---:|---:|
| absolute | 任意 | 16.00x | 1600 | 1600 | 250 | **250 nit** |
| absolute | 任意 | 8.00x | 1600 | 1600 | 250 | **250 nit** |
| absolute | 任意 | 4.00x | 1600 | 1600 | 250 | **250 nit** |
| relative | 200 | 16.00x | 3200 | 3000 | 375 | 188 nit |
| relative | 100 | 16.00x | 1600 | 1600 | 250 | **250 nit** |
| relative | 100 | 8.00x | 800 | 800 | 250 | 500 nit |
| relative | 100 | 4.00x | 400 | 400 | 250 | 1000 nit |

同条件の Windows 外付け（DXGI 1390 / SDR 白 240）は **250 nit**。

つまり **`relative` が Windows と一致するのは `potential == current` かつ `paper_white` が実際の
基準白（100）と一致しているときだけ**で、その条件下では `current × paper_white = potential × 100`
となり **`relative` は `absolute` に退化する**。既定の `paper_white` 200 のままでは 2 倍ずれる。

`absolute` が存在する理由は、potential と current が乖離したとき ─ すなわち輝度スライダを基準
測定点から動かしたとき ─ に較正を保つことにある。上表で `absolute` は current 16→8→4 で 250nit
を維持するが、`relative` は 250→500→1000 と倍々に暴れる。

**Mac / Win の値共通化が目的なら `absolute` を使うこと。** `relative` の用途は「輝度スライダに
応じて絵の明るさも変わってほしい」という、較正とは別の目的に限られる。その場合は
`-bgfx_hdr_paper_white 100` の併用が事実上必須になる。

なお「天井だけを輝度に追従させる」第 3 の基準は存在しない。SDR 白点を動かしてもパネルが出せる
最大輝度は変わらない（`current × 現在のSDR白 = potential × 100 = パネルピーク` で約分される）ため、
絶対 nits で見た天井には追従する対象がない。

---

## 4. モニタ別の実測例

較正値は 3 台すべてで同一（peak 3000 / beam 375 / floor 250）。違うのは導出結果だけである。

| 表示 | パネルピーク | 天井 | ビーム | 実効比 | knee ÷ beam |
|---|---:|---:|---:|---:|---:|
| Windows 外付け（DXGI 1390 / SDR 白 240） | 1390 | 1390 | 250（床） | 5.56 | 4.73 |
| macOS 外付け（potential 14.05x） | 1405 | 1405 | 250（床） | 5.62 | 4.78 |
| macOS 内蔵 XDR（potential 16.00x） | 1600 | 1600 | 250（床） | 6.40 | 5.44 |
| 3000nit 機（想定） | 3000 | 3000 | 375 | 8.00 | 6.80 |
| 5000nit 機（想定） | 3000（目標到達） | 3000 | 375 | 8.00 | 6.80 |

`knee ÷ beam` が、通常ベクター 1 本の上に残る**線形領域の広さ**である。ここが 1.0 だと 1 本を超える構造（重なり、頂点ドウェル、オーバードライブ）が全部ショルダーに乗り、階調が潰れる。

---

## 5. macOS EDR の注意

- EDR の `1.0` は固定値ではなく、その時点の SDR 基準白。`maximumExtendedDynamicRangeColorComponentValue` は「HDR ピーク ÷ 現在の SDR 基準白」の比率である。
- **potential と current は別物**。potential はパネル固有で輝度に依らず安定するため**パネルピークの算出**に使い、current は輝度に追従するため **Present 時の天井**に使う。この使い分けを誤ると、同一設定が実行ごとに別のトーンスケールへ落ちる。
- EDR レイヤー有効化前は current headroom が 1.0（ブートストラップ値）を返す。この間は導出を行わず待つ。
- paper white は導出した reference white に追従する。UI は paper white nits で描かれ Present は reference white で割るので、両者がずれると UI が `paper_white ÷ reference` 倍の明るさになる。
- EDR は 709 原色＝sRGB 原色。

### 5.1 Direct EDR 表示の診断オプション

外部 HDR モニタのフルスクリーンで背景・通常ビームだけが暗くなる場合は、診断ログと Metal HUD でレイヤー状態・Present mode を確認する。

- `-bgfx_macos_edr_diagnostics`: CAMetalLayer のアドレス、画面、pixel format、EDR/opaque/transaction 属性、colorspace、EDR metadata、contents scale、raw current headroom を 1 秒ごとに記録する。
- `bgfx_macos_force_composited` は既定で有効。CAMetalLayer を non-opaque にして Composited 表示を要求するが、bgfx の非同期 present を止めないよう `presentsWithTransaction` は変更しない。Metal HUD で Present mode が `Composited` になったことを確認する。
- Direct-to-display 経路との A/B は `-nobgfx_macos_force_composited` で起動する。既定時だけ安定する場合は macOS の Direct EDR 経路が主因と判断できる。

---

## 6. 補足

- 較正値は**出発点**。パネルのトーンマップ特性で見え方に差が出るので、最終的な明るさ感は実機で詰めること。
- **`bgfx_hdr_paper_white` は実質的に効かない。** Windows HDR10 では OS の SDR 白が、macOS EDR では導出した reference white が上書きする。SDR では `seed_peak = paper_white × sdr_beam_level` を Present で `paper_white` で割るため約分されて消える。残っているのは互換のためで、較正には使わない。
- ビームの明るさは較正 3 値、線の太さは `beam_width_*`、白飛び具合はショルダー、という役割分担で調整する。
- 過去に存在した `beam_peak_nits` / `hdr_rolloff_knee` / `hdr_diagnostics` / `phosphor_gamut` / `edr_sdr_level` は廃止済み。`beam_peak_nits` はチェインが較正 3 値を持たない場合のフォールバックとしてコード上にのみ残る。
