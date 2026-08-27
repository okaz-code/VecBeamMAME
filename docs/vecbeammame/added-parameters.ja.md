# 追加パラメータ一覧

**このファイルは `tools/dump-chain-sliders.py` が `bgfx/chains/vector/*.json` から生成しています。手で編集しないでください。**

スライダーは MAME のスライダーメニュー（既定では `Tab` → Sliders）から操作し、変更した値はゲームごとに `cfg/<game>.cfg` に保存されます。
**[M] が付くものはマクロ**で、複数のパラメータをまとめて動かします。マクロ以外は `Advanced Parameters` を On にすると出てきます（`Brightness Threshold (T)` だけは例外で、常に表示されます）。

起動時オプション（`-vector_*` など、ini ファイルに書くもの）は [startup-options.ja.md](startup-options.ja.md) を参照してください。

凡例: 値の列は `既定値 [下限, 上限] /刻み`。チェインによって違う場合はチェインごとに並べています。

## マクロ（Advanced Off でも見える）

| 内部名 | 表示名 | 値 | チェイン | 駆動マクロ |
|---|---|---|---|---|
| `macro_exposure` | [M] Beam Brightness | 1 [0.25, 3] /0.05 | color/mono/vectrex | — |
| `bright_threshold` | Brightness Threshold (T) | color: 0.9 [0.05, 1] /0.01<br>mono: 0.5 [0.05, 1] /0.01<br>vectrex: 0.5 [0.05, 1] /0.01 | color/mono/vectrex | — |
| `macro_overload` | [M] Overload Amount | 1 [0, 2] /0.05 | color | — |
| `macro_bloom` | [M] Bloom Strength | 1 [0, 3] /0.05 | color/mono/vectrex | — |
| `macro_beam_width` | [M] Beam Width | 1 [0.25, 3] /0.05 | color/mono/vectrex | — |
| `macro_point_size` | [M] Point Size | 1 [0.25, 3] /0.05 | color/mono/vectrex | — |
| `macro_point_bright` | [M] Point Brightness | 1 [0, 2] /0.05 | color/mono/vectrex | — |
| `macro_defocus` | [M] Defocus | 1 [0, 3] /0.05 | color/mono/vectrex | — |
| `macro_persistence` | [M] Phosphor Persistence | 1 [0.1, 4] /0.05 | color/mono/vectrex | — |
| `macro_monitor_sim` | [M] Monitor/Glass Sim | 1 [0, 1] /1 | color/mono/vectrex | — |
| `macro_bezel` | [M] Bezel Reflection | 0 [0, 3] /0.05 | color/mono/vectrex | — |
| `macro_beam_sim` | [M] Beam/Supply Sim | 1 [0, 1] /1 | color | — |
| `macro_halation` | [M] Halation Amount | mono: 0 [0, 5] /0.05<br>vectrex: 1 [0, 5] /0.05 | mono/vectrex | — |
| `advanced_sliders` | Advanced Parameters | 0 [0, 1] /1 | color/mono/vectrex | — |

## Phosphor persistence and decay（蛍光体の残光と減衰）

| 内部名 | 表示名 | 値 | チェイン | 駆動マクロ |
|---|---|---|---|---|
| `phosphor_half_ms` | Phosphor Half-life (ms) | 16 [1, 300] /1 | color/mono/vectrex | 倍率: [M] Phosphor Persistence |
| `phosphor_hold_ms` | Phosphor Hold (ms, full brightness) | color: 12 [0, 80] /1<br>mono: 16 [0, 80] /1<br>vectrex: 16 [0, 80] /1 | color/mono/vectrex | — |
| `phosphor_curve` | Phosphor Decay Curve | color: 5 [0.4, 8] /0.05<br>mono: 3 [0.4, 4] /0.05<br>vectrex: 3 [0.4, 4] /0.05 | color/mono/vectrex | — |
| `phosphor_total_ms` | Phosphor Total (ms) | color: 100 [20, 1500] /10<br>mono: 800 [20, 1500] /10<br>vectrex: 500 [20, 1500] /10 | color/mono/vectrex | 倍率: [M] Phosphor Persistence |
| `phosphor_hit_reset` | Phosphor Hit Reset Floor | 0.02 [0, 0.5] /0.005 | color | — |
| `phosphor_weak_hit_composite` | Phosphor Weak Hit Composite | 1 [0, 1] /1 | color | — |
| `phosphor_rgb_combination` | Phosphor RGB Combination Brightness | 1 [0, 2] /0.05 | color | — |
| `phosphor_rgb_combination_width` | Phosphor RGB Combination Width | 0.15 [0, 1] /0.01 | color | — |
| `phosphor_energy_decay` | Phosphor Energy Decay (bright faster) | color: 4 [0, 4] /0.05<br>mono: 3 [0, 4] /0.05<br>vectrex: 0 [0, 4] /0.05 | color/mono/vectrex | — |
| `phosphor_rgb_decay` | Phosphor RGB Decay (halflife x) | [1.0, 1.05, 0.95] [[0.2, 0.2, 0.2], [3.0, 3.0, 3.0]] /0.05 | color | — |
| `phosphor_color` | Phosphor Color  | color: [1.0, 1.0, 1.0] [[0.0, 0.0, 0.0], [1.0, 1.0, 1.0]] /0.01<br>mono: [0.9, 0.9, 1.0] [[0.0, 0.0, 0.0], [1.0, 1.0, 1.0]] /0.01<br>vectrex: [0.5, 0.7, 1.0] [[0.0, 0.0, 0.0], [1.0, 1.0, 1.0]] /0.01 | color/mono/vectrex | — |

