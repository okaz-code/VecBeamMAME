# HDR 設定ガイド

VecBeamMAME の HDR 出力の仕組みと、モニタのピーク輝度に合わせた設定方法をまとめる。ベクターCRTは黒地に細く非常に明るい線を描くため HDR と相性がよく、背景を真っ黒に保ったままビームだけがディスプレイのピーク輝度を使える。

関連: [追加パラメータ一覧](added-parameters.ja.md)（HDR / SDR presentation の節）、[起動時オプション](startup-options.ja.md)

---

## 1. 仕組み

HDR経路はフレームをnits基準のワーキングバッファへ合成し、最後のPresentパスで表示方式に応じて変換する。Windows HDR10と数値ピーク指定のmacOS EDRでは絶対輝度として較正し、Mac EDR autoでは同じ数値をSDR基準白に対する相対スケールとして扱う。

Windows HDR10のST.2084 PQとSDRのガンマOETFはRGB各成分へ個別に適用する。変換後のコード値比率ではなく、ディスプレイが逆変換した後の線形RGB比率をWin HDR・Mac EDR・SDRで一致させるためである。色相保持と高輝度圧縮はOETFの前段にある線形領域のロールオフで行う。

- **Windows HDR10** — Rec.2020 原色＋ST.2084（PQ）トランスファ、HDR10 スワップチェイン。**Windows の HDR モード有効＋d3d11/d3d12 バックエンド**が必要。コンテンツは絶対 nits を出力し、パネル側が自分のピークへトーンマップする。
- **macOS EDR** — 拡張リニア出力。`1.0`＝その時点のディスプレイの SDR 基準白で、それを超えるビームはHDRヘッドルームを使う。NSScreenが返すのは絶対nitではなくこの比率である。`bgfx_hdr_display_peak`を数値指定した場合だけ、`指定ピーク ÷ 現在headroom`からEDR 1.0の物理nitを求め、絶対輝度ワーキングバッファを正しくEDR値へ換算する。
- **SDR フォールバック** — HDR 無効時は同じ内容を通常バックバッファ向けにトーンマップ。

ワーキングバッファへの書き込み:

- ベクター画（`screen_hdr`）は `screen_hdr × beam_peak_nits` nits で書き込まれる → **フル強度の線1本 = `beam_peak_nits` nits**
- UI/アートワーク/背景は `paper_white` nits で書き込まれる

---

## 2. パラメータ

| パラメータ | 種別 | 既定 | 意味（nits換算） |
|---|---|---|---|
| `bgfx_hdr` | 起動オプション | 1 | HDR10/EDRを試行。利用不能ならSDRへ安全にフォールバック。0=SDR強制。 |
| `bgfx_hdr_paper_white` | 起動オプション | 200 | UI/アートワーク/背景の目標白。Windows HDRではOSのSDR白レベルを取得できた場合に自動置換。Macの相対auto時は内部の基準単位。 |
| `bgfx_hdr_display_peak` | 起動オプション | `auto` | Windows autoはDXGIの絶対ピーク。macOS autoはheadroom比だけで相対較正し、絶対ピークを推測しない。XDRを絶対nit較正する場合は`1600`のように数値指定する。 |
| `beam_peak_nits` | チェインスライダー | 800（JSON既定） | **フル強度の1本のベクター線**の輝度基準。絶対較正時はnit、Mac EDR auto時は相対スケール。HDR auto有効時は下記の自動値へ置換される。最重要ノブ。 |
| `hdr_rolloff_knee` | 〃（×beam_peak） | 1.0 | ここ（×beam_peak）までは無加工。 |
| `hdr_rolloff_max` | 〃（×beam_peak） | 2.4（JSON既定） | 線の**重なり/Overload**の到達上限。`beam_peak × max` nits に色相保持で漸近。HDR autoでは表示ピークから自動導出される。 |
| `hdr_sat_protect` | 〃 | 0.5 | 飽和色が1原色だけピーク超えを要求して白飛びするのを保護（カラーのみ有効）。 |
| `phosphor_gamut` | 〃 | 0.0 | Rec.2020 コンテナ内で色を実機 P22 蛍光体原色へ寄せる。 |
| `edr_sdr_level` | 〃 | 1.0 | macOS EDR 時に HDR ビームを触らず UI/アートを暗くする。 |
| `hdr_diagnostics` | 〃 | 0 | HDR/EDR最終リニアバッファの実測オーバレイ。通常は0、調整時だけ1。 |

