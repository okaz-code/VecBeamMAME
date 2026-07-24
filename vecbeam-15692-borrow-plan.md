# VecBeam 改造プラン — PR #15692 からの借用

作成: 2026-07-24 / ブランチ: `vector-beam-sim`

## 背景

MAME 本家に vector CRT レンダラの PR が2本ある:

- **#15428**(自分 / okaz-code)"bgfx: render vector games through a post-processing chain" — Open、6/15 以降停滞。
- **#15692**(hansandersson92)"bgfx: add vector CRT renderer" — 7/17 MooglyGuy 承認済み・デフォルト ON 化済みで**マージ寸前**。RGBA16F 蓄積 + ガウシアンビーム + 単純指数減衰 + bloom + SDR tonemap の薄い汎用実装。

VecBeam 本体を派生として本家 PR する予定はない(物量過大)。ただし #15692 の実装で **VecBeam が力技で解決した箇所を素直に置き換えられる**アイデアがあるため、それを VecBeam 側に取り込む。本ドキュメントは実施予定分のみを記述。

参考:#15692 は内部 FP16 だが**出力は SDR tonemap 止まり**(実 HDR 出力なし)。VecBeam の HDR 出力経路(`fs_vector_hdr_present.sc` / `beam_peak_nits` / PQ)は VecBeam 固有の資産として温存する。

---

## 実施項目

### FU-1(本命):正規化基準を向きに不変な軸へ + `/1920` の集約

#### 目的
横画面ゲームと縦画面ゲームで、**同一チェイン・同一パラメータのまま線幅/glow の見え方を一致**させる。あわせて散在する解像度正規化を1関数に集約する。

#### 原因(確定)
`drawbgfx.cpp:3879-3882` で正規化基準 `m_vec_res_w` を **CRT フェース矩形の幅だけ**から取っている:

```cpp
const float screen_w = screen_max_x - screen_min_x;
m_vec_res_w = (screen_w > 1.0f)
    ? std::clamp(screen_w, 64.0f, float(s_width[window_index]))
    : float(s_width[window_index]);
```

ビーム幅・sigma・glow・cap・bloom など約15箇所が `× (m_vec_res_w / 1920.0f)` でスケールしている。

- 横画面(4:3・高さフィット):フェース矩形 ≈ 1440×1080 → `res_scale = 0.75`
- 縦画面(表示幅が横の 1/3):フェース矩形 ≈ 480×1080 → `res_scale = 0.25`

→ 同じ `beam_width` でも **0.75 対 0.25 = 3倍** 線幅が変わる。これが「縦横でパラメータを変えないと大幅に見え方が違う」の正体。

`:3809-3813` のコメントにある通り、以前 lit-line bbox から CRT フェース幅へ変えたのは「疎な画面で細くなりすぎる」対策だったが、**縦画面では幅そのものが小さいため今度は逆に細くなりすぎている**。

なお `screen_h`(高さ)は同じ走査ループで既に計算済み(`screen_max_y - screen_min_y`、`m_edge_box_*_y` に格納)。追加走査ゼロで基準を作り直せる。

#### 基準の選択肢

| 基準 | 横画面 | 縦画面 | 縦横差 | 備考 |
|---|---|---|---|---|
| 幅のみ(現状) | 0.75 | 0.25 | 3.0× | — |
| 高さのみ | 0.56 | 0.56 | **1.0×(一致)** | 横モニタで常に高さフィット=現環境に最適だが、縦モニタ/ストレッチで崩れる |
| major = max(w,h) | 0.75 | 0.56 | 1.33× | 向き混在で汎用だが完全一致はしない |
| **フィット軸自動判定** | 0.56 | 0.56 | ~1.0× | **本命**(向き・モニタ問わず堅牢) |

#### 修正内容

**段階1(最小・即効):高さ基準で動作確認**
現環境(横モニタ・高さフィット)なら高さ基準で縦横一致する。まずこれで効果を確認。