## Brightness transfer（輝度トランスファ）

| 内部名 | 表示名 | 値 | チェイン | 駆動マクロ |
|---|---|---|---|---|
| `bright_sigmoid` | Brightness Sigmoid | color: -2 [-4, 4] /0.1<br>mono: 0 [-4, 4] /0.1<br>vectrex: -3 [-4, 4] /0.1 | color/mono/vectrex | — |
| `bright_sigmoid_center` | Brightness Sigmoid Center | color: 0.4 [0.05, 0.95] /0.01<br>mono: 0.4 [0.05, 0.95] /0.01<br>vectrex: 0.8 [0.05, 0.95] /0.01 | color/mono/vectrex | — |

## Beam width（ビーム幅）

| 内部名 | 表示名 | 値 | チェイン | 駆動マクロ |
|---|---|---|---|---|
| `beam_width_min` | Beam Width Minimum | color: 2.8 [0.1, 24] /0.05<br>mono: 2 [0.1, 24] /0.05<br>vectrex: 0.1 [0.1, 24] /0.05 | color/mono/vectrex | 倍率: [M] Beam Width |
| `beam_width_max` | Beam Width Maximum | color: 3 [0.1, 24] /0.1<br>mono: 2 [0.1, 24] /0.1<br>vectrex: 0.5 [0.1, 24] /0.1 | color/mono/vectrex | 倍率: [M] Beam Width |
| `width_curve` | Width Curve | color: 2 [0.2, 4] /0.05<br>mono: 1 [0.2, 4] /0.05<br>vectrex: 1 [0.2, 4] /0.05 | color/mono/vectrex | — |
| `core_flat` | Line Core Flatness | color: 0.4 [0, 0.98] /0.05<br>mono: 0.5 [0, 0.98] /0.05<br>vectrex: 0.5 [0, 0.98] /0.05 | color/mono/vectrex | — |
| `width_knee` | Width at Threshold | 0.8 [0, 1] /0.01 | mono/vectrex | — |
| `width_sigmoid` | Width Sigmoid | 0 [-4, 4] /0.1 | mono/vectrex | — |
| `width_sigmoid_center` | Width Sigmoid Center | 0.5 [0.05, 0.95] /0.01 | mono/vectrex | — |
| `beam_width_overmax` | Beam Width Over-Max (lift) | 0.5 [0, 32] /0.5 | mono/vectrex | — |
| `width_over_curve` | Width Overload Curve | 1 [0.2, 4] /0.05 | mono/vectrex | — |
| `halo_quad_extent` | Halo Quad Extent (sigma, perf) | 2.5 [2, 3.5] /0.1 | color/mono/vectrex | — |

## Points and dots（点・ドット）

| 内部名 | 表示名 | 値 | チェイン | 駆動マクロ |
|---|---|---|---|---|
| `line_point_threshold` | Line/Point Threshold (px) | 2 [0, 8] /0.5 | color/mono/vectrex | — |
| `point_width_scale` | Point Width Scale | color: 1.8 [0.05, 4] /0.05<br>mono: 0.6 [0.05, 4] /0.05<br>vectrex: 1 [0.05, 4] /0.05 | color/mono/vectrex | 倍率: [M] Point Size |
| `point_brightness_scale` | Point Brightness Scale | color: 2 [0, 4] /0.05<br>mono: 1.5 [0, 4] /0.05<br>vectrex: 1.5 [0, 4] /0.05 | color/mono/vectrex | 倍率: [M] Point Brightness |
| `point_roundness` | Point Roundness | color: 0.25 [0, 1] /0.05<br>mono: 0.4 [0, 1] /0.05<br>vectrex: 0.4 [0, 1] /0.05 | color/mono/vectrex | — |
| `vertex_dwell` | Vertex Dwell (corner dots) | color: 0.5 [0, 1] /0.05<br>mono: 0 [0, 1] /0.05<br>vectrex: 0 [0, 1] /0.05 | color/mono/vectrex | — |
| `cap_no_persist` | Short-Dwell Dots: No Persistence | color: 0 [0, 1] /1<br>mono: 1 [0, 1] /1<br>vectrex: 1 [0, 1] /1 | color/mono/vectrex | — |
| `dot_no_persist_dwell` | Dot No-Persist Dwell (us) | color: 10 [0, 60] /1<br>mono: 20 [0, 60] /1<br>vectrex: 0 [0, 60] /1 | color/mono/vectrex | — |
| `z_rise_tau` | Z Rise Time (us) | color: 0.2 [0, 20] /0.01<br>mono: 0 [0, 20] /0.01<br>vectrex: 10 [0, 20] /0.01 | color/mono/vectrex | — |
| `isolated_dot_min_size` | Isolated Dwell Dot Minimum (px) | 3 [0, 16] /0.1 | vectrex | 倍率: [M] Point Size |

## Focus and defocus（フォーカス／ボケ）

