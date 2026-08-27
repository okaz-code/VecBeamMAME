# VecBeamMAME

**VecBeamMAME** は、ベクタースキャン式アーケードゲームおよび Vectrex 系表示の**視覚再現を強化**する
ことを目的とした、MAME の非公式・実験的派生版です。実行ファイルは `vbmame`、読み込む設定ファイルは
`vbmame.ini` で、素の MAME とは別物として同じフォルダに共存できます。

VecBeamMAME is an unofficial, experimental fork of MAME aimed at a physically
motivated, HDR-aware simulation of vector-scan displays.  It builds as `vbmame`
and reads `vbmame.ini`, so it can sit alongside a stock MAME.  The documentation
below is currently Japanese only; an English translation is planned.

---

## これは何か

従来のベクター描画は、プリミティブを実質 `X / Y / Z(輝度) / Color` だけで扱います。VecBeamMAME は
ここに**時間情報とビーム関連メタデータ**を加え、実際のベクター CRT が「ビーム移動・ビーム強度・
蛍光体励起・再描画・フォーカス・光学拡散」にどう反応するかを近似します。

単に線を引くのではなく、次のような差を表現することを目指しています。

- 短い線をゆっくり描くと、長い線を高速に描くより明るい
- 蛍光体が消えきる前に同じ場所を再描画すると、単発より明るい（蓄積）
- 高エネルギーのベクターは太く・フォーカスが甘くなる
- 停止したビーム照射（パークドット）や画面外照射の寄与

## 設計の3層モデル

再現を意図的に3層に分けています。何が「エミュレーション（忠実）」で、何が「シミュレーション
（物理近似）」で、何が「経験的調整（見た目合わせ）」かを明確にするためです。

1. **ベクター生成回路のエミュレーション** — ハードウェアが生成するタイミング/信号。AVG/DVG/Vectrex
   のベクタータイミング、開始/終了時刻、描画時間・可視時間・ブランク移動時間、Z/Intensity タイミング、
   Scale 依存の移動時間、既知の Overload 条件など。これらは「元の回路が何を出力したか」を表す情報で、
   忠実側に置きます。
2. **ベクター CRT 表示のシミュレーション** — その信号が実表示装置でどう見えるかを物理原則で近似。
   ビーム速度→エネルギー、蛍光体残光と蓄積、フォーカス/デフォーカス、Overload の見た目、Glow/Halo、
   画面外照射など。
3. **光学レンダリングと経験的調整** — 実機ごとの正確な計測が難しい最終ルックを、写真/動画/キャプチャ
   比較や主観比較で追い込む部分。

> 表示経路全体を「完全なハードウェアエミュレーション」と称するのは避けています。同時に「ただの Bloom
> シェーダ」と矮小化するのも、時間情報とビームメタデータの追加を無視するため不適切です。

## ドキュメント

| | |
|---|---|
| [追加パラメータ一覧](docs/vecbeammame/added-parameters.ja.md) | 実行中に変えるスライダー全 224 本。`bgfx/chains/vector/*.json` から生成 |
| [起動時オプション](docs/vecbeammame/startup-options.ja.md) | `vbmame.ini` に書くもの。ini の作り方から |
| [HDR 設定ガイド](docs/vecbeammame/hdr-settings.ja.md) | HDR 出力の仕組みとモニタに合わせた設定 |
| [ベクターチェイン解説](bgfx/chains/vector/README.md) | 同梱チェインの構成 |

同梱ツール: [MVEC ビューア](tools/mvec-viewer/)（ビーム事象ストリームの可視化）、
[カラーチューナ](tools/vector-color-tuner/)（原色の調整）。どちらもブラウザで開くだけです。

---

## FAQ

### 実機を参考にしていますか？

自宅にある機材で確認して調整しています。

| 対象 | 確認に使った機材 | 主な確認タイトル |
|---|---|---|
| カラーベクター | Amplifone '19 モニタ | ATARI STAR WARS |
| カラーベクター | Wells-Gardner WG6100 モニタ | MAJOR HAVOC |
| モノクロベクター | Vectrex 実機 | — |

