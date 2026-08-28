# 起動時オプション リファレンス

English: [startup-options.md](startup-options.md)

対象: VecBeamMAME（MAME 0.289 ベース）

**素の MAME 0.289 に無く、VecBeamMAME が追加した起動時オプション 27 個**の全リファレンス。
コマンドラインと ini の両方で指定できる。

チェイン側のスライダー(実行中に変えるもの、cfg に保存されるもの)は
[追加パラメータ一覧](added-parameters.ja.md) を参照。**この文書は起動時に決めるものだけ**を扱う。

---

## 1. ini ファイルの書き方

### 1-0. ini を作る

まず雛形を作る。**VecBeamMAME の実行ファイルは `vbmame`**、読む ini は **`vbmame.ini`** で、
素の MAME とは別物（同じフォルダに両方置ける）。

```
vbmame -createconfig
```

短縮形は `-cc`。カレントディレクトリに `vbmame.ini`（全オプションの既定値入り）と `ui.ini` ができる。
以後はこのファイルを編集する。

**素の MAME から移ってきた場合、既存の `mame.ini` は読まれない。** 名前を `vbmame.ini` に
変えるか、上のコマンドで作り直して必要な行を移すこと。ゲームごとの `cfg/` `nvram/` は
そのまま使える。

### 1-0b. 効かない素の MAME オプション

`beam_width_min` / `beam_width_max` / `beam_dot_size` / `beam_intensity_weight` は
**bgfx ベクターチェイン使用時は効きません。** ini に書いても、Tab → Slider Controls の
同名スライダー（一覧の上のほうにある素の MAME 側）を動かしても変わりません。ビーム幅は
下のほうに並ぶ**チェインスライダー**の値で決まります。そちらを触ってください。

`flicker` は効きますが、チェイン側の周期フリッカと二重になるため、同梱の ini プリセットでは
`flicker 0.00` にしてあります。そのままにしてください。

詳しくは [FAQ](faq.ja.md) の「ini や Tab メニューの `beam_width_min` が効きません」を参照。

### 1-1. 書式

1 行 1 オプション。`-` は付けない。値との区切りは空白(何個でもよい)。`#` 以降はコメント。

```ini
#
# CORE VECTOR OPTIONS
#
vector_beam_window        1
vector_present_rate       auto
bgfx_hdr                  1
bgfx_hdr_display_peak     auto
```

**真偽値**は `1` / `0` を書く。コマンドラインの `-novector_playback_overlay` のような
`no` 接頭辞は ini では使わず、`vector_playback_overlay 0` と書く。

**パス型**(`vector_record` など)は値を空にすると未設定になる。

### 1-2. ini の探索場所と優先順位

`inipath`(既定 `$HOME/.mame;.;ini;ini/presets`)から探し、**下にあるものが上を上書きする**。

| ini | 適用対象 | 優先度 |
|---|---|---|
| `vbmame.ini` | すべて | 101(最弱) |
| `debug.ini` | デバッガ起動時 | 102 |
| `horizont.ini` / `vertical.ini` | 画面の向き別 | 103 |
| `vector.ini` / `raster.ini` / `lcd.ini` など | **画面種別** | 104 |
| `source/<ソースファイル名>.ini` | ドライバのソース単位(例 `source/starwars.ini`) | 105 |
| `<祖父セット>.ini` | | 106 |
| `<親セット>.ini` | | 107 |
| `<ドライバ>.ini` | | 108 |
| `<セット名>.ini` | 例 `starwars.ini` | 109 |
| **コマンドライン** | | **151(最強)** |

ベクター機だけに効かせたいなら `vector.ini`、Star Wars だけなら `starwars.ini` に書く。

VecBeamMAME には `ini/presets/vector.ini` と `ini/presets/vector-mono.ini` が同梱されている。
`inipath` に `ini/presets` が含まれているので、`ini/vector.ini` を作れば presets 側を上書きできる。

### 1-3. 優先度 105 の境界に注意

`bgfx_screen_chains` は **105(`source/*.ini`)以上で指定すると「明示指定」扱い**になり、
cfg に保存されたチェイン選択を読まず、また cfg へ書き戻さない。
`vbmame.ini`(101)に書いた場合は通常の既定値として扱われ、cfg の選択が優先される。