| 内部名 | 表示名 | 値 | チェイン | 駆動マクロ |
|---|---|---|---|---|
| `defocus` | Defocus (beam blur),  | color: [0.3, 0.3] [[0.0, 0.0], [2.0, 2.0]] /0.1<br>mono: [0.2, 0.2] [[0.0, 0.0], [2.0, 2.0]] /0.1<br>vectrex: [0.4, 0.4] [[0.0, 0.0], [2.0, 2.0]] /0.1 | color/mono/vectrex | 倍率: [M] Defocus |
| `edge_defocus` | Edge Defocus | color: 0.1 [0, 8] /0.1<br>mono: 0.2 [0, 8] /0.1<br>vectrex: 0.1 [0, 8] /0.1 | color/mono/vectrex | 倍率: [M] Defocus<br>0/1: [M] Monitor/Glass Sim |
| `edge_defocus_curve` | Edge Defocus Curve | 3 [0.5, 4] /0.1 | color/mono/vectrex | — |

## Line ends（線端＝ブランキング遷移）

| 内部名 | 表示名 | 値 | チェイン | 駆動マクロ |
|---|---|---|---|---|
| `line_cap_width` | Line End Width (x body) | color: 1.1 [0.1, 4] /0.05<br>mono: 1.25 [0.1, 4] /0.05<br>vectrex: 1.25 [0.1, 4] /0.05 | color/mono/vectrex | — |
| `line_cap_overload_add` | Line End Overload Add (px) | color: 2 [0, 12] /0.1<br>mono: 1 [0, 12] /0.1<br>vectrex: 1 [0, 12] /0.1 | color/mono/vectrex | — |
| `line_cap_overload_curve` | Line End Overload Curve | color: 0.25 [0.25, 8] /0.25<br>mono: 4 [0.25, 8] /0.25<br>vectrex: 4 [0.25, 8] /0.25 | color/mono/vectrex | — |
| `line_cap_transition` | Line End Transition (px) | color: 10 [0.5, 64] /0.5<br>mono: 8 [0.5, 64] /0.5<br>vectrex: 8 [0.5, 64] /0.5 | color/mono/vectrex | — |
| `line_cap_curve` | Line End Transition Curve | color: 0.5 [0.25, 4] /0.05<br>mono: 1.5 [0.25, 4] /0.05<br>vectrex: 1.5 [0.25, 4] /0.05 | color/mono/vectrex | — |
| `line_cap_mode` | Line End Mode | color: 0 [0, 3] /1<br>mono: 1 [0, 3] /1<br>vectrex: 2 [0, 3] /1 | color/mono/vectrex | — |
| `cap_ramp_only` | Line End: RAMP termini only | 1 [0, 1] /1 | vectrex | — |

## Beam energy model（ビームエネルギーモデル）

| 内部名 | 表示名 | 値 | チェイン | 駆動マクロ |
|---|---|---|---|---|
| `energy_model` | Energy Model (speed/dwell) | 1 [0, 1] /1 | color/mono/vectrex | — |
| `energy_infl` | Energy Influence (no-current src) | color: 1 [0, 1] /0.05<br>mono: 0.6 [0, 1] /0.05<br>vectrex: 0.6 [0, 1] /0.05 | color/mono/vectrex | — |
| `energy_speed_norm` | Energy Speed Norm (scr/ms) | color: 6 [0.05, 12] /0.05<br>mono: 0.6 [0.05, 12] /0.05<br>vectrex: 0.6 [0.05, 12] /0.05 | color/mono/vectrex | — |
| `energy_curve` | Energy Curve | color: 1.05 [0.2, 4] /0.05<br>mono: 1 [0.2, 4] /0.05<br>vectrex: 1 [0.2, 4] /0.05 | color/mono/vectrex | — |
| `energy_line_max` | Energy Line Max / Density Cap | color: 1.2 [1, 8] /0.1<br>mono: 4 [1, 8] /0.1<br>vectrex: 4 [1, 8] /0.1 | color/mono/vectrex | — |
| `energy_dot_ref` | Energy Dot Ref (us) | 30 [2, 300] /1 | color/mono/vectrex | — |
| `energy_dot_curve` | Energy Dot Curve | 1.6 [0.2, 4] /0.05 | color/mono/vectrex | — |
| `energy_dot_max` | Energy Dot Max / Density Cap | 3.2 [1, 16] /0.1 | color/mono/vectrex | — |
| `energy_stroke_agg` | Energy Stroke Aggregation | 1 [0, 1] /1 | color/mono/vectrex | — |
| `energy_dwell_cap` | Energy Dwell Cap | 1.6 [0, 16] /0.1 | vectrex | — |
| `energy_obj_lift` | Energy Object Lift | 1 [0, 1] /1 | vectrex | — |
| `energy_obj_knee` | Energy Object Knee | 0.75 [0, 1] /0.01 | vectrex | — |
| `energy_obj_sharp` | Energy Object Sharpness | 2 [0.1, 8] /0.1 | vectrex | — |
| `energy_obj_max` | Energy Object Max | 3 [1, 8] /0.1 | vectrex | — |
| `energy_obj_star` | Energy Object Star Boost | 1.5 [1, 4] /0.05 | vectrex | — |

## Overload and overdrive（過大入力とオーバードライブ）