> `bgfx_hdr`、`bgfx_hdr_paper_white`、`bgfx_hdr_display_peak`はコマンドライン/ini、その他はスライダーメニュー（またはcfg）で設定する。

表のチェイン既定値は統合後の`vector-color`。Star Warsで調整した非HDRパラメータはこのチェインへ反映済みだが、`beam_peak_nits`と`hdr_rolloff_max`はモニタ依存なので固定較正値を焼き込まず、HDR autoに委ねる。SDRでは通常線を`sdr_beam_level × paper_white`から開始し、最終的に`sdr_rolloff_ceiling`へ収める。

---

## 3. 調整の考え方（要点）

1. **`beam_peak_nits`** = 線1本の明るさ。Windows HDR10および数値ピークで較正したmacOS EDRでは絶対nits、Mac autoではSDR基準白に対する相対単位。
2. **`beam_peak_nits × hdr_rolloff_max`** = 線の重なり/Overloadが近づく上限。絶対較正時はモニタのピークnit、Mac auto時は利用可能headroomへ合わせる。こうするとパネルがハードクリップ（＝色ずれ）する前に、シェーダ側の**色相保持ロールオフ**が滑らかに肩を作る。
3. 絶対ピークを取得できる自動設定では、十分なヘッドルームがあれば線1本を **500nit** に置く。500nitを表示ピークの85%以内に収められない場合だけ、従来の`min(1.65 × SDR基準白, 0.85 × display peak)`へフォールバックする。Mac EDR autoは絶対ピークが不明なので従来の相対式を使う。
4. **`paper_white`** はUI/背景の白。Windows HDRではWindowsの「SDRコンテンツの明るさ」をWin32 DisplayConfigから取得して自動使用する。`gun_saturation`はRGBチャンネルの非線形飽和であり、この白レベル較正とは別機能。
5. `hdr_rolloff_knee` を 0.8 程度に下げると、線1本の手前から圧縮が始まり重なりの伸びをより穏やかにできる（好みで）。

---

## 3.5 自動設定（`bgfx_hdr_display_peak`）

本派生版の既定は `-video bgfx -bgfx_hdr 1 -bgfx_hdr_display_peak auto`。WindowsではBGFX初期化前に対象モニタのAdvanced Color状態を確認し、HDR有効かつD3D11/D3D12（`auto`を含む）なら最初のスワップチェインからHDR10/RGB10A2で生成する。これにより、一時的なSDRスワップチェインをWindows Auto HDRが検出する問題を避ける。HDR非対応モニタ・バックエンドではSDRへフォールバックし、`-bgfx_hdr 0`で明示的にSDRを強制できる。

- Windowsピーク: DXGI `IDXGIOutput6::GetDesc1().MaxLuminance`
- Windows SDR白: `DisplayConfigGetDeviceInfo(DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL)`。HDR10のUI/アート紙白へ自動反映
- macOS `auto`: `maximumPotentialExtendedDynamicRangeColorComponentValue`はEDR対応判定だけに使用し、`maximumExtendedDynamicRangeColorComponentValue`をフレーム毎に現在利用可能なEDRヘッドルームとして取得する。絶対ピークnitには換算しない
- macOS 数値指定: `EDR基準白nit = 指定Display Peak ÷ 現在headroom`を求め、Present時は`内部nit ÷ EDR基準白nit`で出力する。例: XDR 1600nit/headroom 16ならEDR 1.0=100nit
- 通常線（絶対ピーク既知）: `500 <= 0.85 × display peak`なら500nit。ヘッドルーム不足時は`min(1.65 × SDR白, 0.85 × display peak)`へフォールバックし、10nit刻みで丸める
- 通常線（macOS auto）: 絶対ピークを推測せず、従来どおりSDR基準白の1.65倍に相当する相対値
- Overload上限: Windows/数値指定は`hdr_rolloff_max = display peak / beam_peak`（範囲1.1〜8.0）。macOS autoはチェイン既定/cfg/手動値を維持し、present時だけ`min(ユーザー上限, current EDR headroom)`へ動的制限する
- 自動取得失敗時: 1000nitを仮定せず、チェイン既定を維持
- 優先順位: チェイン既定 < 自動導出 < 保存済みの手動cfg値 < 実行中のスライダー操作