つまり:

- **全体の好みとして**チェインを決めたい → `vbmame.ini` に書く
- **そのゲームでは必ずこれ**と固定したい → `<セット名>.ini` かコマンドライン

---

## 2. ベクター描画 コアオプション

`-vector_*`。エミュレーション側(`src/emu`, `src/devices/video/vector.cpp`)のオプションで、
レンダラを問わず効く。

### `vector_quality` / 別名 `vecq`

| | |
|---|---|
| 型 / 既定 | string / **未設定(何も変えない)** |
| 値 | `high` / `medium`(別名 `middle`) / `low` |

**描画負荷を決める4つの設定をまとめて指定するプリセット。** 個別に
`bgfx_render_scale` / `bgfx_output_scale` / `vector_beam_window` / `vector_present_rate`
を覚えていなくても、機体の余力に合わせて1語で選べる。

| 値 | `bgfx_render_scale` | `bgfx_output_scale` | `vector_beam_window` | `vector_present_rate` |
|---|---|---|---|---|
| `high` | `1.0` | `1.0` | オン | `auto` |
| `medium` | `0.75` | `0.75` | オン | **`60`** |
| `low` | `0.5` | `0.5` | **オフ** | `0`(present ループ自体が回らない) |

実際に効くのは解像度スケールの2本で、ビーム窓はその上に乗る演出。**余力が無い機体で
最初に落とすのが窓**なので `low` だけ窓を切る。

**present レートを含めているのは、60 Hz を超えるモニタ対策。** 窓がオンだと present レートは
`auto`(モニタのリフレッシュ)に昇格するので、144 Hz のパネルでは蛍光体・モニタチェインが
毎秒 144 回走る。しかも唯一の上限装置である `vector_phosphor_rate` は、窓がオンのときは
効かない(present ごとに掃引の別スライスを運んでいるため、前の出力を再提示できない)。
そこで `medium` はモニタ任せにせず 60 に固定する。

`low` を `0` のままにしてあるのは意図的で、**窓がオフなら昇格が起きず present タイマーが
そもそも生成されない**(レンダラとして一番安い状態)。ここにレートを書くと、今は存在しない
ループを新設することになり、かえって重くなる。

