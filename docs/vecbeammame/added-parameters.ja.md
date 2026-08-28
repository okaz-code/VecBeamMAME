# 追加パラメータ一覧

English: [added-parameters.md](added-parameters.md)

**このファイルは `tools/dump-chain-sliders.py` が `bgfx/chains/vector/*.json` から生成しています。手で編集しないでください。**

スライダーは MAME のスライダーメニュー（既定では `Tab` → Sliders）から操作し、変更した値はゲームごとに `cfg/<game>.cfg` に保存されます。
**[M] が付くものはマクロ**で、複数のパラメータをまとめて動かします。マクロ以外は `Advanced Parameters` を On にすると出てきます（`Brightness Threshold (T)` だけは例外で、常に表示されます）。

起動時オプション（`-vector_*` など、ini ファイルに書くもの）は [startup-options.ja.md](startup-options.ja.md) を参照してください。

凡例: 値の列は `既定値 [下限, 上限] /刻み`。チェインによって違う場合はチェインごとに並べています。説明は 1 行の要約です。

## マクロ（Advanced Off でも見える）

| 内部名 | 表示名 | 説明 | 値 | チェイン | 駆動マクロ |
|---|---|---|---|---|---|
| `macro_exposure` | [M] Beam Brightness | ビームの明るさの総量。HDR では `beam_peak_nits`(ビーム核のピーク輝度 nits)、 SDR では `sdr_beam_level` を同じ倍率で動かすので、HDR / SDR どちらで見ていても 同じ操作で同じ方向に効く。 | 1 [0.25, 3] /0.05 | color/mono/vectrex | — |
| `bright_threshold` | Brightness Threshold (T) | 輝度が飽和する点。ここより上のドライブは幅に回る。color の既定 0.9 は「ほとんど飽和させない」、mono/vectrex の 0.5 は「半分から幅に回す」。 | color: 0.7 [0.05, 1] /0.01<br>mono: 0.5 [0.05, 1] /0.01<br>vectrex: 0.5 [0.05, 1] /0.01 | color/mono/vectrex | — |
| `macro_overload` | [M] Overload Amount | Z 軸を振り切った描画(爆発・弾)の「焼き付き感」の量。折れ線で 3 本を同時に動かす: `overload_threshold`(どのエネルギーから過大とみなすか。 | 1 [0, 2] /0.05 | color | — |
| `macro_bloom` | [M] Bloom Strength | グロー 3 系統（`glow_wide` / `glow_narrow` / `analytic_glow`）の一括倍率。1.0 が出荷値。 | 1 [0, 3] /0.05 | color/mono/vectrex | — |
| `macro_beam_width` | [M] Beam Width | 線の太さ。`beam_width_min` / `beam_width_max`(暗い線・明るい線それぞれの幅)と `overload_width_add`(過大時の追加幅)を同じ倍率で動かす。 | 1 [0.25, 3] /0.05 | color/mono/vectrex | — |
| `macro_point_size` | [M] Point Size | 点の大きさ。`point_width_scale` と、Vectrex では `isolated_dot_min_size` (孤立した停留ドットの最小サイズ px)。 | 1 [0.25, 3] /0.05 | color/mono/vectrex | — |
| `macro_point_bright` | [M] Point Brightness | 点の明るさ(`point_brightness_scale`)。点は面積が小さく同じ輝度でも埋もれるので、 大きさと別に持たせてある。 | 1 [0, 2] /0.05 | color/mono/vectrex | — |
| `macro_defocus` | [M] Defocus | フォーカスのボケ量。`defocus`(全面)、`edge_defocus`(偏向角による周辺の非点収差)、 `overload_bloom`(過大時のボケ)を同じ倍率で動かす。 | 1 [0, 3] /0.05 | color/mono/vectrex | — |
| `macro_persistence` | [M] Phosphor Persistence | 残光の長さ。`phosphor_half_ms`(半減期)と `phosphor_total_ms`(完全に消えるまで)を 同じ倍率で動かすので、減衰カーブの形は保ったまま時間軸だけ伸縮する。 | 1 [0.1, 4] /0.05 | color/mono/vectrex | — |
| `macro_monitor_sim` | [M] Monitor/Glass Sim | 0/1 のトグル。管面・ガラス・ベゼルという「ビーム以外の光学系」を丸ごと切る。 | 1 [0, 1] /1 | color/mono/vectrex | — |
| `macro_bezel` | [M] Bezel Reflection | ベゼル(管面の縁の外側)がどれだけ光を返すかの量。既定 0 = オフ、1.0 で `bezel_glow_strength` / `monitor_bezel_reflection` がそれぞれの JSON 既定値になる。 | 0 [0, 3] /0.05 | color/mono/vectrex | — |
| `macro_beam_sim` | [M] Beam/Supply Sim | 0/1 のトグル。電源と偏向系の非理想性を丸ごと切る。ビームジッタ、HV 垂下、 RGB 別フリッカ深度、ビーム時間窓。 | 1 [0, 1] /1 | color | — |
| `macro_halation` | [M] Halation Amount | ハレーション(管面内部での反射リング)とスターバースト(放射状の筋)の一括倍率。 | mono: 0 [0, 5] /0.05<br>vectrex: 1 [0, 5] /0.05 | mono/vectrex | — |
| `advanced_sliders` | Advanced Parameters | On にすると以下の全詳細パラメータがメニューに出る。マクロだけで運用するなら Off のまま。 | 0 [0, 1] /1 | color/mono/vectrex | — |

## Phosphor persistence and decay（蛍光体の残光と減衰）

| 内部名 | 表示名 | 説明 | 値 | チェイン | 駆動マクロ |
|---|---|---|---|---|---|
| `phosphor_half_ms` | Phosphor Half-life (ms) | 残光が半分の明るさになるまでの時間。残光の体感的な長さを決める主役。色チェインの既定 16ms は AVG のカラー管、mono の 24ms は P4 系、 Vectrex の 16ms は実測に合わせた値。 | 16 [1, 300] /1 | color/mono/vectrex | 倍率: [M] Phosphor Persistence |
| `phosphor_hold_ms` | Phosphor Hold (ms, full brightness) | 減衰を始めるまでフル輝度で保持する時間。半減期を短く較正すると(トレイルらしい見え方になる)、1 present 分の間隔だけで 残光が 27% 程度まで落ちてしまい、ゆっくり動く明るい線の連続位置の間に | color: 14 [0, 80] /1<br>mono: 16 [0, 80] /1<br>vectrex: 16 [0, 80] /1 | color/mono/vectrex | — |
| `phosphor_curve` | Phosphor Decay Curve | 減衰カーブの形(ヒル指数 p)。大きいほど「しばらく明るく、その後急に落ちる」。 | color: 5 [0.4, 8] /0.05<br>mono: 3 [0.4, 4] /0.05<br>vectrex: 3 [0.4, 4] /0.05 | color/mono/vectrex | — |
| `phosphor_total_ms` | Phosphor Total (ms) | 完全に 0 になるまでの時間。半減期との比が実質的なカーブの伸びを決める。mono の既定 800ms は長残光管の想定。 | color: 100 [20, 1500] /10<br>mono: 800 [20, 1500] /10<br>vectrex: 500 [20, 1500] /10 | color/mono/vectrex | 倍率: [M] Phosphor Persistence |
| `phosphor_hit_reset` | Phosphor Hit Reset Floor | 蛍光体を再励起と見なす下限。これ以上の新しい光で age が 0 に戻る。 | 0.02 [0, 0.5] /0.005 | color | — |
| `phosphor_weak_hit_composite` | Phosphor Weak Hit Composite | 弱い再描画が、より明るく残っている残光を暗くしないようにする。 | 1 [0, 1] /1 | color | — |
| `phosphor_rgb_combination` | Phosphor RGB Combination Brightness | RGB が同じ場所で光ったときの合成の明るさ。0 でピーク正規化、1 で物理加算。 | 1 [0, 2] /0.05 | color | — |
| `phosphor_rgb_combination_width` | Phosphor RGB Combination Width | RGB 合成時に線が太って見える量。 | 0.15 [0, 1] /0.01 | color | — |
| `phosphor_energy_decay` | Phosphor Energy Decay (bright faster) | 2 相減衰。通常分(≤1)は基本半減期で、過大分(>1)はこの係数の倍だけ速く減衰し、 和を取る。 | color: 4 [0, 4] /0.05<br>mono: 3 [0, 4] /0.05<br>vectrex: 0 [0, 4] /0.05 | color/mono/vectrex | — |
| `phosphor_rgb_decay` | Phosphor RGB Decay (halflife x) | 蛍光体の残光時間を RGB 別に変える倍率。青は短く緑は長い、を表現する。 | [1.15, 1.05, 0.95] [[0.2, 0.2, 0.2], [3.0, 3.0, 3.0]] /0.05 | color | — |
| `phosphor_color` | Phosphor Color  | 蛍光体の発光色。mono の `[0.9, 0.9, 1]` はやや青い P4、 Vectrex の `[0.5, 0.7, 1]` は実機の青緑寄りの管。 | color: [1.0, 1.0, 1.0] [[0.0, 0.0, 0.0], [1.0, 1.0, 1.0]] /0.01<br>mono: [0.9, 0.9, 1.0] [[0.0, 0.0, 0.0], [1.0, 1.0, 1.0]] /0.01<br>vectrex: [0.3, 0.6, 1.0] [[0.0, 0.0, 0.0], [1.0, 1.0, 1.0]] /0.01 | color/mono/vectrex | — |

## Brightness transfer（輝度トランスファ）

| 内部名 | 表示名 | 説明 | 値 | チェイン | 駆動マクロ |
|---|---|---|---|---|---|
| `bright_sigmoid` | Brightness Sigmoid | べき乗カーブの上にさらに掛けるS字整形。正 = コントラストが強く(パキッと)、 負 = 緩く(なめらかに)、0 = オフ。 | color: -2 [-4, 4] /0.1<br>mono: 0 [-4, 4] /0.1<br>vectrex: 0.4 [-4, 4] /0.1 | color/mono/vectrex | — |
| `bright_sigmoid_center` | Brightness Sigmoid Center | S 字の変曲点。vectrex の 0.8 は「明るい側でだけ効かせる」設定。 | color: 0.4 [0.05, 0.95] /0.01<br>mono: 0.4 [0.05, 0.95] /0.01<br>vectrex: 0.8 [0.05, 0.95] /0.01 | color/mono/vectrex | — |

## Beam width（ビーム幅）

| 内部名 | 表示名 | 説明 | 値 | チェイン | 駆動マクロ |
|---|---|---|---|---|---|
| `beam_width_min` | Beam Width Minimum | 暗い線と明るい線それぞれの幅(px 相当)。エネルギーがこの 2 値の間を動く。 | color: 3.7 [0.1, 24] /0.05<br>mono: 2 [0.1, 24] /0.05<br>vectrex: 0.15 [0.1, 24] /0.05 | color/mono/vectrex | 倍率: [M] Beam Width |
| `beam_width_max` | Beam Width Maximum | 暗い線と明るい線それぞれの幅(px 相当)。エネルギーがこの 2 値の間を動く。 | color: 4 [0.1, 24] /0.1<br>mono: 2 [0.1, 24] /0.1<br>vectrex: 0.8 [0.1, 24] /0.1 | color/mono/vectrex | 倍率: [M] Beam Width |
| `width_curve` | Width Curve | エネルギー → 幅の応答の曲げ。1 超 = 中間域が細く最大への到達が遅い、 1 未満 = 中間域が太い、1.0 = 線形。 | color: 2 [0.2, 4] /0.05<br>mono: 1 [0.2, 4] /0.05<br>vectrex: 1 [0.2, 4] /0.05 | color/mono/vectrex | — |
| `core_flat` | Line Core Flatness | 幅とボケの結合を切る。通常 sigma = width/3.2 でボケが幅に連動するため、 幅を持ち上げられた(オーバードライブされた)ビームが1 本の広い柔らかい塊になってしまう。 | color: 0.4 [0, 0.98] /0.05<br>mono: 0.5 [0, 0.98] /0.05<br>vectrex: 0.5 [0, 0.98] /0.05 | color/mono/vectrex | — |
| `width_knee` | Width at Threshold | しきい値 T の時点で到達している幅の割合。T までは緩く、T を超えると急に太る。 | 0.8 [0, 1] /0.01 | mono/vectrex | — |
| `width_sigmoid` | Width Sigmoid | 幅カーブの上に掛ける S 字整形。正で急峻、負でなめらか、0 でオフ。 | 0 [-4, 4] /0.1 | mono/vectrex | — |
| `width_sigmoid_center` | Width Sigmoid Center | 幅の S 字整形の変曲点。 | 0.5 [0.05, 0.95] /0.01 | mono/vectrex | — |
| `beam_width_overmax` | Beam Width Over-Max (lift) | しきい値を超えたエネルギーが幅をどこまで太らせるか。min→max 幅の何倍まで足すか。 | 0.5 [0, 32] /0.5 | mono/vectrex | — |
| `width_over_curve` | Width Overload Curve | しきい値を超えた領域の幅の伸び方。1.0 で直線、大きいほど後半で急に太る。 | 1 [0.2, 4] /0.05 | mono/vectrex | — |
| `halo_quad_extent` | Halo Quad Extent (sigma, perf) | 解析ライン 1 本を描くクアッドの大きさ（σ の何倍まで）。小さいほど速いが広いハローが切れる。 | 2.5 [2, 3.5] /0.1 | color/mono/vectrex | — |

## Points and dots（点・ドット）

| 内部名 | 表示名 | 説明 | 値 | チェイン | 駆動マクロ |
|---|---|---|---|---|---|
| `line_point_threshold` | Line/Point Threshold (px) | これより短い線分を「点」として扱う境界(px)。単位はレイアウト / 向きの変換後で、 元の線分の実長を保った座標系。 | 2 [0, 8] /0.5 | color/mono/vectrex | — |
| `point_width_scale` | Point Width Scale | 点の幅倍率。color の 1.8 は「点を線より太く見せる」較正、mono の 0.6 は逆に細く。 | color: 1.8 [0.05, 4] /0.05<br>mono: 0.6 [0.05, 4] /0.05<br>vectrex: 1.3 [0.05, 4] /0.05 | color/mono/vectrex | 倍率: [M] Point Size |
| `point_brightness_scale` | Point Brightness Scale | 点の輝度倍率。点は面積が小さいので同じ輝度では埋もれる。それを補う。 | color: 2 [0, 4] /0.05<br>mono: 2.25 [0, 4] /0.05<br>vectrex: 2.25 [0, 4] /0.05 | color/mono/vectrex | 倍率: [M] Point Brightness |
| `point_roundness` | Point Roundness | 点の丸み。0 = 線と同じ断面、1 = 完全な円。 | color: 0.25 [0, 1] /0.05<br>mono: 0.4 [0, 1] /0.05<br>vectrex: 0.4 [0, 1] /0.05 | color/mono/vectrex | — |
| `vertex_dwell` | Vertex Dwell (corner dots) | 線の折れ点でビームが減速することによる頂点の明るさ。実機では角が明るく見える。color の既定 0.5、mono/vectrex は 0(それぞれ別の機構で扱う)。 | color: 0.5 [0, 1] /0.05<br>mono: 0 [0, 1] /0.05<br>vectrex: 0 [0, 1] /0.05 | color/mono/vectrex | — |
| `cap_no_persist` | Short-Dwell Dots: No Persistence | 1 のとき、短滞留のジャンクションドットを残光プールに入れず、専用の no-persist FBO に描く。 | color: 0 [0, 1] /1<br>mono: 1 [0, 1] /1<br>vectrex: 1 [0, 1] /1 | color/mono/vectrex | — |
| `dot_no_persist_dwell` | Dot No-Persist Dwell (us) | 上の判定に使う滞留時間の閾値(µs)。これ未満の滞留を「残光なし」に回す。 | color: 10 [0, 60] /1<br>mono: 20 [0, 60] /1<br>vectrex: 0 [0, 60] /1 | color/mono/vectrex | — |
| `z_rise_tau` | Z Rise Time (us) | Z 軸増幅器の立ち上がり時間。滞留が短い描画は Z が上がりきる前に終わるので暗い。 | color: 0.2 [0, 20] /0.01<br>mono: 0 [0, 20] /0.01<br>vectrex: 10 [0, 20] /0.01 | color/mono/vectrex | — |
| `isolated_dot_min_size` | Isolated Dwell Dot Minimum (px) | 孤立した滞留ドットの最小サイズ（px）。小さすぎる点が消えるのを防ぐ。 | 3.9 [0, 16] /0.1 | vectrex | 倍率: [M] Point Size |

## Focus and defocus（フォーカス／ボケ）

| 内部名 | 表示名 | 説明 | 値 | チェイン | 駆動マクロ |
|---|---|---|---|---|---|
| `defocus` | Defocus (beam blur),  | X/Y 別のボケ量(vec2)。管面全体に一律にかかる。電子ビームのスポット径そのもの。 | color: [0.3, 0.3] [[0.0, 0.0], [2.0, 2.0]] /0.1<br>mono: [0.2, 0.2] [[0.0, 0.0], [2.0, 2.0]] /0.1<br>vectrex: [0.4, 0.4] [[0.0, 0.0], [2.0, 2.0]] /0.1 | color/mono/vectrex | 倍率: [M] Defocus |
| `edge_defocus` | Edge Defocus | 偏向角による非点収差。画面中心から離れるほどビームが斜めに当たるので、 スポットが楕円に伸びてボケる。 | color: 0.1 [0, 8] /0.1<br>mono: 0.2 [0, 8] /0.1<br>vectrex: 0.1 [0, 8] /0.1 | color/mono/vectrex | 倍率: [M] Defocus<br>0/1: [M] Monitor/Glass Sim |
| `edge_defocus_curve` | Edge Defocus Curve | 中心からの距離に対する効き方の曲げ。大きいほど「中心付近は効かず、端で急に効く」。 | 3 [0.5, 4] /0.1 | color/mono/vectrex | — |

## Line ends（線端＝ブランキング遷移）

| 内部名 | 表示名 | 説明 | 値 | チェイン | 駆動マクロ |
|---|---|---|---|---|---|
| `line_cap_width` | Line End Width (x body) | 端の幅を本体の幅に対する倍率で指定。1 超で端が膨らむ(実機の端の丸まり)。 | color: 1.1 [0.1, 4] /0.05<br>mono: 1.25 [0.1, 4] /0.05<br>vectrex: 1.25 [0.1, 4] /0.05 | color/mono/vectrex | — |
| `line_cap_overload_add` | Line End Overload Add (px) | 過大駆動時に端へ追加される幅(px)。爆発の線の端が特に膨らむ現象。 | color: 2 [0, 12] /0.1<br>mono: 1 [0, 12] /0.1<br>vectrex: 1 [0, 12] /0.1 | color/mono/vectrex | — |
| `line_cap_overload_curve` | Line End Overload Curve | その追加分の曲げ。color の 0.25 と mono/vectrex の 4 で正反対の設定になっている (color は早く効かせ、mono/vectrex は後半で効かせる)。 | color: 0.25 [0.25, 8] /0.25<br>mono: 4 [0.25, 8] /0.25<br>vectrex: 4 [0.25, 8] /0.25 | color/mono/vectrex | — |
| `line_cap_transition` | Line End Transition (px) | 本体から端へ遷移する距離(px)。長いほど端がなだらかに膨らむ。 | color: 10 [0.5, 64] /0.5<br>mono: 8 [0.5, 64] /0.5<br>vectrex: 8 [0.5, 64] /0.5 | color/mono/vectrex | — |
| `line_cap_curve` | Line End Transition Curve | その遷移の曲げ。 | color: 0.5 [0.25, 4] /0.05<br>mono: 1.5 [0.25, 4] /0.05<br>vectrex: 1.5 [0.25, 4] /0.05 | color/mono/vectrex | — |
| `line_cap_mode` | Line End Mode | 端の形状モード(0〜3)。color = 0、mono = 1、vectrex = 2 と全チェインで違う。 | color: 0 [0, 3] /1<br>mono: 1 [0, 3] /1<br>vectrex: 2 [0, 3] /1 | color/mono/vectrex | — |
| `cap_ramp_only` | Line End: RAMP termini only | 線端の処理を、ドライバが RAMP と印を付けた端点だけに限定する。 | 1 [0, 1] /1 | vectrex | — |

## Beam energy model（ビームエネルギーモデル）

| 内部名 | 表示名 | 説明 | 値 | チェイン | 駆動マクロ |
|---|---|---|---|---|---|
| `energy_model` | Energy Model (speed/dwell) | 0 = オフ。`n` は表示輝度そのもの(素の挙動)。1 = 速度 / 滞留からの導出を有効化。 | 1 [0, 1] /1 | color/mono/vectrex | — |
| `energy_infl` | Energy Influence (no-current src) | 導出モデルの効きの強さ。0 = 表示輝度のみ、1 = 完全にモデル任せ。「電流情報を持たないソース」(AVG/DVG など、Z の値しか無いもの)向け。 | color: 1 [0, 1] /0.05<br>mono: 0.6 [0, 1] /0.05<br>vectrex: 0.6 [0, 1] /0.05 | color/mono/vectrex | — |
| `energy_speed_norm` | Energy Speed Norm (scr/ms) | 速度の正規化基準(画面幅 / ms)。ビームが速いほど 1 点あたりの滞留が短く暗くなる、 その換算係数。 | color: 3 [0.05, 12] /0.05<br>mono: 0.6 [0.05, 12] /0.05<br>vectrex: 0.6 [0.05, 12] /0.05 | color/mono/vectrex | — |
| `energy_curve` | Energy Curve | 速度 → エネルギーの曲げ。1.0 = 線形。 | color: 2.5 [0.2, 4] /0.05<br>mono: 1 [0.2, 4] /0.05<br>vectrex: 1 [0.2, 4] /0.05 | color/mono/vectrex | — |
| `energy_line_max` | Energy Line Max / Density Cap | 線のエネルギー上限(蛍光体の面積あたり飽和)。color の 1.2 は「ほぼ飽和させない」、 mono/vectrex の 4 は余裕を持たせた設定。 | color: 1.2 [1, 8] /0.1<br>mono: 4 [1, 8] /0.1<br>vectrex: 4 [1, 8] /0.1 | color/mono/vectrex | — |
| `energy_dot_ref` | Energy Dot Ref (us) | 停留ドットの滞留正規化基準(µs)。既定 30µs。これ以下は `I × dt` が線形に効く。 | 30 [2, 300] /1 | color/mono/vectrex | — |
| `energy_dot_curve` | Energy Dot Curve | 停留飽和の指数。既定 1.6(線形の 1.0 より滞留を強く効かせる)。 | 1.6 [0.2, 4] /0.05 | color/mono/vectrex | — |
| `energy_dot_max` | Energy Dot Max / Density Cap | 停留ドットのエネルギー上限。線の上限とは独立(既定 3.2 対 線の 1.2〜4)。 | 3.2 [1, 16] /0.1 | color/mono/vectrex | — |
| `energy_stroke_agg` | Energy Stroke Aggregation | 1 のとき、ストローク全体(RAMP-on から RAMP-off までの連続区間)の平均速度を セグメントごとの速度の代わりに使う。 | 1 [0, 1] /1 | color/mono/vectrex | — |
| `energy_dwell_cap` | Energy Dwell Cap | 同じ場所に何度も点を打ったときのエネルギーの積み上がり上限。 | 1.6 [0, 16] /0.1 | vectrex | — |
| `energy_obj_lift` | Energy Object Lift | オブジェクト（弾・爆発など）のエネルギー持ち上げ量。0 でオフ。 | 1 [0, 1] /1 | vectrex | — |
| `energy_obj_knee` | Energy Object Knee | オブジェクト持ち上げが効き始めるエネルギー。 | 0.75 [0, 1] /0.01 | vectrex | — |
| `energy_obj_sharp` | Energy Object Sharpness | オブジェクト持ち上げの立ち上がりの鋭さ。大きいほど閾値から急に光る。 | 2 [0.1, 8] /0.1 | vectrex | — |
| `energy_obj_max` | Energy Object Max | オブジェクト持ち上げの上限。 | 3 [1, 8] /0.1 | vectrex | — |
| `energy_obj_star` | Energy Object Star Boost | 星（孤立した点）に対する追加の持ち上げ。 | 1.5 [1, 4] /0.05 | vectrex | — |

## Overload and overdrive（過大入力とオーバードライブ）

| 内部名 | 表示名 | 説明 | 値 | チェイン | 駆動マクロ |
|---|---|---|---|---|---|
| `overload_threshold` | Overload Threshold | 過大とみなすエネルギーの下限。下げるとより多くが過大扱いになる。color 1 / mono 0.55 / vectrex 0.3 と大きく違うのは、それぞれのソースの エネルギー分布が違うため。 | color: 1 [0.1, 4] /0.05<br>mono: 0.55 [0.1, 4] /0.05<br>vectrex: 0.3 [0.1, 4] /0.05 | color/mono/vectrex | 折れ線: [M] Overload Amount |
| `overload_ramp` | Overload Ramp (n span) | 閾値から最大効果までのエネルギー幅。狭いと「ある点から急に焼ける」、 広いとなだらかに移行する。 | color: 0.75 [0, 4] /0.05<br>mono: 1 [0, 4] /0.05<br>vectrex: 1 [0, 4] /0.05 | color/mono/vectrex | — |
| `intensity_overdrive` | Overdrive (hot core) | 過大分による核の輝度の上乗せ。0 で完全にオフ。 | 1 [0, 4] /0.05 | color/mono/vectrex | 折れ線: [M] Overload Amount |
| `intensity_overdrive_curve` | Overdrive Curve | その上乗せの曲げ。mono/vectrex の既定 2 は「後半で急に効く」設定。 | color: 1 [0.25, 8] /0.05<br>mono: 2 [0.25, 8] /0.05<br>vectrex: 2 [0.25, 8] /0.05 | color/mono/vectrex | — |
| `mask_overdrive_flare` | Mask Overdrive Hot Core | シャドウマスクを通した後の白熱コアの強さ。 | 1 [0, 1] /1 | color | — |
| `overload_dot_gain` | Overdrive Dot Gain | 停留ドット優遇。止まったビーム(長さ 0 の点)はエネルギーを 1 点に集中させるので、 同じ `n` でも掃引された線やテキストのストロークよりずっと熱く見える。 | 2 [1, 8] /0.1 | color/mono/vectrex | — |
| `overload_max` | Overdrive Max (x peak) | オーバードライブの上限(ピークの倍数)。既定 3。 | 3 [0, 8] /0.1 | color/mono/vectrex | — |
| `overdrive_core` | Overdrive to Core | 過大分を「フレアだけ」に出すか「核そのもの」にも出すか。0 = フレアのみ(従来挙動)。 | color: 0.5 [0, 1] /0.05<br>mono: 1 [0, 1] /0.05<br>vectrex: 0 [0, 1] /0.05 | color/mono/vectrex | — |
| `overload_bloom` | Overload Defocus (blur) | 過大時の追加ボケ。[M] Defocus の倍率下。 | color: 0.2 [0, 4] /0.05<br>mono: 0.7 [0, 4] /0.05<br>vectrex: 0.3 [0, 4] /0.05 | color/mono/vectrex | 倍率: [M] Defocus |
| `overload_width_add` | Overload Core Width Add (px) | オーバーロード時に芯へ足す幅（px）。 | color: 5 [0, 12] /0.1<br>vectrex: 1.7 [0, 12] /0.1 | color/vectrex | 倍率: [M] Beam Width |
| `overload_width_steepness` | Overload Width Steepness | オーバーロード幅の立ち上がりの鋭さ。 | 6 [1, 20] /0.5 | color/vectrex | — |
| `overload_width_center` | Overload Width Center | オーバーロード幅の立ち上がりの中心。 | color: 0.2 [0.05, 0.99] /0.01<br>vectrex: 0.65 [0.05, 0.99] /0.01 | color/vectrex | — |
| `overlap_white_strength` | Overlap White Strength | 線が重なった場所が白熱する強さ。1 本の高輝度ではなく複数本の重なりにだけ効く。 | 1 [0, 1] /0.05 | color | — |
| `overlap_white_count` | Overlap White Count | 白熱と見なすのに必要な重なり本数の目安。 | 39 [1, 50] /0.25 | color | — |
| `overlap_white_brightness` | Overlap White Brightness | 重なり白熱の明るさ。 | 1.5 [0, 2] /0.05 | color | — |
| `overlap_white_spread` | Overlap White Spread | 重なり白熱の広がり。 | 1.25 [0, 12] /0.25 | color | — |
| `overload_display_compression` | Overload Display Compression | オーバーロードしたエネルギーを表示側で圧縮する量。 | 1 [0, 1] /0.05 | color | — |
| `phosphor_overdrive` | Overdrive (white at peak) | ピークで白に振り切る量。強い励起が色を失って白熱する挙動。 | 0.5 [0, 1] /0.05 | mono | 倍率: [M] Halation Amount |
| `overdrive_knee` | Overdrive Knee/Ceiling (xpeak)  | オーバードライブの膝／天井（ピークの倍数）。 | [0.6, 0.6] [[0.0, 0.0], [4.0, 4.0]] /0.05 | mono | — |
| `overdrive_sat_curve` | Overdrive Saturation Curve | オーバードライブで彩度が抜けていくカーブ。 | 1 [0.2, 4] /0.05 | mono | — |
| `overdrive_color` | Overdrive Color  | オーバードライブ時の色。振り切った描画がどの色へ寄っていくか。 | [1.0, 1.0, 1.0] [[0.0, 0.0, 0.0], [1.0, 1.0, 1.0]] /0.01 | mono | — |
| `overload_width_bloom_link` | Overload Width/Bloom Link | オーバーロードの幅とブルームの連動の強さ。 | 0 [0, 1] /1 | vectrex | — |

## Glow and bloom（グロー／ブルーム）

| 内部名 | 表示名 | 説明 | 値 | チェイン | 駆動マクロ |
|---|---|---|---|---|---|
| `analytic_glow` | Glow Source Strength (all vectors) | 解析線由来のグロー強度。全ベクターにかかる(過大かどうかを問わない)。このチェインで最も効く 1 本。 | color: 0.095 [0, 1] /0.005<br>mono: 0.034 [0, 1] /0.001<br>vectrex: 0.065 [0, 1] /0.005 | color/mono/vectrex | 倍率: [M] Bloom Strength |
| `analytic_glow_width` | Glow Source Radius (all vectors) | その半径(px)。mono の上限が 160 と広いのは長残光管の拡散を表現するため。 | color: 20 [1, 80] /1<br>mono: 15 [1, 160] /1<br>vectrex: 15 [1, 160] /1 | color/mono/vectrex | — |
| `glow_narrow` | Glow Narrow Strength | 狭いブルーム段の強度。既定が 0.003〜0.005 と極小なのは、 ブルームが加算で効くため少量で十分だから。 | color: 0.004 [0, 0.2] /0.001<br>mono: 0.002 [0, 0.2] /0.001<br>vectrex: 0.004 [0, 0.2] /0.001 | color/mono/vectrex | 倍率: [M] Bloom Strength |
| `overload_core_gain` | Overload Hot Core Gain | オーバーロード時の白熱コアの強さ。直接発光として芯に足される分。 | 0.015 [0, 0.5] /0.005 | color | — |
| `glow_wide` | Glow Wide (low-res) | 広いブルーム段。低解像度ターゲットで計算するので安い。既定は mono で 0.00018 と極小。 | color: 0.003 [0, 0.02] /0.0001<br>mono: 0.00013 [0, 0.02] /1e-05<br>vectrex: 0.0002 [0, 0.02] /0.0001 | color/mono/vectrex | 倍率: [M] Bloom Strength |
| `glow_wide_reach` | Glow Wide Reach | 広域グローの届く距離。ピラミッドを何段まで使うか。 | 8 [0, 16] /0.25 | color | — |
| `glow_wide_pivot` | Glow Wide Pivot (source level) | 広域グロー整形の基準レベル。このレベルは動かず、上下だけが曲がる。 | 1 [0.05, 4] /0.05 | color | — |
| `glow_wide_curve` | Glow Wide Curve (above pivot up) | 広域グローの整形カーブ。1 より大きいと暗いグローを潰し、明るいところだけ残す。 | 0.8 [0.5, 3] /0.05 | color | — |
| `glow_wide_smooth` | Glow Wide Downsample | 広域グローのダウンサンプル時のなめらかさ。 | 1 [0, 2] /1 | color | — |
| `overload_glow_gain` | Overload Glow (bloom) | オーバーロード時に追加されるグローの量。 | 1.06 [0, 2] /0.02 | color | 折れ線: [M] Overload Amount |
| `overload_glow_width` | Overload Glow Width (px) | オーバーロード時のグローの広がり（px）。 | 16 [4, 200] /2 | color | — |
| `glow_tail_curve` | Glow Tail Curve | グローの裾の落ち方。1 未満(mono の 0.6)は裾が長く残り、 1 超(color の 1.44)は早く落ちる。 | color: 1.44 [0.4, 2.5] /0.02<br>mono: 0.6 [0.4, 2.5] /0.02<br>vectrex: 1.82 [0.4, 2.5] /0.02 | color/mono/vectrex | — |
| `glow_fbo_scale` | Glow FBO Scale (perf) | グロー FBO の解像度倍率。性能パラメータ。0.4 なら面積 16%。グローはもともとぼやけているので落としても見た目の劣化が小さい。 | color: 0.4 [0.1, 1] /0.05<br>mono: 0.5 [0.1, 1] /0.05<br>vectrex: 0.4 [0.1, 1] /0.05 | color/mono/vectrex | — |
| `glow_black_toe` | Glow Black Toe | グローの暗部を切る足切り。これ未満の弱いグローを消して黒を締める。 | 0.002 [0, 0.05] /0.001 | mono/vectrex | — |

## Halation and starburst（ハレーション／スターバースト）

| 内部名 | 表示名 | 説明 | 値 | チェイン | 駆動マクロ |
|---|---|---|---|---|---|
| `halation_gain` | Halation Gain | ハレーション全体のゲイン。mono の上限が 5 なのは、[M] Halation Amount の上限を 3 → 5 に上げたときこちらが真の天井だったため合わせて上げた。 | mono: 1 [0, 5] /0.001<br>vectrex: 0.66 [0, 2] /0.001 | mono/vectrex | 倍率: [M] Halation Amount |
| `ring_over_gain` | Halation from Overdrive | オーバードライブ量に対するリングの反応。上限 20 と大きいのは、 爆発だけに強くリングを出したいケースがあるため。 | 1 [0, 20] /0.05 | mono/vectrex | — |
| `ring_gain` | Halation Rim | リングの輪郭の強さ。 | mono: 0.005 [0, 0.2] /0.001<br>vectrex: 0.033 [0, 0.2] /0.001 | mono/vectrex | 倍率: [M] Halation Amount |
| `ring_fill` | Halation Fill | リングの内側の塗りの強さ。輪郭と内側を分離してあるのは、 実機では「細い明るい輪」と「内側の淡い円盤」が別に見えるため。 | mono: 0.02 [0, 0.2] /0.001<br>vectrex: 0.077 [0, 0.2] /0.001 | mono/vectrex | 倍率: [M] Halation Amount |
| `ring_radius` | Halation Radius (px) | リングの半径。mono 44px / vectrex 30px。ガラスの厚みに相当する。 | mono: 44 [4, 80] /1<br>vectrex: 30 [4, 80] /1 | mono/vectrex | — |
| `ring_width` | Halation Width (px) | リングの太さ。 | mono: 3 [0.75, 12] /0.25<br>vectrex: 1.25 [0.75, 12] /0.25 | mono/vectrex | — |
| `ring_min_dwell` | Halation Min Dwell (us) | リングを出すのに必要な最小滞留(µs)。既定 20µs。 | 20 [0, 200] /1 | mono/vectrex | — |
| `ring_threshold` | Halation Threshold (bright) | 輝度側の門。既定 0(輝度では絞らない)。 | 0 [0, 2] /0.02 | mono/vectrex | — |
| `ray_gain` | Starburst Gain | 放射状の筋のゲイン。既定 0.005 前後と極小。 | mono: 0.005 [0, 0.5] /0.0001<br>vectrex: 0.0044 [0, 0.5] /0.0001 | mono/vectrex | 倍率: [M] Halation Amount |
| `ray_var` | Starburst Uneven | 筋ごとの強度のばらつき。実機の筋は均一でない。 | 0.6 [0, 1] /0.05 | mono/vectrex | — |
| `ray_count` | Starburst Rays | 筋の本数。既定 6。 | 6 [2, 12] /1 | mono/vectrex | — |
| `ray_length` | Starburst Length (px) | 筋の長さ。 | mono: 68 [8, 300] /2<br>vectrex: 50 [8, 300] /2 | mono/vectrex | — |
| `ray_length_rand` | Starburst Length Random | 時間変動するランダム化。長さと本数がフレームごとに揺れる。静止した固定パターンに見えるのを避ける。 | 0.1 [0, 1] /0.05 | mono/vectrex | — |
| `ray_count_rand` | Starburst Count Random | 時間変動するランダム化。長さと本数がフレームごとに揺れる。静止した固定パターンに見えるのを避ける。 | mono: 0.4 [0, 1] /0.05<br>vectrex: 0.15 [0, 1] /0.05 | mono/vectrex | — |
| `ray_width` | Starburst Width (px) | 筋の太さ。 | mono: 0.8 [0.5, 6] /0.1<br>vectrex: 0.5 [0.5, 6] /0.1 | mono/vectrex | — |
| `ray_angle` | Starburst Angle (deg) | 筋の基準角度。既定 15°。実機のガラス構造の向きに合わせる。 | 15 [0, 180] /1 | mono/vectrex | — |

## Edge glow (beam past the visible area)（画面外へ振れたビームの発光）

| 内部名 | 表示名 | 説明 | 値 | チェイン | 駆動マクロ |
|---|---|---|---|---|---|
| `edge_glow` | Edge Glow Gain | 発光の強さ。既定 0.75。 | 0.75 [0, 2] /0.01 | vectrex | — |
| `edge_glow_threshold` | Edge Glow Threshold | 発光に必要な最小エネルギー。既定 0(エネルギーでは絞らない)。実際に効かせるためにはエネルギー床・深さ上限・回転の扱いを直す必要があった。 | 0 [0, 10] /0.01 | vectrex | — |
| `edge_glow_sensitivity` | Edge Glow Sensitivity | 画面外への振れ幅に対する感度。深く出るほど強く光るが、その換算。 | 1 [0.01, 10] /0.01 | vectrex | — |
| `edge_glow_width` | Edge Glow Width (px) | 縁に沿った発光の幅(px)。既定 100 と広いのは、実機の縁の光が広く内側へ滲むため。 | 100 [4, 400] /2 | vectrex | — |
| `edge_glow_length` | Edge Glow Length (px) | 縁に沿った発光の長さ(px)。 | 150 [20, 600] /5 | vectrex | — |
| `edge_glow_persist` | Edge Glow Persistence (ms) | 時間方向の平滑化。既定 120ms。これが無いと、画面外へ出入りするたびに縁がフレーム単位で点滅する。 | 120 [0, 1000] /10 | vectrex | — |

## Convergence（コンバージェンス）

| 内部名 | 表示名 | 説明 | 値 | チェイン | 駆動マクロ |
|---|---|---|---|---|---|
| `convergence_global_gain` | Convergence Global Bloom | 位置ずれではなく「ずれによって生じる低彩度のにじみ」。3 原色が完全に重ならないため、 明るい部分の周囲に白っぽいにじみが出る。 | color: 0.01 [0, 0.5] /0.005<br>mono: 0 [0, 0.5] /0.005<br>vectrex: 0 [0, 0.5] /0.005 | color/mono/vectrex | 0/1: [M] Monitor/Glass Sim |
| `convergence_global_coverage` | Convergence Global Coverage | そのにじみが2 つの境界からなる環として出る範囲。厚いリング状のコンバージェンスを 表現するために、単純なぼかしではなく境界を 2 つ持たせている。 | color: 0.3 [0.1, 1.2] /0.025<br>mono: 0.55 [0.1, 1.2] /0.025<br>vectrex: 0.55 [0.1, 1.2] /0.025 | color/mono/vectrex | — |
| `converge_red` | Red Linear Convergence,  | 赤ビームの位置ずれ。画面全体を平行移動する量（px）。 | [0.0, 0.0] [[-10.0, -10.0], [10.0, 10.0]] /0.1 | color | — |
| `converge_green` | Green Linear Convergence,  | 緑ビームの位置ずれ。画面全体を平行移動する量（px）。 | [0.0, 0.0] [[-10.0, -10.0], [10.0, 10.0]] /0.1 | color | — |
| `converge_blue` | Blue Linear Convergence,  | 青ビームの位置ずれ。画面全体を平行移動する量（px）。 | [0.0, 0.0] [[-10.0, -10.0], [10.0, 10.0]] /0.1 | color | — |
| `radial_converge_red` | Red Radial Convergence,  | 赤ビームの放射方向のずれ。画面中心からの距離に比例して広がる／縮む。 | [0.0, 0.0] [[-0.1, -0.1], [0.1, 0.1]] /0.001 | color | — |
| `radial_converge_green` | Green Radial Convergence,  | 緑ビームの放射方向のずれ。画面中心からの距離に比例して広がる／縮む。 | [0.0, 0.0] [[-0.1, -0.1], [0.1, 0.1]] /0.001 | color | — |
| `radial_converge_blue` | Blue Radial Convergence,  | 青ビームの放射方向のずれ。画面中心からの距離に比例して広がる／縮む。 | [0.0, 0.0] [[-0.1, -0.1], [0.1, 0.1]] /0.001 | color | — |

## Beam time window（ビーム時間窓＝1プレゼントあたりの掃引スライス）

| 内部名 | 表示名 | 説明 | 値 | チェイン | 駆動マクロ |
|---|---|---|---|---|---|
| `beam_window` | Beam Time Window | 0/1。[M] Beam/Supply Sim でもゲートされる。 | 1 [0, 1] /1 | color/mono/vectrex | 0/1: [M] Beam/Supply Sim |
| `beam_window_scale` | Beam Time Window Rate (x real time) | 窓の幅を実時間に対する倍率で指定する。大きくすると 1 窓が広くなり、掃引を 覆いきってしまうと窓が意味を失う(`inert` と報告される)。 | color: 2 [0.25, 4] /0.05<br>mono: 1.5 [0.25, 4] /0.05<br>vectrex: 1.25 [0.25, 4] /0.05 | color/mono/vectrex | — |
| `beam_flash_ms` | Beam Strike Flash (ms) | 打たれた直後のピクセルを減衰カーブより明るく出す（蛍光体の初期速い成分）。`beam_flash_ms` が 0 なら完全に無処理。 | 1 [0, 40] /0.5 | color/mono/vectrex | — |
| `beam_flash_gain` | Beam Strike Flash Gain (x) | 打たれた直後のピクセルを減衰カーブより明るく出す（蛍光体の初期速い成分）。`beam_flash_ms` が 0 なら完全に無処理。 | 1.5 [1, 8] /0.1 | color/mono/vectrex | — |

## Flicker, HV droop and beam jitter（フリッカ・HV垂下・ビームジッタ）

| 内部名 | 表示名 | 説明 | 値 | チェイン | 駆動マクロ |
|---|---|---|---|---|---|
| `flicker_thresh_ms` | Cyclic Flicker Threshold (draw ms) | 1 掃引の描画時間がこれを超えるとフリッカが出始める。実機の AVG はベクター数が 多すぎると 1 フレームで描き切れず、次のフレームへ持ち越して明滅する。 | color: 24 [0, 300] /1<br>mono: 22 [0, 300] /1<br>vectrex: 12 [0, 300] /1 | color/mono/vectrex | — |
| `flicker_buckets` | Cyclic Flicker Buckets (N) | 持ち越しを何段階に分けるか。多いほど滑らかに、少ないほど段付きになる。 | color: 9 [2, 16] /1<br>mono: 6 [2, 16] /1<br>vectrex: 6 [2, 16] /1 | color/mono/vectrex | — |
| `flicker_period_ms` | Cyclic Flicker Step (ms) | 1 段あたりの時間。mono の 16.7ms は 60Hz の 1 フレーム。 | color: 20 [2, 100] /0.5<br>mono: 16.7 [2, 100] /0.5<br>vectrex: 16.7 [2, 100] /0.5 | color/mono/vectrex | — |
| `flicker_red_depth` | Cyclic Flicker Red Depth | 周期フリッカで赤チャンネルを何割落とすか。 | 1 [0, 1] /0.01 | color | 0/1: [M] Beam/Supply Sim |
| `flicker_green_depth` | Cyclic Flicker Green Depth | 周期フリッカで緑チャンネルを何割落とすか。 | 0.5 [0, 1] /0.01 | color | 0/1: [M] Beam/Supply Sim |
| `flicker_blue_depth` | Cyclic Flicker Blue Fine Depth | 周期フリッカで青チャンネルを何割落とすか。青蛍光体は残光が短いので深めに設定する。 | 0.1 [0, 0.1] /0.001 | color | 0/1: [M] Beam/Supply Sim |
| `hv_droop` | HV Droop (dim+defocus) | 垂下の強さ(暗化+デフォーカス)。0=無効。 | 0.5 [0, 1] /0.01 | color | 0/1: [M] Beam/Supply Sim |
| `hv_droop_dim` | HV Droop Dim (0=defocus only) | HV 垂下で輝度も落とすか。0 ならフォーカスの甘さだけに出る。 | 0 [0, 1] /0.05 | color | — |
| `hv_droop_onset` | HV Droop Overload Onset | HV 垂下が効き始める負荷。画面が明るいほど高圧が下がる、その閾値。 | 0.5 [0, 60] /0.5 | color | — |
| `hv_droop_ref` | HV Droop Load Ref | 負荷を 0..1 に正規化する基準。小さいほど早く飽和。 | 5 [1, 60] /0.5 | color | — |
| `beam_jitter` | Beam Jitter (energy + position) | 電源・偏向系の揺らぎ。ビームのエネルギーと位置を同時に微小に振る。0 でオフ。 | 0.1 [0, 1] /0.01 | color | 0/1: [M] Beam/Supply Sim |
| `beam_jitter_hz` | Beam Jitter Speed (Hz) | ビームジッタの速さ。低いとゆっくりうねり、高いとざらつく。 | 15 [1, 120] /1 | color | — |
| `beam_jitter_saturation_start` | Beam Jitter Saturation Start | ジッタが弱まり始めるエネルギー。明るい描画ほど揺れなくなる（電源が踏ん張る）。 | 1.5 [0.5, 6] /0.05 | color | — |
| `beam_jitter_saturation_range` | Beam Jitter Saturation Range | ジッタが弱まりきるまでのエネルギー幅。 | 1.5 [0.1, 6] /0.05 | color | — |
| `beam_jitter_saturation_curve` | Beam Jitter Saturation Curve | ジッタの弱まり方のカーブ。大きいほど急に落ちる。 | 2 [0.25, 8] /0.05 | color | — |

## Color adjustment（色調整）

| 内部名 | 表示名 | 説明 | 値 | チェイン | 駆動マクロ |
|---|---|---|---|---|---|
| `primary_color_mode` | Color Adjustment Mode | 0 = 色度座標(`chroma_*`)で指定、1 = HSB(`primary_*`)で指定。 | 1 [0, 1] /1 | color | — |
| `primary_red_hue` | Red Hue Shift (deg) | 赤の色相シフト(度)・彩度・明度。既定彩度 0.85 = sRGB より浅い。実機の赤蛍光体は sRGB 原色ほど飽和していない。 | 1 [-60, 60] /1 | color | — |
| `primary_red_saturation` | Red Saturation | 赤の色相シフト(度)・彩度・明度。既定彩度 0.85 = sRGB より浅い。実機の赤蛍光体は sRGB 原色ほど飽和していない。 | 0.95 [0, 2] /0.01 | color | — |
| `primary_red_brightness` | Red Brightness | 赤の色相シフト(度)・彩度・明度。既定彩度 0.85 = sRGB より浅い。実機の赤蛍光体は sRGB 原色ほど飽和していない。 | 1 [0, 2] /0.01 | color | — |
| `primary_green_hue` | Green Hue Shift (deg) | 緑。既定彩度 0.8。 | 0 [-60, 60] /1 | color | — |
| `primary_green_saturation` | Green Saturation | 緑。既定彩度 0.8。 | 0.9 [0, 2] /0.01 | color | — |
| `primary_green_brightness` | Green Brightness | 緑。既定彩度 0.8。 | 0.6 [0, 2] /0.01 | color | — |
| `primary_blue_hue` | Blue Hue Shift (Violet +) | 青。表示名が `Blue Hue Shift (Violet +)` で正方向が紫寄り。 | 1 [-60, 60] /1 | color | — |
| `primary_blue_saturation` | Blue Saturation | 青。表示名が `Blue Hue Shift (Violet +)` で正方向が紫寄り。 | 0.9 [0, 2] /0.01 | color | — |
| `primary_blue_brightness` | Blue Brightness | 青。表示名が `Blue Hue Shift (Violet +)` で正方向が紫寄り。 | 1.2 [0, 2] /0.01 | color | — |
| `chroma_a` | Phosphor A Chromaticity  | CIE xy 色度座標での原色指定(`primary_color_mode` 0 のとき)。 | [0.63, 0.34] [[0.0, 0.0], [1.0, 1.0]] /0.001 | color | — |
| `chroma_b` | Phosphor B Chromaticity  | CIE xy 色度座標での原色指定(`primary_color_mode` 0 のとき)。 | [0.31, 0.595] [[0.0, 0.0], [1.0, 1.0]] /0.001 | color | — |
| `chroma_c` | Phosphor C Chromaticity  | CIE xy 色度座標での原色指定(`primary_color_mode` 0 のとき)。 | [0.17, 0.07] [[0.0, 0.0], [1.0, 1.0]] /0.001 | color | — |
| `chroma_y_gain` | Phosphor Gain,  | 各原色の輝度寄与(Y)。既定 `[0.2124, 0.62, 0.1]` は Rec.709 の輝度係数に近い。 | [0.2124, 0.62, 0.1] [[0.0, 0.0, 0.0], [1.0, 1.0, 1.0]] /0.001 | color | — |

## Vector geometry（ベクター幾何）

| 内部名 | 表示名 | 説明 | 値 | チェイン | 駆動マクロ |
|---|---|---|---|---|---|
| `vector_linearity_x` | Vector Linearity X (gain) | 偏向の直線性(ゲイン)。1.0 が理想。実機は X と Y でわずかに違い、正方形が長方形に見える。 | 1 [0.8, 1.2] /0.005 | color/mono/vectrex | — |
| `vector_linearity_y` | Vector Linearity Y (gain) | 偏向の直線性(ゲイン)。1.0 が理想。実機は X と Y でわずかに違い、正方形が長方形に見える。 | 1 [0.8, 1.2] /0.005 | color/mono/vectrex | — |
| `vector_pincushion_x_quad` | Vector Pincushion X (Quad) | X 方向の非対称な糸巻き歪み(2 次)。0 でオフ。`tube_distortion` が管面の球面によるものなのに対し、これは偏向の非線形性。 | 0 [-1, 1] /0.01 | color/mono/vectrex | — |
| `vector_image_scale` | Vector Image Scale | ベクター像の倍率。`tube_face_scale` とは別物。管面(ガラス)の大きさに対して ベクター像がどれだけの範囲を占めるかで、実機で「管面の縁まで描かれていない」状態を表す。 | color: 0.94 [0.75, 1.15] /0.005<br>mono: 0.95 [0.75, 1.15] /0.005<br>vectrex: 0.96 [0.75, 1.15] /0.005 | color/mono/vectrex | 折れ線: [M] Monitor/Glass Sim |

## Shadow mask（シャドウマスク）

| 内部名 | 表示名 | 説明 | 値 | チェイン | 駆動マクロ |
|---|---|---|---|---|---|
| `shadow_mask_strength` | Shadow Mask Strength | シャドウマスクの濃さ。0 でマスクなし。[M] Monitor/Glass Sim で 0/1 ゲート。 | 0.3 [0, 1] /0.01 | color | 0/1: [M] Monitor/Glass Sim |
| `shadow_mask_scale` | Shadow Mask Size (px @1080p) | マスクピッチ(1080p 基準の px)。 | 0.65 [0.25, 8] /0.05 | color | — |
| `shadow_mask_brightboost` | Shadow Mask Brightness Boost | マスクで失われた輝度の補償。既定 0(補償しない)。 | 0 [0, 2] /0.05 | color | — |
| `masked_core_peak` | Masked Core Peak Limit | マスク越しに見える核のピーク制限。 | 1 [0, 4] /0.05 | color | — |
| `core_overlap_max` | Direct Core Overlap | 核の直接重なりの扱い(0/1)。 | 1 [0, 1] /1 | color | — |
| `ambient_mask` | Ambient Shadow-Mask (0=flat,1=masked) | 環境光にマスクを掛けるか。1 = マスク越し(実機では非励起の蛍光体もマスク越しに見える)、 0 = 平坦。 | 1 [0, 1] /0.05 | color | — |

## Tube, ambient and bezel（管面・環境光・ベゼル）

| 内部名 | 表示名 | 説明 | 値 | チェイン | 駆動マクロ |
|---|---|---|---|---|---|
| `tube_distortion` | Tube Quadric Distortion | 管面の球面による歪み(ピンクッション)。負値で樽型。 | color: 0.12 [-2, 2] /0.01<br>mono: 0.15 [-2, 2] /0.01<br>vectrex: 0.15 [-2, 2] /0.01 | color/mono/vectrex | 0/1: [M] Monitor/Glass Sim |
| `tube_round_corner` | Tube Rounded Corner | 管面の角丸。実機の管は角が丸い。 | color: 0.15 [0, 2] /0.01<br>mono: 0.2 [0, 2] /0.01<br>vectrex: 0.3 [0, 2] /0.01 | color/mono/vectrex | 0/1: [M] Monitor/Glass Sim |
| `tube_vignetting` | Tube Vignetting | 周辺光量落ち。 | 0.8 [0, 2] /0.01 | color/mono/vectrex | 0/1: [M] Monitor/Glass Sim |
| `tube_face_scale` | Tube Face Scale | 管面(ガラス面)の大きさをウィンドウに対する比で指定。`vector_image_scale` が その中でベクター像が占める範囲なので、2 つは入れ子の関係。 | color: 0.98 [0.8, 1] /0.005<br>mono: 0.99 [0.8, 1] /0.005<br>vectrex: 0.98 [0.8, 1] /0.005 | color/mono/vectrex | 折れ線: [M] Monitor/Glass Sim |
| `ambient_color` | Ambient Color  | 励起されていない蛍光体の素の色。実機の消えている管面は真っ黒ではなく灰緑や灰青。 | color: [0.35, 0.35, 0.35] [[0.0, 0.0, 0.0], [1.0, 1.0, 1.0]] /0.01<br>mono: [0.35, 0.45, 0.38] [[0.0, 0.0, 0.0], [1.0, 1.0, 1.0]] /0.01<br>vectrex: [0.35, 0.45, 0.38] [[0.0, 0.0, 0.0], [1.0, 1.0, 1.0]] /0.01 | color/mono/vectrex | — |
| `ambient_level` | Ambient Level (x0.001) | その明るさ(×0.001)。既定 1 = 0.001。 | color: 1 [0, 200] /0.5<br>mono: 1 [0, 200] /0.5<br>vectrex: 2 [0, 200] /0.5 | color/mono/vectrex | 0/1: [M] Monitor/Glass Sim |
| `mglow_coefficient` | Monitor Glow Coefficient | モニタグローの強さの係数。 | 0.4 [0, 10] /0.05 | color | — |
| `mglow_brightness` | Monitor Glow Brightness | 明るさ。同じ理由でゲート対象外。 | 0.15 [0, 2] /0.01 | color | — |
| `mglow_center_edge_diff` | Monitor Glow Center-Edge Diff | 中心と縁の明るさの差。同じ理由でゲート対象外。 | 0.1 [0, 1] /0.01 | color | — |
| `mglow_rgb_bands` | Monitor Glow RGB Bands | RGB のバンド分割。0 のときバンド項はちょうど 1 になり、グローは中立の色合いで出る。 | 0.35 [0, 1] /0.01 | color | 0/1: [M] Monitor/Glass Sim |
| `mglow_rgb_band_count` | Monitor Glow RGB Band Count | バンドの本数。0 が「オフ」を意味しないのでゲート対象外(ゲートすると形が変わってしまう)。 | 8 [3, 24] /1 | color | — |
| `mglow_min_distance` | Monitor Glow Min Distance (screen) | 画面外へどれだけ出たらグローが始まるか。 | 0.3 [0, 0.75] /0.01 | color | — |
| `mglow_coverage_start` | Monitor Glow Coverage Start | 画面の被覆率でゲートする。画面外へのビームが多いほど強く光る。start で効き始め、full で最大。 | 0.65 [0, 0.95] /0.01 | color | — |
| `mglow_coverage_full` | Monitor Glow Coverage Full | 画面の被覆率でゲートする。画面外へのビームが多いほど強く光る。start で効き始め、full で最大。 | 0.85 [0.05, 1] /0.01 | color | — |
| `bezel_glow_strength` | Bezel Reflection Glow | ベゼル(枠)の発光。[M] Monitor/Glass Sim でゲート。 | color: 2 [0, 8] /0.01<br>mono: 0.25 [0, 2] /0.01<br>vectrex: 0.25 [0, 2] /0.01 | color/mono/vectrex | 倍率: [M] Bezel Reflection<br>0/1: [M] Monitor/Glass Sim |
| `monitor_bezel_reflection` | Monitor Glow Bezel Reflection | モニタグロー由来の光がベゼルに返る割合。ビーム由来の反射とは別系統。 | 0.5 [0, 1] /0.01 | color | 折れ線: [M] Bezel Reflection<br>0/1: [M] Monitor/Glass Sim |
| `bezel_long_reflection` | Bezel Long-Line Reflection | 長いストロークがベゼルに返す光の割合。 | 1 [0, 2] /0.05 | color | — |
| `bezel_short_reflection` | Bezel Short-Line Reflection | 短いストロークがベゼルに返す光の割合。既定 0.1 は文字をベゼルに映さないための値。 | 0.1 [0, 1] /0.02 | color | — |
| `bezel_long_threshold` | Bezel Long-Line Threshold (px) | 長い／短いの境目（ウィンドウ px）。これより長いセグメントが long 扱い。 | 160 [40, 400] /10 | color | — |
| `bezel_glow_width` | Bezel Glow Width (px) | ベゼル発光の幅と落ち方。 | color: 100 [2, 200] /1<br>mono: 32 [2, 200] /1<br>vectrex: 32 [2, 200] /1 | color/mono/vectrex | — |
| `bezel_glow_curve` | Bezel Glow Curve | ベゼル発光の幅と落ち方。 | 2 [0.25, 4] /0.05 | color/mono/vectrex | — |
| `room_ambient` | Room Ambient (Overlay/Bezel) | 部屋の明るさ。オーバーレイとベゼルの見え方に効く。 | 0.25 [0, 2] /0.05 | vectrex | — |

## Printed overlay（印刷オーバーレイ＝Vectrex）

| 内部名 | 表示名 | 説明 | 値 | チェイン | 駆動マクロ |
|---|---|---|---|---|---|
| `overlay_white_transmission` | Overlay White Transmission | 白地部分の透過率。既定 0.65 = 実測値。 | 0.65 [0, 1] /0.01 | vectrex | — |
| `overlay_white_diffusion` | Overlay Resin Diffusion Strength | 白地の樹脂による拡散の強さ。ビームがオーバーレイを通ると滲む。 | 0.1 [0, 1] /0.01 | vectrex | — |
| `overlay_diffusion_radius` | Overlay Resin Diffusion Radius (px) | その拡散半径(px)。 | 12 [0, 16] /0.25 | vectrex | — |
| `overlay_diffusion_shape` | Overlay Resin Diffusion Curve | 拡散の形。既定 0.4 は「近くに集中して裾が短い」。 | 0.4 [0.2, 6] /0.05 | vectrex | — |
| `overlay_white_reflectance` | Overlay White Reflectance | 白地の反射率。室内光を反射してオーバーレイ自体が明るく見える成分。 | 0.25 [0, 1] /0.01 | vectrex | — |
| `overlay_color_density` | Overlay Color Optical Density | 色樹脂の光学濃度。大きいほど濃い色になり、通る光が減る。既定 1.75。 | 3 [0, 6] /0.05 | vectrex | — |
| `overlay_color_glow` | Overlay Rear Resin Scatter | 裏面の樹脂による散乱。色樹脂の裏側でも光が散る。既定 1.1。 | 1.1 [0, 2] /0.05 | vectrex | — |
| `overlay_color_dark_level` | Overlay Resin Dark Level | 色樹脂の暗部レベル。真っ黒にならない床。 | 0.15 [0, 1.6] /0.005 | vectrex | — |
| `overlay_color_highlight_bleach` | Overlay Highlight Color Release | 明るい部分で色が抜ける。強い光が色樹脂を通ると飽和して白く見える現象。`bleach` が抜ける量(既定 0 = オフ)、`knee` が始まる輝度、`curve` がその曲げ。 | 0 [0, 1] /0.01 | vectrex | — |
| `overlay_color_highlight_knee` | Overlay Highlight Release Knee | 明るい部分で色が抜ける。強い光が色樹脂を通ると飽和して白く見える現象。`bleach` が抜ける量(既定 0 = オフ)、`knee` が始まる輝度、`curve` がその曲げ。 | 0.15 [0, 0.95] /0.01 | vectrex | — |
| `overlay_color_highlight_curve` | Overlay Highlight Release Curve | 明るい部分で色が抜ける。強い光が色樹脂を通ると飽和して白く見える現象。`bleach` が抜ける量(既定 0 = オフ)、`knee` が始まる輝度、`curve` がその曲げ。 | 0.55 [0.2, 3] /0.05 | vectrex | — |
| `overlay_ambient_light` | Overlay Ambient Light | オーバーレイに当たる環境光。[M] Monitor/Glass Sim でゲート。 | 0.5 [0, 1] /0.01 | vectrex | — |

## HDR / SDR presentation（HDR／SDR 提示）

| 内部名 | 表示名 | 説明 | 値 | チェイン | 駆動マクロ |
|---|---|---|---|---|---|
| `beam_peak_nits` | HDR Beam Peak (nits) | ビーム核のピーク輝度(nits)。[M] Beam Brightness の倍率下。 | color: 800 [80, 2000] /10<br>mono: 192 [80, 2000] /2<br>vectrex: 240 [80, 2000] /10 | color/mono/vectrex | 倍率: [M] Beam Brightness |
| `hdr_glow_stability` | HDR Glow Stability | `beam_peak_nits` を上げてもグローの裾が太らないようにする補償。 | 1 [0, 1] /0.05 | color/mono/vectrex | — |
| `hdr_rolloff_knee` | HDR Highlight Knee (xpeak) | ハイライトのロールオフ。ピークの倍数で指定。knee から丸め始め max で飽和。 | 1 [0.5, 2] /0.05 | color/mono/vectrex | — |
| `hdr_rolloff_max` | HDR Highlight Max (xpeak) | ハイライトのロールオフ。ピークの倍数で指定。knee から丸め始め max で飽和。 | color: 2.4 [0.5, 8] /0.05<br>mono: 2.5 [0.5, 8] /0.05<br>vectrex: 2.5 [0.5, 8] /0.05 | color/mono/vectrex | — |
| `hdr_sat_protect` | HDR Saturated Color Protect | 飽和色の保護。単純にクリップすると彩度の高い色が色相ごと崩れる。色相を保ったまま輝度だけ丸める度合い。 | color: 0.5 [0, 1] /0.05<br>mono: 1 [0, 1] /0.05<br>vectrex: 1 [0, 1] /0.05 | color/mono/vectrex | — |
| `sdr_beam_level` | SDR Beam Level | SDR でのビーム露出。[M] Beam Brightness の倍率下(HDR の `beam_peak_nits` と 同じマクロで動くので、どちらで見ていても同じ操作で同じ方向に効く)。 | color: 0.9 [0.1, 1] /0.01<br>mono: 0.72 [0.1, 1] /0.01<br>vectrex: 0.9 [0.1, 1] /0.01 | color/mono/vectrex | 倍率: [M] Beam Brightness |
| `bright_normal_cap` | SDR Normal Brightness Cap | SDR で「通常の明るさ」が到達する上限。HDR ではこの制限は無い (HDR/EDR が有効なとき 1.0 に固定)。 | color: 0.8 [0.1, 1] /0.01<br>mono: 0.6 [0.1, 1] /0.01<br>vectrex: 0.95 [0.1, 1] /0.01 | color/mono/vectrex | 折れ線: [M] Beam Brightness |
| `sdr_rolloff_knee` | SDR Highlight Knee (xwhite) | SDR のハイライトロールオフ。白の倍数で指定。ceiling <= knee でロールオフ無効。 | 0.75 [0.1, 4] /0.05 | color/mono/vectrex | — |
| `sdr_rolloff_ceiling` | SDR Highlight Ceiling (xwhite) | SDR のハイライトロールオフ。白の倍数で指定。ceiling <= knee でロールオフ無効。 | 1 [0.1, 4] /0.05 | color/mono/vectrex | — |
| `sdr_shadow_curve` | SDR Shadow Curve | SDR の暗部カーブ。1 未満で暗部を持ち上げる。既定 0.95。--- | 0.95 [0.3, 3] /0.05 | color/mono/vectrex | — |