自動導出値のまま終了した場合、`beam_peak_nits`と`hdr_rolloff_max`はcfgへ書き出さない。次回起動やモニタ変更時に、その表示環境から再計算できるようにするためである。スライダーを1ステップでも手動変更すると通常のcfg値として保存され、以後はその手動値がautoより優先される。

`-verbose`では有効経路、SDR白、headroom、自動導出値を確認できる。macOS autoはpotential capabilityと、EDRレイヤー有効後に確定したcurrent headroomを表示する。数値指定時は指定ピーク・headroom・導出したEDR Reference Whiteを表示する。

### 3.6 HDR診断オーバレイ

スライダー **HDR Diagnostics Overlay** を1にすると、最終Present直前の線形nitsバッファを約30フレームごとに読み戻し、画面左上へ次を表示する。

- Windows HDR10: OSから得た絶対display peak、paper whiteに対するheadroom、ロールオフ前後のnit値
- macOS EDR数値指定: 指定display peak、`peak/headroom`で導出したEDR Reference White、ロールオフ前後の絶対nit値
- macOS EDR auto: `Absolute display peak: unknown`と明示し、Beam/Pre/Post/上位平均/全画面平均をEDR倍率（`x`）で表示
- 絶対較正時は上位128画素平均、display peakの80%以上および1000nit以上の画素数も表示

値は最終Present直前の線形バッファから算出する。Windows HDR10と数値ピーク指定のmacOS EDRでは要求絶対nit、Mac autoではSDR基準白に対する相対倍率であり、測定器によるパネル実発光値そのものではない。ロールオフ後ピークがdisplay peak/headroomへどの程度達しているか、重ね描きが余裕を使えているかの調整に使う。SDRフォールバック時は測定せず、HDR/EDR出力が無効であることを表示する。全画面GPU readbackを伴うため、性能測定中や通常プレイでは0へ戻す。

## 4. モニタ別の設定例

| 表示 | モード | EDR/SDR基準 | ピーク | beam | rolloff max |
|---|---|---:|---:|---:|---:|
| Windows HDR400 | auto | SDR白200nitの場合 | 400nit | 330nit | 1.21 |
| Windows（実機検証例） | auto | SDR白148nit | 604nit | 500nit | 1.21 |
| Windows（高ピーク例） | auto | SDR白240nit | 1390nit | 500nit | 2.78 |
| macOS XDR | auto（相対） | current headroom 16x / nominal white 200 | 絶対値不明 | 1.65x（nominal 330） | チェイン/cfg値（current headroomで動的制限） |
| macOS XDR | `bgfx_hdr_display_peak 1600` | EDR Reference White 100nit（headroom 16x時） | 1600nit | 500nit | 3.20 |

XDRでWin HDRとの絶対輝度対応を取りたい場合は`bgfx_hdr_display_peak 1600`を指定する。数値指定後は診断オーバレイが`macOS EDR, absolute`となり、`Reference white: 100.0 nits`のように表示されることを確認する。headroomは画面輝度・電源・温度条件で変わるため、実行中もフレーム毎に追跡される。

絶対ピークが十分な表示では通常線を500nitに置き、追加のピーク能力を線の重なり、Overload、爆発へ割り当てる。低ピーク表示では従来のPaper White基準式へ戻る。保存済みcfgに手動設定したBeam Peak/Highlight Maxがある場合は自動値よりcfgが優先されるため、完全な自動値を確認するときは該当cfg値をリセットする。

## 5. macOS EDR の注意