| 内部名 | 表示名 | 値 | チェイン | 駆動マクロ |
|---|---|---|---|---|
| `overload_threshold` | Overload Threshold | color: 1 [0.1, 4] /0.05<br>mono: 0.55 [0.1, 4] /0.05<br>vectrex: 0.3 [0.1, 4] /0.05 | color/mono/vectrex | 折れ線: [M] Overload Amount |
| `overload_ramp` | Overload Ramp (n span) | color: 0.75 [0, 4] /0.05<br>mono: 1 [0, 4] /0.05<br>vectrex: 1 [0, 4] /0.05 | color/mono/vectrex | — |
| `intensity_overdrive` | Overdrive (hot core) | 1 [0, 4] /0.05 | color/mono/vectrex | 折れ線: [M] Overload Amount |
| `intensity_overdrive_curve` | Overdrive Curve | color: 1 [0.25, 8] /0.05<br>mono: 2 [0.25, 8] /0.05<br>vectrex: 2 [0.25, 8] /0.05 | color/mono/vectrex | — |
| `mask_overdrive_flare` | Mask Overdrive Hot Core | 1 [0, 1] /1 | color | — |
| `overload_dot_gain` | Overdrive Dot Gain | 2 [1, 8] /0.1 | color/mono/vectrex | — |
| `overload_max` | Overdrive Max (x peak) | 3 [0, 8] /0.1 | color/mono/vectrex | — |
| `overdrive_core` | Overdrive to Core | color: 0.5 [0, 1] /0.05<br>mono: 1 [0, 1] /0.05<br>vectrex: 0 [0, 1] /0.05 | color/mono/vectrex | — |
| `overload_bloom` | Overload Defocus (blur) | color: 0.2 [0, 4] /0.05<br>mono: 0.7 [0, 4] /0.05<br>vectrex: 0.3 [0, 4] /0.05 | color/mono/vectrex | 倍率: [M] Defocus |
| `overload_width_add` | Overload Core Width Add (px) | color: 7 [0, 12] /0.1<br>vectrex: 1 [0, 12] /0.1 | color/vectrex | 倍率: [M] Beam Width |
| `overload_width_steepness` | Overload Width Steepness | 6 [1, 20] /0.5 | color/vectrex | — |
| `overload_width_center` | Overload Width Center | color: 0.2 [0.05, 0.99] /0.01<br>vectrex: 0.65 [0.05, 0.99] /0.01 | color/vectrex | — |
| `overlap_white_strength` | Overlap White Strength | 1 [0, 1] /0.05 | color | — |
| `overlap_white_count` | Overlap White Count | 39 [1, 50] /0.25 | color | — |
| `overlap_white_brightness` | Overlap White Brightness | 1.5 [0, 2] /0.05 | color | — |
| `overlap_white_spread` | Overlap White Spread | 1.25 [0, 12] /0.25 | color | — |
| `overload_display_compression` | Overload Display Compression | 1 [0, 1] /0.05 | color | — |
| `phosphor_overdrive` | Overdrive (white at peak) | 0.5 [0, 1] /0.05 | mono | 倍率: [M] Halation Amount |
| `overdrive_knee` | Overdrive Knee/Ceiling (xpeak)  | [0.6, 0.6] [[0.0, 0.0], [4.0, 4.0]] /0.05 | mono | — |
| `overdrive_sat_curve` | Overdrive Saturation Curve | 1 [0.2, 4] /0.05 | mono | — |
| `overdrive_color` | Overdrive Color  | [1.0, 1.0, 1.0] [[0.0, 0.0, 0.0], [1.0, 1.0, 1.0]] /0.01 | mono | — |
| `overload_width_bloom_link` | Overload Width/Bloom Link | 0 [0, 1] /1 | vectrex | — |

## Glow and bloom（グロー／ブルーム）

| 内部名 | 表示名 | 値 | チェイン | 駆動マクロ |
|---|---|---|---|---|
| `analytic_glow` | Glow Source Strength (all vectors) | color: 0.095 [0, 1] /0.005<br>mono: 0.038 [0, 1] /0.001<br>vectrex: 0.065 [0, 1] /0.005 | color/mono/vectrex | 倍率: [M] Bloom Strength |
| `analytic_glow_width` | Glow Source Radius (all vectors) | color: 20 [1, 80] /1<br>mono: 15 [1, 160] /1<br>vectrex: 15 [1, 160] /1 | color/mono/vectrex | — |
| `glow_narrow` | Glow Narrow Strength | color: 0.004 [0, 0.2] /0.001<br>mono: 0.002 [0, 0.2] /0.001<br>vectrex: 0.004 [0, 0.2] /0.001 | color/mono/vectrex | 倍率: [M] Bloom Strength |
| `overload_core_gain` | Overload Hot Core Gain | 0.015 [0, 0.5] /0.005 | color | — |
| `glow_wide` | Glow Wide (low-res) | color: 0.0024 [0, 0.02] /0.0001<br>mono: 0.00014 [0, 0.02] /1e-05<br>vectrex: 0.0002 [0, 0.02] /0.0001 | color/mono/vectrex | 倍率: [M] Bloom Strength |
| `glow_wide_reach` | Glow Wide Reach | 8 [0, 16] /0.25 | color | — |
| `glow_wide_pivot` | Glow Wide Pivot (source level) | 1 [0.05, 4] /0.05 | color | — |
| `glow_wide_curve` | Glow Wide Curve (above pivot up) | 1.6 [0.5, 3] /0.05 | color | — |
| `glow_wide_smooth` | Glow Wide Downsample | 1 [0, 2] /1 | color | — |
| `overload_glow_gain` | Overload Glow (bloom) | 1.06 [0, 2] /0.02 | color | 折れ線: [M] Overload Amount |
| `overload_glow_width` | Overload Glow Width (px) | 16 [4, 200] /2 | color | — |
| `glow_tail_curve` | Glow Tail Curve | color: 1.44 [0.4, 2.5] /0.02<br>mono: 0.6 [0.4, 2.5] /0.02<br>vectrex: 1.82 [0.4, 2.5] /0.02 | color/mono/vectrex | — |
| `glow_fbo_scale` | Glow FBO Scale (perf) | color: 0.4 [0.1, 1] /0.05<br>mono: 0.5 [0.1, 1] /0.05<br>vectrex: 0.4 [0.1, 1] /0.05 | color/mono/vectrex | — |
| `glow_black_toe` | Glow Black Toe | 0.002 [0, 0.05] /0.001 | mono/vectrex | — |