**段階2(本採用):フィット軸自動判定**
実際にウィンドウ端に達している(=表示スケールを決めている)軸の実ピクセル長を基準にする。向き・モニタ構成に依存しない。

`drawbgfx.h` に追加:
```cpp
float m_vec_res_h = 0.0f;   // m_vec_res_w の縦版(CRT フェース矩形の高さ, px)

inline float res_scale() const {   // 旧 (m_vec_res_w / 1920.0f) の唯一の定義
    const uint32_t si = window().index();
    const float fill_w = m_vec_res_w / std::max(1.0f, float(s_width[si]));
    const float fill_h = m_vec_res_h / std::max(1.0f, float(s_height[si]));
    const float basis = (fill_h >= fill_w) ? m_vec_res_h : m_vec_res_w;   // より埋めている軸
    return basis / 1920.0f;   // 基準 1920 は踏襲(再較正不要)
}
```

`drawbgfx.cpp:3880` 付近、`m_vec_res_w` 格納の直後に:
```cpp
const float screen_h = screen_max_y - screen_min_y;
m_vec_res_h = (screen_h > 1.0f)
    ? std::clamp(screen_h, 64.0f, float(s_height[window_index]))
    : float(s_height[window_index]);
```

#### `/1920` 置換対象と適用範囲
`(m_vec_res_w / 1920.0f)` を `res_scale()` に置換する箇所(ビームのスポットサイズ系 = 向き不変にすべき対象):

- `:1794`(res)、`:2356-2357`(width/normal_width)、`:2548`(overload_bloom σ)、`:2564`(edge_defocus)、`:2569`(hv_droop)、`:2604`(oglow_sig)、`:2672`(analytic_glow_width)、`:2777`(res)、`:3003`(line_cap_max)、`:3316`(width)、`:3383`(cap_res_scale)、`:4236`(convergence_bloom_source_radius)、`:4620`(convergence_bloom_falloff)

**そのまま(基準変更しない)箇所:**
- edge glow の r 位置計算(`:2557-2569` の `sw`/`sh` 直接使用)= 画面内の位置であって magnitude ではない。
- energy の `e_screen_ref`(`:2170-2171, 3196-3197`)= 速度正規化。フィット軸に揃えると縦横でエネルギーも一致するので**切替推奨**だが、切替前後で Vectrex の見え方を1回確認してから。

#### 検証
横画面ゲーム(Asteroids 等)と縦画面ゲームを **同一チェイン・同一パラメータ**で並べ、線幅・glow が一致するかスクショ比較。
1. まず「高さのみ」基準で即効性を確認。
2. 問題なければ「フィット軸自動判定」に格上げ。
3. 既定チェインで横画面の before/after が実質不変(0.75 のまま)であることも確認。

#### 工数・リスク
小(半日)。`m_vec_res_h` 追加 + `res_scale()` 導入 + 約14箇所の置換。リスク低(横画面は基準ほぼ不変、縦画面のみ改善)。energy 系の切替のみ Vectrex で1回目視。

---

### FU-3(副):t0/t1 非対応ドライバ向け arrival 順フォールバック

#### 目的
`generic_beam_energy()`(`drawbgfx.cpp:2036`)がタイミング情報を持たないドライバで `return I`(フラット)に落ちる(`:2040-2041`)。ここに #15692 の arrival モデルを入れ、**t0/t1 を出さないゲームでも擬似的なフレーム内グラデーション**を出す。

#### 認識
Vectrex のような実 dwell/速度ダイナミクスではなく、**ディスプレイリストの描画順ベースの近似**(「フラットよりマシ」)。既定 0 で完全に従来動作。

#### 修正内容
```cpp
if (!(prim->t0 >= 0.0 && prim->t1 > prim->t0)) {
    if (m_vs.scan_persist_ms <= 0.0f || N <= 1) return I;    // 0 = 従来フラット
    const float arrival = float(draw_index) / float(N - 1);   // 0..1
    const float age_ms  = m_vec_frame_interval_ms * (1.0f - arrival);
    return I * expf(-age_ms / m_vs.scan_persist_ms);           // 後発ほど明るい
}
```
- フレーム先頭で LINE 総数 `N` を取得(既存の `vector_count` を流用可)、`draw_index` は描画ループで加算。
- スライダー `scan_persist_ms`(既定 0 = 全チェイン従来、generic チェインのみ ~4ms)。