**計測器は使っていません。** 目視での比較合わせです。輝度計もオシロも当てていないので、
数値として正しいことは保証できません。写真や動画、自分の記憶との突き合わせで追い込んでいます。

### 実機のベクターモニタにどのくらい迫りますか？

カラーはそこそこいい線まで来た気はしますが、モノクロが全然どうにもなりません。
ピーク輝度が 3,000 nits 出て 360 Hz で画面更新するモニタがあればなんとか？

### 動画で撮影されたベクタースキャンモニタに比べて地味に見えます

カメラで CRT モニタを撮影すると基本的に派手に見えます。盛大な BLOOM が発生して、
輝度の明るい部分は白飛びして見えます。**実機を肉眼で見るとそうはなりません。**

ただし実機に合わせただけでは、CRT の局所的な高輝度がディスプレイ側で再現できないという
別の問題が出ます。そのためある程度は、カメラで撮影された動画や自分の記憶を元に補正しています。

### 画質に満足いかない

モニタの個体差、あるいは OS 側でも個体差が激しいので仕方がないです。
ある程度は自分で調整してください。マクロパラメータから始めるのが早いです（下記）。

### パラメータが多すぎる

大抵は**マクロパラメータ**でどうにかなります。スライダー名が `[M]` で始まるものがそれで、
`[M] Beam Brightness` のように、関連する複数のスライダーをまとめて動かします。
多すぎるとは思いますが、適切に減らすのもまた大変なのです。

→ [追加パラメータ一覧](docs/vecbeammame/added-parameters.ja.md) の冒頭が
「マクロ（Advanced Off でも見える）」の節です。

### どのチェインが使われますか？

同梱チェインは4本で、**カラー機／モノクロ機／Vectrex はそれぞれ対応するチェインを自動で選びます。**

| チェイン | 対象 |
|---|---|
| `vector-color` | カラーベクター機（STAR WARS、MAJOR HAVOC など） |
| `vector-monochrome` | モノクロベクター機 |
| `vector-vectrex` | Vectrex |
| `default-vector` | 最小構成のフォールバック（上記の読み込みに失敗したとき、および未知のベクターハードウェア） |

自分で選ぶなら `-video bgfx -bgfx_screen_chains <チェイン名>`。
旧 `*-balanced` 系の名前は読み込み時に標準名へ移行されるので、**既存の cfg はそのまま使えます。**

### リファレンスにしているモニタは？

MacBook Pro (M5) 内蔵の Liquid Retina XDR ディスプレイです。
外部モニタとして TCL 32R84（DisplayHDR 1400 認証、mini LED HVA パネル）も併用しています。

### 推奨するモニタは？

**DisplayHDR 1000 以上**のモニタです。OLED パネルについては試していないので不明です。

### 画面が暗い

順に確認してください。

1. **モニタの明るさを最大に上げる。** 自動輝度調整が有効になっている場合は切る
2. **HDR が有効になっているか確認する。** 有効にできないと SDR にフォールバックするので暗くなる
3. モニタのピーク輝度に合わせて `bgfx_hdr_display_peak` と `bgfx_hdr_paper_white` を設定する
4. ゲームによって出しているビーム強度が倍半分違います。`Brightness Threshold (T)` が
   「どの強度で最大輝度に達するか」を決めるので、暗いゲームではこれを下げてください

モニタの限界は超えられません。

→ [HDR 設定ガイド](docs/vecbeammame/hdr-settings.ja.md)、
オプションの詳細は [起動時オプション](docs/vecbeammame/startup-options.ja.md)。

### 表示がおかしい

**グラフィックスドライバ側の設定で輝度が自動調整される場合があります。**
該当する機能（コントラスト強調、動的輝度・コントラスト調整など）が有効になっている場合は切ってください。

ベクタースキャンは「黒地に線または点」という極端な画像なので、自動輝度補正には鬼門です。

### 速度が遅い

遅い PC で動かすための各種オプションを用意したので活用してください。まず1語で試せます。