## Halation and starburst（ハレーション／スターバースト）

| 内部名 | 表示名 | 値 | チェイン | 駆動マクロ |
|---|---|---|---|---|
| `halation_gain` | Halation Gain | mono: 1 [0, 5] /0.001<br>vectrex: 0.66 [0, 2] /0.001 | mono/vectrex | 倍率: [M] Halation Amount |
| `ring_over_gain` | Halation from Overdrive | 1 [0, 20] /0.05 | mono/vectrex | — |
| `ring_gain` | Halation Rim | mono: 0.005 [0, 0.2] /0.001<br>vectrex: 0.033 [0, 0.2] /0.001 | mono/vectrex | 倍率: [M] Halation Amount |
| `ring_fill` | Halation Fill | mono: 0.02 [0, 0.2] /0.001<br>vectrex: 0.077 [0, 0.2] /0.001 | mono/vectrex | 倍率: [M] Halation Amount |
| `ring_radius` | Halation Radius (px) | mono: 44 [4, 80] /1<br>vectrex: 30 [4, 80] /1 | mono/vectrex | — |
| `ring_width` | Halation Width (px) | mono: 3 [0.75, 12] /0.25<br>vectrex: 1.25 [0.75, 12] /0.25 | mono/vectrex | — |
| `ring_min_dwell` | Halation Min Dwell (us) | 20 [0, 200] /1 | mono/vectrex | — |
| `ring_threshold` | Halation Threshold (bright) | 0 [0, 2] /0.02 | mono/vectrex | — |
| `ray_gain` | Starburst Gain | mono: 0.005 [0, 0.5] /0.0001<br>vectrex: 0.0044 [0, 0.5] /0.0001 | mono/vectrex | 倍率: [M] Halation Amount |
| `ray_var` | Starburst Uneven | 0.6 [0, 1] /0.05 | mono/vectrex | — |
| `ray_count` | Starburst Rays | 6 [2, 12] /1 | mono/vectrex | — |
| `ray_length` | Starburst Length (px) | mono: 68 [8, 300] /2<br>vectrex: 50 [8, 300] /2 | mono/vectrex | — |
| `ray_length_rand` | Starburst Length Random | 0.1 [0, 1] /0.05 | mono/vectrex | — |
| `ray_count_rand` | Starburst Count Random | mono: 0.4 [0, 1] /0.05<br>vectrex: 0.15 [0, 1] /0.05 | mono/vectrex | — |
| `ray_width` | Starburst Width (px) | mono: 0.8 [0.5, 6] /0.1<br>vectrex: 0.5 [0.5, 6] /0.1 | mono/vectrex | — |
| `ray_angle` | Starburst Angle (deg) | 15 [0, 180] /1 | mono/vectrex | — |

## Edge glow (beam past the visible area)（画面外へ振れたビームの発光）

| 内部名 | 表示名 | 値 | チェイン | 駆動マクロ |
|---|---|---|---|---|
| `edge_glow` | Edge Glow Gain | 0.75 [0, 2] /0.01 | vectrex | — |
| `edge_glow_threshold` | Edge Glow Threshold | 0 [0, 10] /0.01 | vectrex | — |
| `edge_glow_sensitivity` | Edge Glow Sensitivity | 1 [0.01, 10] /0.01 | vectrex | — |
| `edge_glow_width` | Edge Glow Width (px) | 100 [4, 400] /2 | vectrex | — |
| `edge_glow_length` | Edge Glow Length (px) | 150 [20, 600] /5 | vectrex | — |
| `edge_glow_persist` | Edge Glow Persistence (ms) | 120 [0, 1000] /10 | vectrex | — |

## Convergence（コンバージェンス）

| 内部名 | 表示名 | 値 | チェイン | 駆動マクロ |
|---|---|---|---|---|
| `convergence_global_gain` | Convergence Global Bloom | color: 0.01 [0, 0.5] /0.005<br>mono: 0 [0, 0.5] /0.005<br>vectrex: 0 [0, 0.5] /0.005 | color/mono/vectrex | 0/1: [M] Monitor/Glass Sim |
| `convergence_global_coverage` | Convergence Global Coverage | color: 0.3 [0.1, 1.2] /0.025<br>mono: 0.55 [0.1, 1.2] /0.025<br>vectrex: 0.55 [0.1, 1.2] /0.025 | color/mono/vectrex | — |
| `converge_red` | Red Linear Convergence,  | [0.0, 0.0] [[-10.0, -10.0], [10.0, 10.0]] /0.1 | color | — |
| `converge_green` | Green Linear Convergence,  | [0.0, 0.0] [[-10.0, -10.0], [10.0, 10.0]] /0.1 | color | — |
| `converge_blue` | Blue Linear Convergence,  | [0.0, 0.0] [[-10.0, -10.0], [10.0, 10.0]] /0.1 | color | — |
| `radial_converge_red` | Red Radial Convergence,  | [0.0, 0.0] [[-0.1, -0.1], [0.1, 0.1]] /0.001 | color | — |
| `radial_converge_green` | Green Radial Convergence,  | [0.0, 0.0] [[-0.1, -0.1], [0.1, 0.1]] /0.001 | color | — |
| `radial_converge_blue` | Blue Radial Convergence,  | [0.0, 0.0] [[-0.1, -0.1], [0.1, 0.1]] /0.001 | color | — |