**プリセットは出発点なので、明示指定に負ける。** 上表の4項目のうち、**まだ誰も触って
いないもの(優先度 0 = 組み込みの既定値のまま)だけ**を埋める。ini(101 以上)でも
コマンドライン(151)でも、一度書かれた値はそのまま残る
(→ [1-2](#1-2-ini-の探索場所と優先順位))。つまり
`-vector_quality low -vector_beam_window` は「解像度は半分、窓はオン」になり
(このとき窓が働くよう present レートは `auto` へ昇格する)、`vbmame.ini` に
`bgfx_render_scale 1.0` と書いてあれば `low` でも解像度は下がらない。
`-vector_quality medium -vector_present_rate 120` なら 120 Hz がそのまま通る。

起動時のログでどれが効いたか分かる。

```
Vector presentation timer enabled at 60 Hz (from -vector_quality)
Vector presentation timer enabled at auto, initial 60 Hz (requested by vector_beam_window)
```

`high` / `medium` / `low` 以外を渡すと警告を出して**プリセットごと無視**する
(4項目とも設定どおりのまま)。

```
Unknown -vector_quality 'ultra'; expected high, medium or low. Ignoring.
```

なお `vector_quality` 自体は `src/emu` のオプションだが、埋める4項目のうち2つは
BGFX 側(→ [4](#4-bgfx-レンダラ-オプションvecbeam-追加分))なので、**BGFX 以外の
レンダラでは窓と present レートの指定だけが効く。**

### `vector_overscan_x` / `vector_overscan_y`

| | |
|---|---|
| 型 / 既定 | float / `1.0` |

画面中心を基準としたベクター像のズーム倍率。**1.0 未満にすると像が縮み、
本来は画面外へ振れていたビームが可視領域に入ってくる。**

実機のモニタは可視面よりわずかに広い範囲まで偏向できる。ゲームが意図的に画面外へ
ビームを振る場面(Star Wars の爆発、Major Havoc の窓外)で、実機なら管面の端が光る。
その光り方を見たい / モニタグローの元になるビームを画面内で確認したいときに下げる。

`0.95` 前後で端の挙動が見えるようになる。像が小さくなるので通常プレイでは `1.0`。

### `vector_blank_leak`

| | |
|---|---|
| 型 / 既定 | float / `0.0`(オフ) |

ブランク中(`intensity == 0` の移動・リトレース)の軌跡を微小輝度で描く。

実機の Z 軸ブランキングは完全に切れず、わずかに漏れる。速いジャンプほど滞留が短いので
薄く、遅い移動ほど濃く出る(`beam_energy` が `-1` = 未供給のデバイスでは速度から推定)。

**0 のときは頂点自体を発行しない**のでコストはゼロ。ブランク移動の幾何情報はデバイス側に
しかないため、チェインのスライダーにはできない。色は AVG の STAT カラーを使う。

### `vector_beam_window` / 別名 `beamwin`

| | |
|---|---|
| 型 / 既定 | bool / **`1`(オン)** |

**1回の present で、ビーム掃引のうちその present が担当する時間スライスだけを堆積する。**
残りは蛍光体の残光が保持する。実機の CRT で「1フレームかけて順に描かれている」状態を
再現するもので、掃引が長いゲームでは走査の進行そのものが見える。

窓に切る対象が無いと意味がないので、**このオプションがオンのとき
`vector_present_rate` が既定値のままなら `auto` に昇格する**(明示指定した値は尊重される)。

```
Vector presentation timer enabled at auto, initial 60 Hz (requested by vector_beam_window)
BGFX: beam time window active - sweep 29.90 ms over 12.50 ms windows (2.4 per sweep), scale 2.00
```

掃引が 1 窓に収まるゲーム(asteroid など)では窓に切る余地が無く、`inert` と報告して
フレーム単位の描画に戻る。

**コスト**: present ループが全ベクター機で回るので、窓が働かないタイトルでも
フレーム単位経路の 2〜4 倍のベクター CPU を使う。切るには `-novector_beam_window`、
または `-vector_present_rate 0`。

チェイン側にも `beam_window` / `beam_window_scale` スライダーがあり、**両方オンでないと働かない**
(起動時のマスタスイッチがこのオプション、実行中の調整がスライダー)。

### `vector_window_sim`

| | |
|---|---|
| 型 / 既定 | bool / **`0`(オフ)** |

**クリップ窓の「揺らぎ」のスイッチ。** ラッチされた辺の位置がフレームごとに少しずつ
違う場所に落ちる現象(`vector_window_jitter`)を出すかどうかを決める。窓が揺れて見える
のはこの項目だけで、残る3本(垂下・誘電吸収・オフセット)は**常に有効**。

**既定はオフ。** Major Havoc は下端の1辺が動くだけなので実機どおりに見えるが、
Battlezone は矩形の4辺すべてが保持値なので、同じ量でも「枠が呼吸する」ように見える。
そのため opt-in にしてある。

```
vbmame mhavoc -vector_window_sim
```

### `vector_window_droop` / `vector_window_memory` / `vector_window_jitter` / `vector_window_bias`

| オプション | 型 | 既定 |
|---|---|---|
| `vector_window_droop` | float | `5.0` |
| `vector_window_memory` | float | `0.005` |
| `vector_window_jitter` | float | `1.0`(ただし `vector_window_sim` が必要) |
| `vector_window_bias` | float | `0.5` |

**この回路を持つ機種だけに効く。** Major Havoc はスクロール領域の上端(`ymin` クリップ)の1辺、
Battlezone はクリップ矩形の4辺すべてを、アナログスイッチとホールドコンデンサで保持している
(同じ LF13201 と同じ 1000pF)。その回路の非理想性を再現する 4 本。

**`window_jitter` だけスイッチが要る。** これはラッチ値そのもののばらつきで、**フレーム間で
位置が動く唯一の項目**＝窓が揺れて見える正体。値は実機に寄せた `1.0` が入っているが、
`vector_window_sim` がオフのあいだは 0 として扱われる(→ 上記)。

残る3本(垂下・誘電吸収・オフセット)は実機に寄せた値が既定に入っていて、**常に有効**。これらは
**フレーム間では動かない**(位置が一定量ずれる／描画順に沿って傾く)ので、揺れとしては見えない。
外すならその項目に `0` を渡す。

| オプション | モデル | 単位 | オフの値 |
|---|---|---|---|
| `window_droop` | ホールド電圧の垂下 | 画面高に対する % / 秒 | `0`(理想ホールド) |
| `window_memory` | ホールドコンデンサの誘電吸収(過去のサンプルに引っ張られる) | 0..1 | `0` |
| `window_jitter` | サンプルごとの散らばり。**フレーム内では一定** | 画面高に対する % | `0` |
| `window_bias` | 系統的オフセット(チャージインジェクション / コンパレータオフセット) | 画面高に対する %(符号あり) | `0` |

他のゲームでは効果がない。

### `vector_present_rate` / 別名 `vecpresent`

| | |
|---|---|
| 型 / 既定 | string / `0`(オフ) |
| 値 | `0` = オフ、`auto` = モニタのリフレッシュ、`1`〜`360` = 固定 Hz |

**エミュレーションのタイミングを変えずに**、完成した出力を指定 Hz で再提示する。
ソースフレーム周期(ゲーム側の描画レート)は変わらない。

`auto` は Windows では実デスクトップ更新率(分数精度)、macOS では `NSScreen` の
最大フレームレートを取得して最寄り整数 Hz を使う。**Linux には検出経路が無い**ので
60 Hz 固定になる(必要なら明示指定する)。

高リフレッシュのモニタで蛍光体の減衰を滑らかに見せる目的と、`vector_beam_window` の
土台としての目的がある。

**このレートに比例して増えるのは蛍光体・合成・モニタチェイン(GPU 側)だけ。** ベクター
プリミティブの構築と補助経路(glow / halation / no-persist / 星芒)はソースフレーム単位で
作られ、present だけのリフレッシュでは使い回されるので増えない。それでも 144 Hz は 60 Hz の
2.4 倍の合成コストなので、余力の無い機体では `-vector_quality medium`(60 に固定)か、
このオプションを直接 50〜60 に指定する。

### `vector_phosphor_rate` / 別名 `vecphosphor`

| | |
|---|---|
| 型 / 既定 | int (0-360) / `0`(無制限) |

高レート再提示中に、重い蛍光体 / モニタチェインの更新回数を秒あたりで制限する。

**新しいソースフレームは遅延させない。** 中間の present だけが完成済み出力を再利用する。
`vector_present_rate 240` などで負荷が厳しいときに、見た目をほぼ保ったまま GPU を空ける。

### `vector_event_dump`

| | |
|---|---|
| 型 / 既定 | path / 未設定 |

デバッグ用。タイムドビームイベントを 1 行 1 イベントの CSV で書き出す。
`tools/mvec-viewer/mvec-viewer.html` をブラウザで開いて可視化できる。

---

## 3. MVEC ビーム事象ストリーム

ゲームの実プレイで生成された**最終ベクターリスト**を記録・再生する。
記録は 1 フレーム = エミュレート 1 フレームで、レンダラより上流の中間表現なので、
**同じ MVEC を別のレンダラ設定に流せば差分はレンダラだけになる。**

### `vector_record`

| | |
|---|---|
| 型 / 既定 | path / 未設定 |

MVEC ストリームを書き出す。各フレームの最終ビームイベント
(x/y・色・intensity・beam_energy・t0/t1・cap_flags・Vectrex 由来メタデータ)を保存し、
同時に最終スピーカーミックスを 16-bit PCM の `<MVECパス>.wav` に自動保存する。

`vector_playback` と**排他**。1 マシンに複数のベクターデバイスがある場合は最初の 1 つだけ。

書き込みは専用スレッドで行うのでエミュレーション速度への影響は小さいが、
**容量は大きい**(1 point 66 バイト。starwars で 1 分あたり約 500MB)。

### `vector_playback`

| | |
|---|---|
| 型 / 既定 | path / 未設定 |

MVEC ストリームを再生する。ビームリスト・リスト世代・stale/timed フラグを記録値で
上書きするので、**CRT フリッカやタイミングモデルまで含めて録画時のセッションを
決定論的に再現する**。同時記録した `.wav` があればゲーム音声を置換してフレーム位置に同期する。

任意のチェインを通して再生できるので、実プレイを一度録っておけば**再現不要で正確な
A/B 較正**ができる。これが比較動画の土台。

**注意**: 再生は 1 記録フレーム = 1 スクリーン更新で進む。記録時のレートと
再生時のドライバのレートが違うと再生速度が狂うので、乖離があれば警告が出る。

### `vector_playback_start` / 別名 `vecstart`

| | |
|---|---|
| 型 / 既定 | int / `0`(先頭から) |

**再生開始位置。** 最初のフレームを出す前に、`Alt+G` のジャンプと同じ位置決めを済ませる。
数千フレーム先でしか起きない事象を調べるとき、毎回手でフレーム番号を打つ必要が無くなる
= **計測が再現可能になる。**

```
MVEC: starting playback at frame 4200
```

**フレーム番号は 1 始まり**で、オーバーレイの表示・`Alt+G` の入力と同じ数え方。
オーバーレイに出ている番号をそのまま渡せる。`-vector_playback_start 100` で止まるのは
オーバーレイが `100` と表示するフレームで、`Alt+G` に `100` と打った位置と一致する。

`0` 以下は指定なしとして扱い先頭から再生する。ストリーム長を超える値は**最終フレームに
丸める**(エラーにはならない)。

### `vector_playback_end`(旧名 `vector_exit_after_playback`)

| | |
|---|---|
| 型 / 既定 | int (0-2) / `0` |

ストリーム終端に達したときの動作。

| 値 | 動作 |
|---|---|
| `0` | 最終フレームを保持(黒画面にはしない) |
| `1` | プログラムを終了する |
| `2` | 先頭に戻ってループする |

`1` は動画の自動撮影で使う。両ビルドが同じフレーム数で終わる。

**旧名 `vector_exit_after_playback` はエイリアスとして残っているが、値が必須になった。**
BOOLEAN から INTEGER になったため、コマンドラインの値なしフラグ形式
`-vector_exit_after_playback` は使えない。しかも `-vector_exit_after_playback -window` のように
書くと**後続のオプションを値として食う**(警告は出る)。`-vector_playback_end 1` を使うこと。
ini では `vector_exit_after_playback 1` は従来どおり動く。

### `vector_playback_overlay`

| | |
|---|---|
| 型 / 既定 | bool / **`1`(表示)** |

再生位置オーバーレイの**初期表示状態**。既定は従来どおり表示する。

`-novector_playback_overlay` で非表示起動。動画撮影では画面に入れたくないので使う。
`Alt+O`(macOS では `Option+O`)のトグルは以後も両方向で有効で、
`Alt+G` のフレーム番号入力中は強制的に表示される(隠れたモーダル入力は入力ロックと
区別できないため)。

なお `-aviwrite` は UI レイヤーを含まないので、**AVI にオーバーレイは元々写らない**。
これは画面表示のためのオプション。

### 再生中のキー操作

すべて Alt(macOS では Option)の後ろ。

| キー | 動作 |
|---|---|
| `Alt+P` | 一時停止 / 再開 |
| `Alt+←` / `→` | ±1 フレーム |
| `Alt+PgUp` / `PgDn` | ±60 フレーム |
| `Alt+Home` / `End` | 先頭 / 末尾 |
| `Alt+G` | フレーム番号を入力してジャンプ(数字 → Enter、Esc で取消、Backspace で1文字消去) |
| `Alt+O` | オーバーレイ表示トグル |

フレーム番号は**1 始まり**(オーバーレイの表示と一致)。

---

## 4. BGFX レンダラ オプション(VecBeam 追加分)

`-bgfx_*`。OSD 側(`src/osd/modules`)のオプション。

### `bgfx_vec_line_shader`

| | |
|---|---|
| 型 / 既定 | string / `analytic` |
| 値 | `classic` / `analytic` |

ベクター線の描画方式。

- `analytic` — ガウシアン線積分。1 本の線分を解析的に評価するので、サブピクセル幅でも
  途切れず、端点も連続する。**既定**
- `classic` — クアッド + 端点のファン。旧方式

`vector_engine=analytic` を宣言したチェインでのみ使われる。同梱4本のうち
`vector-color` / `vector-monochrome` / `vector-vectrex` の3本が宣言している。
**`default-vector` は宣言していない**ので、このオプションは効かない(素の描画経路)。

### `bgfx_vec_supersample`

| | |
|---|---|
| 型 / 既定 | int (1-2) / `1` |

ベクター FBO のスーパーサンプル倍率。`1` = ウィンドウ解像度、`2` = 2倍。

`analytic` 線描画はすでに解析的なのでエイリアスは出にくいが、細い線が密集する場面で
効く。コストは面積比で 4 倍。

### `bgfx_render_scale`

| | |
|---|---|
| 型 / 既定 | float (0.1-1.0) / `1.0` |

**ベクター内部レンダリングのスケール。** 最終 BGFX 出力・UI・アートワークは
ウィンドウ解像度のまま保たれる。

重いチェインを高解像度で回すときの逃げ道。`0.5` で内部を半分にすると
ベクター描画とグローのコストが約 1/4 になる。線は細くならない
(幅の正規化が解像度に依存しないよう作られている)。

### `bgfx_output_scale`

| | |
|---|---|
| 型 / 既定 | float (0.25-1.0) / `1.0` |

**HDR コンポジットの解像度**をウィンドウに対する比で指定する。最終像はウィンドウへ
アップスケールされる。`bgfx_render_scale` がベクター内部だけを縮めるのに対し、
これはコンポジット全体を縮める。

### `bgfx_hdr`

| | |
|---|---|
| 型 / 既定 | int (0-1) / **`1`** |

HDR10 / EDR 出力を試みる。**利用できない場合は SDR にフォールバックする**ので、
既定でオンにしていても SDR モニタや非対応バックエンドで問題にならない。

`0` で SDR を強制する。

```
BGFX: HDR present path = macOS EDR (Metal, extended-linear RGBA16F)
BGFX: HDR present path = HDR10 (PQ / Rec.2020, RGB10A2)
BGFX: HDR present path = SDR fallback (HDR requested but unavailable)
```

判定は `BGFX_CAPS_HDR10` と、macOS では Metal + EDR ヘッドルームの有無。
**OpenGL / Vulkan バックエンドでは HDR10 の能力が立たないので SDR になる。**

### `bgfx_hdr_paper_white`

| | |
|---|---|
| 型 / 既定 | int / `200`(nits) |

HDR モードでの UI / メニューの白レベル。ゲーム画面ではなく**UI の明るさ**。
HDR ディスプレイで UI が眩しすぎる / 暗すぎるときに調整する。

macOS の EDR では、この値が「基準白」としてヘッドルーム計算の分母になる。

### `bgfx_hdr_display_peak`

| | |
|---|---|
| 型 / 既定 | string / `auto` |
| 値 | `auto` = OS に問い合わせる、数値 = nits、`0` = チェイン既定を使う |

ディスプレイのピーク輝度。ここから **`beam_peak_nits` と `hdr_rolloff_max` の
既定値を導出する**。`bgfx_hdr` がオンのときだけ使われる。

`auto` の検出経路は Windows(DXGI)と macOS(NSScreen EDR)のみ。
**Linux には無い**ので数値指定が必要(ただし Linux では HDR 自体が来ないので実質無関係)。

自動導出された値は**意図的に cfg へ保存されない**。保存してしまうと、次回起動で
自動設定の後に cfg がその数値を復元し、モニタ依存の較正が固定値に化けるため。
1 段でも手で動かせば自動値と一致しなくなるので、通常どおり保存される。

### `bgfx_macos_force_composited`

| | |
|---|---|
| 型 / 既定 | bool / `1` |

macOS のみ。非不透明 Metal レイヤで Core Animation コンポジットを要求する。
EDR ヘッドルームの取得に関わる。`-nobgfx_macos_force_composited` で無効化。

### `bgfx_macos_edr_diagnostics`

| | |
|---|---|
| 型 / 既定 | bool / `0` |

macOS のみ。`CAMetalLayer` の状態と生の EDR ヘッドルームを 1 秒ごとにログへ出す。
HDR が期待どおり出ないときの切り分け用。

---

## 5. 目的別の設定例

### 5-1. 通常プレイ(HDR ディスプレイ、macOS)

`ini/vector.ini`:

```ini
vector_beam_window        1
vector_present_rate       auto
bgfx_hdr                  1
bgfx_hdr_display_peak     auto
bgfx_hdr_paper_white      200
```

### 5-2. 負荷を下げる

まず1語で試す:

```
-vector_quality low
```

`render_scale` / `output_scale` を `0.5` にし、ビーム窓を切る。効き方が足りない / 効きすぎる
ときに `medium`(`0.75` + 窓あり + present 60 Hz 固定)と `high`(`1.0` + 窓あり + present `auto`)を試す。

個別に書くなら:

```ini
vector_beam_window        0
vector_present_rate       0
bgfx_render_scale         0.5
bgfx_output_scale         0.5
bgfx_vec_supersample      1
```

`vector_beam_window` を切ると present ループが止まり、ベクター CPU が 2〜4 分の 1 になる。

どこが律速かは機体によって違うので、どれを下げるかは実測で決める。`-bgfx_debug` を付けると
1 秒ごとに `BGFX PERF` 行がログに出るので、CPU 側と GPU 側のどちらで詰まっているかの当たりがつく。

### 5-3. 比較動画の撮影

```
-vector_playback <name>.mvec -vector_playback_end 1 -novector_playback_overlay \
  -skip_gameinfo -resolution 1920x1080 -sound none -aviwrite out.avi
```

`-skip_gameinfo` は**必須**。起動時の情報画面がキー入力を待つので、付けないと
再生が進まず `-vector_playback_end 1` でも終了しない(ハングに見える)。

`-vector_playback_end 1` も **`-aviwrite` と併用するなら必須**。既定の `0`(最終フレームを保持)
のままだと、ストリーム終端に達したあとも保持したフレームを AVI に書き続け、
**AVI が止まらずに膨張する。** 実際に 139 GB まで育ててディスクを埋めた。
`-seconds_to_run` では止まらない(再生は1記録フレーム=1スクリーン更新で進むため)。

### 5-4. 画面外のビームを見る

```ini
vector_overscan_x         0.95
vector_overscan_y         0.95
vector_blank_leak         0.05
```

### 5-5. クリップ窓の回路（Major Havoc / Battlezone）

揺れ（`jitter`）を出すには `vector_window_sim` が要る。既定値の `1.0` は Major Havoc の
1 辺に合わせた値なので、4 辺すべてが動く Battlezone では下げたほうが落ち着く。

```ini
# starwars.ini ではなく mhavoc.ini / bzone.ini に書く
vector_window_sim         1
vector_window_jitter      1.0      # bzone なら 0.3 前後から
vector_window_droop       2.0
vector_window_memory      0.15
vector_window_bias        -0.2
```

---

## 6. 素の MAME にもあるが VecBeam で意味が変わるもの

### `bgfx_screen_chains`

VecBeam が追加したオプションではないが、挙動が変わっている。

**優先度 105(`source/*.ini`)以上で指定すると cfg のチェイン選択を読まず、
かつ cfg へ書き戻さない。** 素の MAME 0.289 は読まないのに書いていたので、
一度テストで指定するとそのマシンの保存設定が書き換わっていた(修正済み)。

チェイン名は**ファイル名部分だけ**を指定する。`bgfx/chains/vector/vector-color.json` なら
`vector-color`(`vector/vector-color` ではない)。

### `aviwrite`

UI レイヤーを含まない。オーバーレイやメニューは AVI に写らない。
また HDR チェインでも AVI は SDR で記録される。