```
-vector_quality low
```

`low` / `medium` / `high` の3段階で、内部解像度・出力解像度・ビーム時間窓をまとめて設定します。

| | 内部解像度 | 出力解像度 | ビーム時間窓 |
|---|---|---|---|
| `high` | 1.0 | 1.0 | オン |
| `medium` | 0.75 | 0.75 | オン |
| `low` | 0.5 | 0.5 | **オフ** |

手持ちで一番遅い Windows PC である **Surface Pro 4**（Intel HD 520）で、
`-vector_quality low` 相当の設定なら STAR WARS のデス・スター爆発でも
41〜42 present/秒・実行速度 100% を維持できています。これより遅いマシンであれば厳しいです。

**測る前に AC アダプタを繋いで電源モードを確認してください。** Surface Pro 4 は
電源と熱で **2.7〜3.2 倍**変動します。「遅い」の原因がここだったことが実際にありました。

なお 60 Hz のパネルでは、ドライバのリフレッシュレートが 60 Hz 前後のゲーム
（GRAVITAR、TEMPEST、BATTLEZONE など）は STAR WARS（41 Hz）の 1.5 倍のフレーム数を要求します。
同じ設定でも収まらない場合は `bgfx_render_scale` を下げてください。

### Mac バイナリが頒布されていないのはなぜ？