## Beam time window（ビーム時間窓＝1プレゼントあたりの掃引スライス）

| 内部名 | 表示名 | 値 | チェイン | 駆動マクロ |
|---|---|---|---|---|
| `beam_window` | Beam Time Window | 1 [0, 1] /1 | color/mono/vectrex | 0/1: [M] Beam/Supply Sim |
| `beam_window_scale` | Beam Time Window Rate (x real time) | color: 2 [0.25, 4] /0.05<br>mono: 1.5 [0.25, 4] /0.05<br>vectrex: 1.25 [0.25, 4] /0.05 | color/mono/vectrex | — |
| `beam_flash_ms` | Beam Strike Flash (ms) | 1 [0, 40] /0.5 | color/mono/vectrex | — |
| `beam_flash_gain` | Beam Strike Flash Gain (x) | 1.5 [1, 8] /0.1 | color/mono/vectrex | — |

## Flicker, HV droop and beam jitter（フリッカ・HV垂下・ビームジッタ）

| 内部名 | 表示名 | 値 | チェイン | 駆動マクロ |
|---|---|---|---|---|
| `flicker_thresh_ms` | Cyclic Flicker Threshold (draw ms) | color: 24 [0, 300] /1<br>mono: 22 [0, 300] /1<br>vectrex: 12 [0, 300] /1 | color/mono/vectrex | — |
| `flicker_buckets` | Cyclic Flicker Buckets (N) | color: 9 [2, 16] /1<br>mono: 6 [2, 16] /1<br>vectrex: 6 [2, 16] /1 | color/mono/vectrex | — |
| `flicker_period_ms` | Cyclic Flicker Step (ms) | color: 20 [2, 100] /0.5<br>mono: 16.7 [2, 100] /0.5<br>vectrex: 16.7 [2, 100] /0.5 | color/mono/vectrex | — |
| `flicker_red_depth` | Cyclic Flicker Red Depth | 1 [0, 1] /0.01 | color | 0/1: [M] Beam/Supply Sim |
| `flicker_green_depth` | Cyclic Flicker Green Depth | 0.5 [0, 1] /0.01 | color | 0/1: [M] Beam/Supply Sim |
| `flicker_blue_depth` | Cyclic Flicker Blue Fine Depth | 0.1 [0, 0.1] /0.001 | color | 0/1: [M] Beam/Supply Sim |
| `hv_droop` | HV Droop (dim+defocus) | 0.5 [0, 1] /0.01 | color | 0/1: [M] Beam/Supply Sim |
| `hv_droop_dim` | HV Droop Dim (0=defocus only) | 0 [0, 1] /0.05 | color | — |
| `hv_droop_onset` | HV Droop Overload Onset | 0.5 [0, 60] /0.5 | color | — |
| `hv_droop_ref` | HV Droop Load Ref | 5 [1, 60] /0.5 | color | — |
| `beam_jitter` | Beam Jitter (energy + position) | 0.1 [0, 1] /0.01 | color | 0/1: [M] Beam/Supply Sim |
| `beam_jitter_hz` | Beam Jitter Speed (Hz) | 15 [1, 120] /1 | color | — |
| `beam_jitter_saturation_start` | Beam Jitter Saturation Start | 1.5 [0.5, 6] /0.05 | color | — |
| `beam_jitter_saturation_range` | Beam Jitter Saturation Range | 1.5 [0.1, 6] /0.05 | color | — |
| `beam_jitter_saturation_curve` | Beam Jitter Saturation Curve | 2 [0.25, 8] /0.05 | color | — |

## Color adjustment（色調整）

| 内部名 | 表示名 | 値 | チェイン | 駆動マクロ |
|---|---|---|---|---|
| `primary_color_mode` | Color Adjustment Mode | 1 [0, 1] /1 | color | — |
| `primary_red_hue` | Red Hue Shift (deg) | 1 [-60, 60] /1 | color | — |
| `primary_red_saturation` | Red Saturation | 0.85 [0, 2] /0.01 | color | — |
| `primary_red_brightness` | Red Brightness | 1 [0, 2] /0.01 | color | — |
| `primary_green_hue` | Green Hue Shift (deg) | 0 [-60, 60] /1 | color | — |
| `primary_green_saturation` | Green Saturation | 0.8 [0, 2] /0.01 | color | — |
| `primary_green_brightness` | Green Brightness | 1 [0, 2] /0.01 | color | — |
| `primary_blue_hue` | Blue Hue Shift (Violet +) | 1 [-60, 60] /1 | color | — |
| `primary_blue_saturation` | Blue Saturation | 0.9 [0, 2] /0.01 | color | — |
| `primary_blue_brightness` | Blue Brightness | 1.2 [0, 2] /0.01 | color | — |
| `chroma_a` | Phosphor A Chromaticity  | [0.63, 0.34] [[0.0, 0.0], [1.0, 1.0]] /0.001 | color | — |
| `chroma_b` | Phosphor B Chromaticity  | [0.31, 0.595] [[0.0, 0.0], [1.0, 1.0]] /0.001 | color | — |
| `chroma_c` | Phosphor C Chromaticity  | [0.17, 0.07] [[0.0, 0.0], [1.0, 1.0]] /0.001 | color | — |
| `chroma_y_gain` | Phosphor Gain,  | [0.2124, 0.62, 0.1] [[0.0, 0.0, 0.0], [1.0, 1.0, 1.0]] /0.001 | color | — |