#### 検証
Tempest/Asteroids(DVG・t0/t1 なし)で on/off。Vectrex/Star Wars(t0/t1 あり)はこの分岐に入らないので影響ゼロを確認。

#### 工数・リスク
小(半日)。既定 0 で無効なので低リスク。FU-1 とは独立。

---

## 見送り・保留

- **FU-2 body インスタンシング**:見送り。削れるのは CPU 頂点生成/バンド幅のみで、**フラグメント/フィルレート/パス数は減らない**。ボトルネックは iGPU の GPU 側(大 FP16 FBO への多数 bloom/glow パス)なので効果薄。GPU コスト削減が必要になれば別軸(解像度スケール/パス削減 = #15692 の `clamp(height/1080)` 相当)で。
- **σ フロア(旧 FU-1a)**:向き問題とは無関係の別改善(サブピクセル極細線の減光防止)。VecBeam は既に footprint box積分 AA(`fs_vector_line_analytic.sc:119-125`)を持つため優先度低。余力があれば後日。
- **FU-4 空フレーム識別フラグ**:保守性向上のみ。低優先。
- **FU-5 パラメータ経済性**:balanced 以外を廃止予定のため当面スキップ。
- **FU-6 色相保存 Reinhard**:SDR フォールバック present の部分借用。実 HDR 出力経路は VecBeam 資産なので触らない。低優先。

---

## 着手順

1. **FU-1 段階1**(高さ基準)で縦横一致を確認 →
2. **FU-1 段階2**(フィット軸自動判定)へ格上げ →
3. **FU-3**(arrival 順フォールバック)。

いずれも既定値ニュートラルにでき、既存チェインの見た目を壊さない。

## 参考(現行コード位置)

- 正規化基準算出:`drawbgfx.cpp:3879-3882`(`m_vec_res_w`)、`screen_h` は `:3861-3874` で既算出
- `/1920` 使用箇所:`drawbgfx.cpp:1794, 2356, 2357, 2548, 2564, 2569, 2604, 2672, 2777, 3003, 3383, 4236, 4620`
- generic energy:`drawbgfx.cpp:2036`(フラット return `:2040-2041`)
- 頂点構造体:`bgfx/vertex.h:40`(`AnalyticLineVertex`)
- analytic shader:`bgfx/shaders/vector/fs_vector_line_analytic.sc`

---

## 精査追記（2026-07-24・本節を既存記述より優先）

### 総合判定

**要修正。現状のまま実装には着手しない。**

- **FU-1: 要修正 / 詳細設計 Pending**
- **FU-3: 要修正 / 詳細設計 Pending**
- 見送り・保留項目は現判定を維持する。ただし FU-1/FU-3 の調査結果によって優先度を再評価してよい。

既存節にある「原因（確定）」「フィット軸自動判定を本採用」「小（半日）・低リスク」「既定値ニュートラル」といった判定は、本追記によって撤回または保留とする。

### 実装前に必要な前準備

1. 一時診断ログを追加し、少なくとも Asteroids / Tempest / Quantum / Vectrex / Star Wars について次を採取する。
   - `window_w`, `window_h`
   - VECTORBUF 由来の `screen_w`, `screen_h`
   - `fill_w`, `fill_h`
   - 現行 `m_vec_res_w / 1920`
   - 各候補方式が選ぶ basis と scale
2. 横画面の既存較正を維持するか、1080-line 等の新しい基準へ移行して全パラメータを再較正するかを決める。
3. 横長モニタだけでなく、縦長・正方形ウィンドウ、非等方 stretch、複数 vector screen の扱いを決める。
4. `t0/t1` を持たない vector ドライバと実ゲームを列挙し、FU-3 の実対象と効果範囲を確定する。
5. 上記結果を本書へ反映するまで、FU-1/FU-3 の詳細設計を **Pending** とする。

## FU-1 精査結果 — 要修正 / 詳細設計 Pending

### 現案の矛盾

提案式では、1920×1080 上の 1440×1080 横画面について次の結果になる。

```text
fill_w = 1440 / 1920 = 0.75
fill_h = 1080 / 1080 = 1.0
basis = 1080
res_scale = 1080 / 1920 = 0.5625
```

したがって、横画面も現行の 0.75 から 0.5625 へ25%変化する。既存節の「横画面は0.75のまま」「横画面のbefore/afterは実質不変」という記述とは両立しない。

通常の横モニタで高さフィットするケースでは、段階1（高さ固定）と段階2（現行のフィット軸判定）はほぼ同じ結果になる。このため、現在の段階1→段階2は有効な段階設計になっていない。

### 原因仮説の再確認

「縦画面のCRTフェース矩形 ≈ 480×1080」は未実測であり、一般的な vector game の visarea から予想される寸法としては疑義がある。以前使用していた lit-line bbox と現在の VECTORBUF フェース矩形を混同している可能性もある。

よって「0.75対0.25の3倍差」を原因として確定扱いせず、前記の診断ログで実測してから判断する。

### 現行の自動判定が一般解でない理由

現案が判定するのは「ウィンドウに対してより埋まっている軸」であり、物理的なpixel densityや論理画面上の一定距離ではない。次の条件では、一意な方向不変スケールを保証できない。

- 縦長モニタ
- 正方形または極端なアスペクト比のウィンドウ
- 非等方 stretch
- 両軸が同程度に埋まる場合（現在式は `>=` により高さを選ぶだけ）
- 複数 vector screen（矩形unionが画面間の空白を含み得る）

候補式を採用する前に「何を一定にするスケールか」を定義する。候補は以下。

- 既存横4:3較正を維持する viewport-fit 基準
- 1080-line 等の出力解像度基準（既存値は再較正）
- CRTフェースの短辺・長辺・幾何平均等の対称関数
- 論理visareaとwindow transformから得る px-per-logical-unit

最終案は診断ログと比較画像を見て決めるため、詳細設計は Pending とする。

### `/1920` 置換対象の補足

既存一覧には次が不足している。

- `drawbgfx.cpp:4842-4844`: edge glow の width/length。spot-size 系なので共通化対象。
- `drawbgfx.cpp:4045`: junction 判定許容幅の `s_width / 1920`。共通基準へ含めるか個別判断する。
- `drawbgfx.cpp:3316`: 文書末尾の参考一覧から漏れている。

現時点の関連箇所は次のとおり。

```text
1794, 2356, 2357, 2548, 2564, 2569, 2604, 2672,
2777, 3003, 3316, 3383, 4045, 4236, 4620, 4842
```

`e_screen_ref` はspot-sizeではなく速度正規化基準なので、FU-1から分離して設計する。変更する場合は、描画本体とpre-passで不一致を起こさないよう次の4経路を同時に扱う。

```text
drawbgfx.cpp:2170-2171
drawbgfx.cpp:3196-3197
drawbgfx.cpp:3927-3928
drawbgfx.cpp:4153
```

### FU-1の改訂後着手条件

- 実ゲームのCRTフェース寸法が採取済みであること。
- スケールの意味と、横画面の後方互換方針が決定済みであること。
- 横長・縦長・正方形・stretch・複数画面の期待動作が定義済みであること。
- 採用式、全置換対象、比較条件が本書へ反映済みであること。

これらを満たすまで、工数・リスクは **見積りPending** とする。単純なメンバー追加と置換だけなら小規模だが、実測と再較正を含めて「半日・低リスク」とは評価しない。

## FU-3 精査結果 — 要修正 / 詳細設計 Pending

### #15692との相違

既存案は #15692 の arrival モデルをそのまま借用する内容ではない。#15692 は次の方式を採る。

- line indexではなく累積セグメント長でarrivalを決める。
- `max(segment length, primitive width, 1px)` を時間重みとして使う。
- 一本の線の内部でもarrivalを補間する。
- 減衰をbeamの最終depositへ掛け、線幅やenergyモデルには掛けない。
- scan内persistenceに最低10フレーム相当の下限を設ける。

### 当初案の問題

1. **4msは減衰が強すぎる。**
   - 60Hzでフレーム先頭の係数は `exp(-16.67 / 4) ≈ 0.015` となり、約1.5%まで暗くなる。
   - #15692の10フレーム下限では、最古部でも概ね `exp(-0.1) ≈ 0.905` であり、意図は穏やかなscan variationである。
2. **適用段が不適切。**
   - `generic_beam_energy()` の戻り値へ掛けると、明るさだけでなく線幅、overload、glow、convergence bloom、dwell cap等の入力まで変わる。
   - arrivalはenergy/幅決定後のdeposit強度へ掛ける設計を基本候補とする。
3. **line indexは時間近似として弱い。**
   - 長線、短線、dotをすべて同じ時間として扱うため、一定deflection speedの近似にならない。
4. **検証対象が誤っている。**
   - 現行VecBeamのAVG/DVGは`t0/t1`を生成する。Tempest/Asteroidsは原則fallbackに入らないので、効果確認対象には使えない。
   - Tempest/Asteroidsはtimed経路の非回帰確認に使用する。
5. **実装項目が不足している。**
   - `vec_slider_cache` へのフィールド追加
   - `VEC_SLIDER_DEFS` への登録
   - 対象chain JSONへのslider追加
   - emulated frame intervalの取得とpause/reset/rewind処理
   - pre-passと実描画で同じprimitive arrivalを参照する仕組み
   - flicker exclusionを含むLINE総数とdraw indexの整合

### 改訂設計候補

- untimed primitiveをフレーム先頭で列挙し、#15692相当の累積長arrivalをprimitive単位でキャッシュする。
- 可能ならanalytic lineのvertex/varyingへstart/durationを渡し、線内arrivalをshaderで補間する。
- CPU側だけで実装する場合も `generic_beam_energy()` ではなく、energy/幅決定後のdeposit強度へ係数を掛ける。
- scan内時間定数は独立sliderにするか既存phosphor persistenceから導出するかを決める。初期候補は #15692 同様の `max(persistence, frame_interval * 10)`。
- 既定0で完全無効にする場合、「generic chainのみ約4msを既定」とは両立しないため、chainごとの既定値を明示する。

### FU-3の改訂後着手条件

- 実際のuntimedドライバと対象ゲームが確定済みであること。
- arrivalをline index、累積長、実時間のどれで定義するか決定済みであること。
- temporal係数の適用段と時間定数が決定済みであること。
- timed source、pause、reset、rewind、flicker exclusionの期待動作が定義済みであること。
- 効果が小さすぎる、または対象が少なすぎる場合は実装を見送ること。

これらを満たすまで、工数・リスクは **見積りPending** とする。「半日・低リスク」という既存評価は撤回する。

## 改訂後の着手順

1. CRTフェース寸法と`t0/t1`対応状況の診断ログを採取する。
2. FU-1のスケール定義と後方互換方針を決め、本書の詳細設計を更新する。
3. FU-1候補方式を同一パラメータで比較し、採用方式確定後に実装する。
4. FU-3のuntimed対象、arrival定義、適用段、時間定数を決め、本書の詳細設計を更新する。
5. FU-3の費用対効果が確認できた場合に限り実装する。

診断と設計判断が終わるまでは、FU-1/FU-3とも実装ステータスを **Pending** とする。

---

## 実装結果（2026-07-24・精査追記のPendingを更新）

### ステータス

- **FU-1: 実装済み / ビルド・代表ゲーム回帰確認済み**
- **FU-3: 実装済み / 既定0（無効） / untimed代表ゲームで有効時の安定動作確認済み**

前節でPendingとしていた前準備と詳細設計は、実測結果に基づいて以下のとおり確定した。

## 診断実測結果

同一の `vector-color-balanced` chain、1080-lineウィンドウで採取した代表値。

| Game | Window / CRT face | 旧 width scale | timing |
|---|---:|---:|---:|
| Asteroids | 1440×1080 | 0.750000 | timed 130 / untimed 0 |
| Tempest | 810×1080 | 0.421875 | timed 299 / untimed 0 |
| Quantum | 810×1080 | 0.421875 | timed 39 / untimed 0 |
| Vectrex | 779×1080 | 0.405729 | timed 511 / untimed 0 |
| Star Wars | 1440×1080 | 0.750000 | timed 1019 / untimed 0 |
| Cosmic Chasm | 810×1080 | 0.421875 | timed 0 / untimed 120 |
| Tac/Scan | 810×1080 | 0.421875 | timed 0 / untimed 200 |

フルスクリーン実測では実ディスプレイ解像度3840×2160が使用され、Asteroids/Star WarsのCRT faceは2880×2160、Tempest/Quantum/CChasm/Tac/Scanは1620×2160、Vectrexは約1556.5×2160だった。旧scaleは横4:3が1.5、縦・正方形系が約0.81〜0.844となり、幅基準による方向差を確認した。

当初案の「縦画面≈480×1080、3倍差」は実測と一致しなかったため撤回する。実際の主な差は0.75対0.421875（約1.78倍）だった。

## FU-1 確定設計と実装

### 採用基準

VECTORBUFから得た実フェース寸法 `m_vec_res_w`, `m_vec_res_h` と、windowに対する各軸の充填率を使う。

- 高さ制約（`fill_h` が `fill_w` 以上）: `reference_w = m_vec_res_h × 4/3`
- 幅制約: `reference_w = m_vec_res_w`
- `res_scale = reference_w / 1920`
- 浮動小数点誤差による軸反転を避けるため、充填率比較に `1e-4` の許容差を入れる。

4:3換算を入れることで、既存の横4:3較正を維持しながら、同じ制約軸で表示される縦・正方形ゲームを同じspot-size scaleへ揃える。

1080-lineウィンドウでの新scaleは、Asteroids/Star Warsの0.75を維持し、Tempest/Quantum/Vectrex/CChasm/Tac/Scanも0.75になる。3840×2160フルスクリーンでは全対象が1.5になる。

### 変更範囲

`drawbgfx.h`:

- `m_vec_res_h` を追加。
- 4:3換算と制約軸判定を集約した `vec_res_scale()` を追加。

`drawbgfx.cpp`:

- spot-size系の旧 `m_vec_res_w / 1920` を `vec_res_scale()` へ置換。
- 当初漏れていたedge glow（旧4842付近）も共通化。
- junction許容幅も同じ物理scaleへ統一。
- energy速度正規化の `e_screen_ref` は意味が異なるため変更していない。

## FU-3 確定設計と実装（旧Persistence案・後節のVariation仕様で置換）

### 対象

`t0/t1`を持たないprimitiveのみを対象とする。実測でCosmic ChasmとTac/Scanが全線untimed、Asteroids/Tempest/Quantum/Vectrex/Star Warsが全線timedであることを確認した。

### arrivalモデル

- untimed primitiveだけをリスト順に走査する。
- 各primitiveの重みは `max(segment_length, primitive_width, 1px)`。
- 累積長からprimitive中央のarrivalを算出する。
- `age_ms = frame_interval_ms × (1 - arrival)`。
- `tau_ms = max(scan_persist_ms, frame_interval_ms × 10)`。
- `attenuation = exp(-age_ms / tau_ms)`。

#15692と同じく10フレーム下限を設け、最古部でも概ね0.905となる穏やかなvariationにした。現実装はCPU側primitive中央近似であり、#15692のfragment単位の線内補間までは行わない。

### 適用段

attenuationは `generic_beam_energy()` へ掛けず、energy・線幅・overload判定が完了した後の最終depositへだけ適用する。core、glow、flare、halation、ray、capのdeposit強度を同率で減衰させる。

### 時間処理

- emulated frameが進んだ時だけframe intervalを更新する。
- MVEC playbackでは記録済みのplayback intervalを使う。
- pause中は最後のintervalを保持する。
- playback reset時はscan time historyをリセットする。

### sliderと既定値

balanced系のgeneric chainへ次を追加した。

```text
Untimed Scan Persistence (ms)
name: scan_persist_ms
default: 0
range: 0..500 ms
```

追加先:

- `vector-color-balanced.json`
- `vector-color-balanced-portrait.json`
- `vector-monochrome-balanced_1.json`
- `vector-monochrome-balanced_2.json`

既定0なので従来動作を維持する。Vectrexはtimed sourceなのでVectrex専用balanced chainには追加していない。

## 検証結果

- UCRT64 GCC 16.1.0による増分ビルド成功。
- 変更した4つのchain JSONをコメント除去後にJSON parseし、すべて成功。
- `scan_persist_ms=100` の一時設定でCosmic Chasm / Tac/Scanを実行し、untimed fallbackの安定動作を確認。
- 同じ一時設定でAsteroidsを実行し、timed sourceがfallback対象外でも正常動作することを確認。
- 既定値を0へ戻した最終状態で次の7本を各2秒実行し、すべてexit code 0。
  - Asteroids
  - Tempest
  - Quantum
  - Vectrex
  - Star Wars
  - Cosmic Chasm
  - Tac/Scan

残作業は主観画質の比較・slider tuningのみ。コード上の実装と基本回帰確認は完了している。
---

## FU-3 修正版（2026-07-24）

### 修正理由

旧 `Untimed Scan Persistence (ms)` は `tau_ms = max(scan_persist_ms, frame_interval_ms × 10)` としていたため、60 Hzでは0〜約166.7 msの広い範囲が同じ10フレーム下限へ丸められた。また値を大きくするほど減衰差は小さくなるため、利用者の「値を大きくしても変化が見えない」という観察と一致する。時間定数スライダーとして直感に反するため、この仕様を撤回した。

### 新仕様

スライダーを見た目に直結する強度指定へ変更する。

```text
Untimed Scan Variation (%)
name: scan_variation
default: 0
range: 0..100 %
step: 5 %
```

- 0%: 無効。従来どおり全primitiveを同じ強度でdepositする。
- 25%: リスト最古部が約90%となり、#15692相当の穏やかな差。
- 50%: リスト最古部が約76%となる中程度の差。
- 75%: リスト最古部が約55%となる強い差。
- 100%: リスト最古部が約25%となり、効果を明瞭に確認できる。

untimed primitiveのリスト順・累積長・primitive中央arrivalの定義は維持する。`age = 1 - arrival`、`dimming = 0.4 × strength + 0.35 × strength³`、`oldest_scale = 1 - dimming`、`attenuation = pow(oldest_scale, age)` とする。25%付近の穏やかな応答を維持しながら、上限側だけを三次項で強める。frame intervalや時間履歴には依存しない。attenuationは引き続き最終depositだけへ適用し、energy、線幅、overload判定は変更しない。timed primitive（AVG/DVG/Vectrex等）は対象外である。

### 修正版の検証

- UCRT64 GCC 16.1.0で増分ビルド成功。
- 変更した4つのbalanced chain JSONをparseし、すべて成功。
- 上限強化後の `scan_variation=100` の一時設定でCosmic Chasm / Tac/Scanを各2秒実行し、untimed fallbackの有効経路がexit code 0であることを確認。
- 同じ一時設定でAsteroidsを2秒実行し、timed sourceの除外経路がexit code 0であることを確認。
- 検証後、4 chainの既定値が0であることを確認する。

FU-3の詳細設計と実装ステータスは、本節の仕様で **実装済み / 基本回帰確認済み** とする。