色々と事情があります。MAME 公式だって Mac 用バイナリは頒布していません。
コンパイル自体は難しくはありません（下記 [How to compile?](#how-to-compile)）。

### Linux で動きますか？

**動くはずですが未検証です。** 現状、以下は動きません。

- **HDR / EDR 出力** — 実装が Windows と macOS 向けにしかありません（コンパイル時に除外されます）
- **モニタのリフレッシュレート自動取得** — 同じく Windows / macOS のみ。
  そのため `-vector_present_rate auto` は **60 Hz のまま**になります。
  高リフレッシュレートのパネルを使う場合は `-vector_present_rate 144` のように明示指定してください

---

## Upstream MAME

## What is MAME?

MAME is a multi-purpose emulation framework.

MAME's purpose is to preserve decades of software history. As electronic technology continues to rush forward, MAME prevents this important "vintage" software from being lost and forgotten. This is achieved by documenting the hardware and how it functions. The source code to MAME serves as this documentation. The fact that the software is usable serves primarily to validate the accuracy of the documentation (how else can you prove that you have recreated the hardware faithfully?). Over time, MAME (originally stood for Multiple Arcade Machine Emulator) absorbed the sister-project MESS (Multi Emulator Super System), so MAME now documents a wide variety of (mostly vintage) computers, video game consoles and calculators, in addition to the arcade video games that were its initial focus.

## Where can I find out more?

* [Official MAME Development Team Site](https://www.mamedev.org/) (includes binary downloads, wiki, forums, and more)
* [MAME Testers](https://mametesters.org/) (official bug tracker for MAME)

### Community

* [MAME Forums on bannister.org](https://forums.bannister.org/ubbthreads.php?ubb=cfrm&c=5)
* [r/MAME](https://www.reddit.com/r/MAME/) on Reddit
* [MAMEWorld Forums](https://www.mameworld.info/ubbthreads/)

## Development

![Alt](https://repobeats.axiom.co/api/embed/8461d8ae4630322dafc736fc25782de214b49630.svg "Repobeats analytics image")

### CI status and code scanning

[![CI (Linux)](https://github.com/mamedev/mame/workflows/CI%20(Linux)/badge.svg)](https://github.com/mamedev/mame/actions/workflows/ci-linux.yml) [![CI (Windows](https://github.com/mamedev/mame/workflows/CI%20(Windows)/badge.svg)](https://github.com/mamedev/mame/actions/workflows/ci-windows.yml) [![CI (macOS)](https://github.com/mamedev/mame/workflows/CI%20(macOS)/badge.svg)](https://github.com/mamedev/mame/actions/workflows/ci-macos.yml) [![Compile UI translations](https://github.com/mamedev/mame/workflows/Compile%20UI%20translations/badge.svg)](https://github.com/mamedev/mame/actions/workflows/language.yml) [![Build documentation](https://github.com/mamedev/mame/workflows/Build%20documentation/badge.svg)](https://github.com/mamedev/mame/actions/workflows/docs.yml)  [![Coverity Scan Status](https://scan.coverity.com/projects/5727/badge.svg?flat=1)](https://scan.coverity.com/projects/mame-emulator)

### How to compile?

If you're on a UNIX-like system (including Linux and macOS), it could be as easy as typing

```
make
```

for a full build,

```
make SUBTARGET=tiny
```

for a build including a small subset of supported systems.

See the [Compiling MAME](http://docs.mamedev.org/initialsetup/compilingmame.html) page on our documentation site for more information, including prerequisites for macOS and popular Linux distributions.

For recent versions of macOS you need to install [Xcode](https://developer.apple.com/xcode/) including command-line tools and [SDL 2.0](https://github.com/libsdl-org/SDL/releases/latest).

For Windows users, we provide a ready-made [build environment](http://www.mamedev.org/tools/) based on MinGW-w64.

Visual Studio builds are also possible, but you still need [build environment](http://www.mamedev.org/tools/) based on MinGW-w64.
In order to generate solution and project files just run:

```
make vs2022
```
or use this command to build it directly using msbuild

```
make vs2022 MSBUILD=1
```

### Coding standard

MAME source code should be viewed and edited with your editor set to use four spaces per tab. Tabs are used for initial indentation of lines, with one tab used per indentation level. Spaces are used for other alignment within a line.

Some parts of the code follow [Allman style](https://en.wikipedia.org/wiki/Indent_style#Allman_style); some parts of the code follow [K&R style](https://en.wikipedia.org/wiki/Indent_style#K.26R_style) -- mostly depending on who wrote the original version. **Above all else, be consistent with what you modify, and keep whitespace changes to a minimum when modifying existing source.** For new code, the majority tends to prefer Allman style, so if you don't care much, use that.

All contributors need to either add a standard header for license info (on new files) or inform us of their wishes regarding which of the following licenses they would like their code to be made available under: the [BSD-3-Clause](http://opensource.org/licenses/BSD-3-Clause) license, the [LGPL-2.1](http://opensource.org/licenses/LGPL-2.1), or the [GPL-2.0](http://opensource.org/licenses/GPL-2.0).

See more specific [C++ Coding Guidelines](https://docs.mamedev.org/contributing/cxx.html) on our documentation web site.

## License

VecBeamMAME is a fork of MAME and is distributed under the same terms.  The files
added or modified by this fork carry their own SPDX headers; the new ones are
made available under the
[3-clause BSD License](http://opensource.org/licenses/BSD-3-Clause), the same
license the great majority of MAME uses.  VecBeamMAME is not an official MAME
release and is not endorsed by the MAME project; please do not report issues
with it to MAMEdev.

The MAME project as a whole is made available under the terms of the
[GNU General Public License, version 2](http://opensource.org/licenses/GPL-2.0)
or later (GPL-2.0+), since it contains code made available under multiple
GPL-compatible licenses.  A great majority of the source files (over 90%
including core files) are made available under the terms of the
[3-clause BSD License](http://opensource.org/licenses/BSD-3-Clause), and we
would encourage new contributors to make their contributions available under the
terms of this license.

Please note that MAME is a registered trademark of Gregory Ember, and permission
is required to use the "MAME" name, logo, or wordmark.

<a href="http://opensource.org/licenses/GPL-2.0" target="_blank">
<img align="right" width="100" src="https://opensource.org/wp-content/uploads/2009/06/OSIApproved.svg">
</a>

    Copyright (c) 1997-2026  MAMEdev and contributors

    This program is free software; you can redistribute it and/or modify it
    under the terms of the GNU General Public License version 2, as provided in
    docs/legal/GPL-2.0.

    This program is distributed in the hope that it will be useful, but WITHOUT
    ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
    FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
    more details.

Please see [COPYING](COPYING) for more details.