## Vector geometry（ベクター幾何）

| 内部名 | 表示名 | 値 | チェイン | 駆動マクロ |
|---|---|---|---|---|
| `vector_linearity_x` | Vector Linearity X (gain) | 1 [0.8, 1.2] /0.005 | color/mono/vectrex | — |
| `vector_linearity_y` | Vector Linearity Y (gain) | 1 [0.8, 1.2] /0.005 | color/mono/vectrex | — |
| `vector_pincushion_x_quad` | Vector Pincushion X (Quad) | 0 [-1, 1] /0.01 | color/mono/vectrex | — |
| `vector_image_scale` | Vector Image Scale | color: 0.94 [0.75, 1.15] /0.005<br>mono: 0.95 [0.75, 1.15] /0.005<br>vectrex: 0.96 [0.75, 1.15] /0.005 | color/mono/vectrex | 折れ線: [M] Monitor/Glass Sim |

## Shadow mask（シャドウマスク）

| 内部名 | 表示名 | 値 | チェイン | 駆動マクロ |
|---|---|---|---|---|
| `shadow_mask_strength` | Shadow Mask Strength | 0.3 [0, 1] /0.01 | color | 0/1: [M] Monitor/Glass Sim |
| `shadow_mask_scale` | Shadow Mask Size (px @1080p) | 0.65 [0.25, 8] /0.05 | color | — |
| `shadow_mask_brightboost` | Shadow Mask Brightness Boost | 0 [0, 2] /0.05 | color | — |
| `masked_core_peak` | Masked Core Peak Limit | 1 [0, 4] /0.05 | color | — |
| `core_overlap_max` | Direct Core Overlap | 1 [0, 1] /1 | color | — |
| `ambient_mask` | Ambient Shadow-Mask (0=flat,1=masked) | 1 [0, 1] /0.05 | color | — |

## Tube, ambient and bezel（管面・環境光・ベゼル）

| 内部名 | 表示名 | 値 | チェイン | 駆動マクロ |
|---|---|---|---|---|
| `tube_distortion` | Tube Quadric Distortion | color: 0.12 [-2, 2] /0.01<br>mono: 0.15 [-2, 2] /0.01<br>vectrex: 0.15 [-2, 2] /0.01 | color/mono/vectrex | 0/1: [M] Monitor/Glass Sim |
| `tube_round_corner` | Tube Rounded Corner | color: 0.15 [0, 2] /0.01<br>mono: 0.2 [0, 2] /0.01<br>vectrex: 0.3 [0, 2] /0.01 | color/mono/vectrex | 0/1: [M] Monitor/Glass Sim |
| `tube_vignetting` | Tube Vignetting | 0.8 [0, 2] /0.01 | color/mono/vectrex | 0/1: [M] Monitor/Glass Sim |
| `tube_face_scale` | Tube Face Scale | color: 0.98 [0.8, 1] /0.005<br>mono: 0.99 [0.8, 1] /0.005<br>vectrex: 0.98 [0.8, 1] /0.005 | color/mono/vectrex | 折れ線: [M] Monitor/Glass Sim |
| `ambient_color` | Ambient Color  | color: [0.35, 0.35, 0.35] [[0.0, 0.0, 0.0], [1.0, 1.0, 1.0]] /0.01<br>mono: [0.35, 0.45, 0.38] [[0.0, 0.0, 0.0], [1.0, 1.0, 1.0]] /0.01<br>vectrex: [0.35, 0.45, 0.38] [[0.0, 0.0, 0.0], [1.0, 1.0, 1.0]] /0.01 | color/mono/vectrex | — |
| `ambient_level` | Ambient Level (x0.001) | color: 1 [0, 200] /0.5<br>mono: 1 [0, 200] /0.5<br>vectrex: 2 [0, 200] /0.5 | color/mono/vectrex | 0/1: [M] Monitor/Glass Sim |
| `mglow_coefficient` | Monitor Glow Coefficient | 0.4 [0, 10] /0.05 | color | — |
| `mglow_brightness` | Monitor Glow Brightness | 0.15 [0, 2] /0.01 | color | — |
| `mglow_center_edge_diff` | Monitor Glow Center-Edge Diff | 0.1 [0, 1] /0.01 | color | — |
| `mglow_rgb_bands` | Monitor Glow RGB Bands | 0.35 [0, 1] /0.01 | color | 0/1: [M] Monitor/Glass Sim |
| `mglow_rgb_band_count` | Monitor Glow RGB Band Count | 8 [3, 24] /1 | color | — |
| `mglow_min_distance` | Monitor Glow Min Distance (screen) | 0.3 [0, 0.75] /0.01 | color | — |
| `mglow_coverage_start` | Monitor Glow Coverage Start | 0.65 [0, 0.95] /0.01 | color | — |
| `mglow_coverage_full` | Monitor Glow Coverage Full | 0.85 [0.05, 1] /0.01 | color | — |
| `bezel_glow_strength` | Bezel Reflection Glow | color: 2 [0, 8] /0.01<br>mono: 0.25 [0, 2] /0.01<br>vectrex: 0.25 [0, 2] /0.01 | color/mono/vectrex | 倍率: [M] Bezel Reflection<br>0/1: [M] Monitor/Glass Sim |
| `monitor_bezel_reflection` | Monitor Glow Bezel Reflection | 0.5 [0, 1] /0.01 | color | 折れ線: [M] Bezel Reflection<br>0/1: [M] Monitor/Glass Sim |
| `bezel_long_reflection` | Bezel Long-Line Reflection | 1 [0, 2] /0.05 | color | — |
| `bezel_short_reflection` | Bezel Short-Line Reflection | 0.1 [0, 1] /0.02 | color | — |
| `bezel_long_threshold` | Bezel Long-Line Threshold (px) | 160 [40, 400] /10 | color | — |
| `bezel_glow_width` | Bezel Glow Width (px) | color: 100 [2, 200] /1<br>mono: 32 [2, 200] /1<br>vectrex: 32 [2, 200] /1 | color/mono/vectrex | — |
| `bezel_glow_curve` | Bezel Glow Curve | 2 [0.25, 4] /0.05 | color/mono/vectrex | — |
| `room_ambient` | Room Ambient (Overlay/Bezel) | 0.25 [0, 2] /0.05 | vectrex | — |