- EDRの`1.0`は固定100nit/200nitではなく、その時点のSDR基準白（UI白）。`NSScreen.maximumExtendedDynamicRangeColorComponentValue`は「HDRピーク÷現在のSDR基準白」という比率であり、単独では絶対ピークnitを決定できない。
- `bgfx_hdr_display_peak auto`は安全な**相対モード**として、`L / bgfx_hdr_paper_white`で出力する。診断値もnitではなくEDR倍率で表示し、物理ピークを推測しない。
- 数値ピーク指定時は**絶対モード**になり、`EDR Reference White = 指定ピーク / headroom`を導出して`L / EDR Reference White`で出力する。このとき`beam_peak_nits`と診断オーバレイは絶対nitとして扱える。
- EDRレイヤー有効化前にcurrent headroomが1.0を返すmacOSでは、循環判定を避けるためpotential headroomを能力判定だけに使う。current値はレイヤー表示後に1.0を超えるまで確定せず、その後はフレーム毎に取得する。
- current headroom低下はクリッピング防止のため即時反映し、上昇は約1秒で平滑化する。通常線、`hdr_rolloff_max`、cfgは書き換えず、presentシェーダーの実効上限だけを制限する。
- EDRは709原色＝sRGB原色なので、`phosphor_gamut`（Rec.2020コンテナ向け）はEDR経路ではスキップされる。

### 5.1 Direct EDR表示の診断オプション

外部HDRモニタのフルスクリーンで背景・通常ビームだけが暗くなる場合は、診断ログとMetal HUDでレイヤー状態・Present modeを確認する。

- `-bgfx_macos_edr_diagnostics`: CAMetalLayerのアドレス、画面、pixel format、EDR/opaque/transaction属性、colorspace、EDR metadata、contents scale、raw current headroomを1秒ごとに記録する。
- `bgfx_macos_force_composited`は既定で有効。CAMetalLayerをnon-opaqueにしてComposited表示を要求するが、bgfxの非同期presentを止めないよう`presentsWithTransaction`は変更しない。Metal HUDでPresent modeが`Composited`になったことを確認する。
- Direct-to-display経路とのA/B比較は`-nobgfx_macos_force_composited`で起動する。既定時だけ安定する場合はmacOSのDirect EDR経路が主因と判断できる。

---

## 6. 測定器なしの比較手順

1. 同じMVECと同じフレーム番号をWin/Macで再生する。通常RGB、白、重なり、Overload、Ambient/Glow、デス・スター爆発を含める。
2. 最初は同一外付けモニタを両OSで使い、モニタ設定・室内照明・表示サイズを固定してOS差だけを見る。
3. `-verbose`のpresent経路、SDR白、display peak、beam/rolloff自動値を記録する。
4. 実画面はカメラの露出・ISO・シャッター・ホワイトバランスを固定して比較する。自動露出・自動WBは使わない。
5. 最後にMacBook Pro XDRへ移し、平均輝度→白/原色の色相→Overloadピークの順で確認する。

測定器なしでは絶対nit・色度の一致は保証できない。目標は平均輝度、色相、通常線とOverloadの相対的な眩しさを近づけること。モード別チェインは原則作らず、共通チェイン＋Present出力較正を使う。

## 7. 補足

- ここで示した値は**出発点の推奨値**。実際の見え方はパネルのトーンマップ特性で差が出るので、最終的な明るさ感は実機で微調整すること。
- モニタごとに追い込んだ値は cfg に保存できる（ゲーム別 cfg でも可）。固定プリセットとしてチェイン既定に焼き込むことも可能。
- `beam_peak_nits` は「1本の絶対輝度」なので、`beam_width_*` や `intensity_overdrive` 等（線の太さ/コア強調）とは独立。明るさは `beam_peak_nits`、太さは幅系、白飛び具合はロールオフ、という役割分担で調整するとよい。
- `vector-color`の1/64 Wide Glow拡張はnit較正を変更しない。遠いハローへの光量配分を変える視覚効果であり、通常線の基準nitは引き続き`beam_peak_nits`が決める。