## Printed overlay（印刷オーバーレイ＝Vectrex）

| 内部名 | 表示名 | 値 | チェイン | 駆動マクロ |
|---|---|---|---|---|
| `overlay_white_transmission` | Overlay White Transmission | 0.65 [0, 1] /0.01 | vectrex | — |
| `overlay_white_diffusion` | Overlay Resin Diffusion Strength | 0.5 [0, 1] /0.01 | vectrex | — |
| `overlay_diffusion_radius` | Overlay Resin Diffusion Radius (px) | 12 [0, 16] /0.25 | vectrex | — |
| `overlay_diffusion_shape` | Overlay Resin Diffusion Curve | 0.4 [0.2, 6] /0.05 | vectrex | — |
| `overlay_white_reflectance` | Overlay White Reflectance | 0.25 [0, 1] /0.01 | vectrex | — |
| `overlay_color_density` | Overlay Color Optical Density | 1.75 [0, 6] /0.05 | vectrex | — |
| `overlay_color_glow` | Overlay Rear Resin Scatter | 1.1 [0, 2] /0.05 | vectrex | — |
| `overlay_color_dark_level` | Overlay Resin Dark Level | 0.08 [0, 1.6] /0.005 | vectrex | — |
| `overlay_color_highlight_bleach` | Overlay Highlight Color Release | 0 [0, 1] /0.01 | vectrex | — |
| `overlay_color_highlight_knee` | Overlay Highlight Release Knee | 0.15 [0, 0.95] /0.01 | vectrex | — |
| `overlay_color_highlight_curve` | Overlay Highlight Release Curve | 0.55 [0.2, 3] /0.05 | vectrex | — |
| `overlay_ambient_light` | Overlay Ambient Light | 0.5 [0, 1] /0.01 | vectrex | — |

## HDR / SDR presentation（HDR／SDR 提示）

| 内部名 | 表示名 | 値 | チェイン | 駆動マクロ |
|---|---|---|---|---|
| `beam_peak_nits` | HDR Beam Peak (nits) | color: 800 [80, 2000] /10<br>mono: 192 [80, 2000] /2<br>vectrex: 240 [80, 2000] /10 | color/mono/vectrex | 倍率: [M] Beam Brightness |
| `hdr_glow_stability` | HDR Glow Stability | 1 [0, 1] /0.05 | color/mono/vectrex | — |
| `hdr_rolloff_knee` | HDR Highlight Knee (xpeak) | 1 [0.5, 2] /0.05 | color/mono/vectrex | — |
| `hdr_rolloff_max` | HDR Highlight Max (xpeak) | color: 2.4 [0.5, 8] /0.05<br>mono: 2.5 [0.5, 8] /0.05<br>vectrex: 2.5 [0.5, 8] /0.05 | color/mono/vectrex | — |
| `hdr_sat_protect` | HDR Saturated Color Protect | color: 0.5 [0, 1] /0.05<br>mono: 1 [0, 1] /0.05<br>vectrex: 1 [0, 1] /0.05 | color/mono/vectrex | — |
| `sdr_beam_level` | SDR Beam Level | color: 0.9 [0.1, 1] /0.01<br>mono: 0.72 [0.1, 1] /0.01<br>vectrex: 0.9 [0.1, 1] /0.01 | color/mono/vectrex | 倍率: [M] Beam Brightness |
| `bright_normal_cap` | SDR Normal Brightness Cap | color: 0.8 [0.1, 1] /0.01<br>mono: 0.6 [0.1, 1] /0.01<br>vectrex: 0.95 [0.1, 1] /0.01 | color/mono/vectrex | 折れ線: [M] Beam Brightness |
| `sdr_rolloff_knee` | SDR Highlight Knee (xwhite) | 0.75 [0.1, 4] /0.05 | color/mono/vectrex | — |
| `sdr_rolloff_ceiling` | SDR Highlight Ceiling (xwhite) | 1 [0.1, 4] /0.05 | color/mono/vectrex | — |
| `sdr_shadow_curve` | SDR Shadow Curve | 0.95 [0.3, 3] /0.05 | color/mono/vectrex | — |

