#!/usr/bin/env python3
"""Generate the added-parameters document from the vector chain JSONs.

The chain files are the only place a slider's name, label, default, range, step, macro wiring and
Advanced flag exist, so the table is generated from them rather than transcribed. A hand-written
list went stale twice; this one cannot.

Section order and membership come from tools/chain-slider-sections.json, and the one-line
description of each slider from tools/chain-slider-notes.<lang>.json. A slider that is not listed in
the section map lands in a trailing "unclassified" section, which is how a newly added slider
announces itself - move it into the map to place it, and give it a line in both notes files.

    python3 tools/dump-chain-sliders.py --lang ja > docs/vecbeammame/added-parameters.ja.md
    python3 tools/dump-chain-sliders.py --lang en > docs/vecbeammame/added-parameters.md
"""

import argparse
import json
import os
import sys

CHAINS = (("vector-color.json", "color"),
          ("vector-monochrome.json", "mono"),
          ("vector-vectrex.json", "vectrex"))

MODE_JA = {"scale": "倍率", "curve": "折れ線", "enable": "0/1"}
MODE_EN = {"scale": "scale", "curve": "curve", "enable": "on/off"}


def read_chain(path):
    """Parse a chain file. MAME's reader allows // comments, which json does not."""
    with open(path, encoding="utf-8") as handle:
        text = handle.read()
    stripped = "\n".join("" if line.lstrip().startswith("//") else line
                         for line in text.split("\n"))
    return json.loads(stripped)


def fmt_number(value):
    """0.5 not 0.5000, 160 not 160.0 - the chain files mix ints and floats freely."""
    if isinstance(value, bool):
        return str(value)
    if isinstance(value, float) and value == int(value) and abs(value) < 1e9:
        return str(int(value))
    return str(value)


def fmt_range(slider):
    lo, hi, step = slider.get("min"), slider.get("max"), slider.get("step")
    text = f"{fmt_number(slider['default'])}"
    if lo is not None and hi is not None:
        text += f" [{fmt_number(lo)}, {fmt_number(hi)}]"
    if step is not None:
        text += f" /{fmt_number(step)}"
    return text


def value_column(per_chain):
    """One line if every chain that has the slider agrees, otherwise one line per chain."""
    texts = {label: fmt_range(slider) for label, slider in per_chain.items()}
    if len(set(texts.values())) == 1:
        return next(iter(texts.values()))
    return "<br>".join(f"{label}: {text}" for label, text in texts.items())


def macro_column(name, macros, ja=True):
    """Which macros drive this slider, and how. Empty for a slider nothing drives."""
    parts = []
    for macro_name, mode in sorted(macros.get(name, [])):
        label = MODE_JA.get(mode, mode) if ja else MODE_EN.get(mode, mode)
        parts.append(f"{label}: {macro_name}")
    return "<br>".join(parts) if parts else "—"


def collect(chain_dir):
    sliders = {}          # name -> {chain label -> slider dict}
    macros = {}           # target name -> {(macro name, mode)}
    for filename, label in CHAINS:
        path = os.path.join(chain_dir, filename)
        if not os.path.exists(path):
            print(f"warning: {path} not found, skipping", file=sys.stderr)
            continue
        for slider in read_chain(path).get("sliders", []):
            sliders.setdefault(slider["name"], {})[label] = slider
            for target in slider.get("targets", []):
                macros.setdefault(target["slider"], set()).add(
                    (slider.get("text", slider["name"]), target.get("mode", "?")))
    return sliders, macros


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    root = os.path.dirname(here)
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--chains", default=os.path.join(root, "bgfx", "chains", "vector"),
                        help="directory holding the vector chain JSON files")
    parser.add_argument("--sections", default=os.path.join(here, "chain-slider-sections.json"),
                        help="section order and membership")
    parser.add_argument("--lang", choices=("ja", "en"), default="ja",
                        help="which language to emit")
    parser.add_argument("--notes", default=None,
                        help="one-line description per slider (default: chain-slider-notes.<lang>.json)")
    args = parser.parse_args()

    ja = args.lang == "ja"
    notes_path = args.notes or os.path.join(here, f"chain-slider-notes.{args.lang}.json")
    sliders, macros = collect(args.chains)
    with open(args.sections, encoding="utf-8") as handle:
        sections = json.load(handle)["sections"]
    with open(notes_path, encoding="utf-8") as handle:
        notes = json.load(handle)["notes"]

    placed = set()
    out = []
    if ja:
        out += [
            "# 追加パラメータ一覧",
            "",
            "English: [added-parameters.md](added-parameters.md)",
            "",
            "**このファイルは `tools/dump-chain-sliders.py` が "
            "`bgfx/chains/vector/*.json` から生成しています。手で編集しないでください。**",
            "",
            "スライダーは MAME のスライダーメニュー（既定では `Tab` → Sliders）から操作し、"
            "変更した値はゲームごとに `cfg/<game>.cfg` に保存されます。",
            "**[M] が付くものはマクロ**で、複数のパラメータをまとめて動かします。"
            "マクロ以外は `Advanced Parameters` を On にすると出てきます"
            "（`Brightness Threshold (T)` だけは例外で、常に表示されます）。",
            "",
            "起動時オプション（`-vector_*` など、ini ファイルに書くもの）は "
            "[startup-options.ja.md](startup-options.ja.md) を参照してください。",
            "",
            "凡例: 値の列は `既定値 [下限, 上限] /刻み`。"
            "チェインによって違う場合はチェインごとに並べています。説明は 1 行の要約です。",
            "",
        ]
    else:
        out += [
            "# Added parameters",
            "",
            "日本語: [added-parameters.ja.md](added-parameters.ja.md)",
            "",
            "**This file is generated by `tools/dump-chain-sliders.py` from "
            "`bgfx/chains/vector/*.json`.  Do not edit it by hand.**",
            "",
            "The sliders are reached from MAME's slider menu (`Tab` then Sliders by default), and a "
            "value you change is saved per game in `cfg/<game>.cfg`.",
            "**Anything prefixed `[M]` is a macro** and moves several parameters together.  "
            "Everything else appears once `Advanced Parameters` is On - with one exception, "
            "`Brightness Threshold (T)`, which is always shown.",
            "",
            "Startup options (`-vector_*` and the rest, the ones that go in the ini file) are in "
            "[startup-options.md](startup-options.md).",
            "",
            "Legend: the value column is `default [min, max] /step`.  Where the chains differ, each "
            "is listed.  The description is a one-line summary.",
            "",
        ]

    def emit(title, names):
        rows = [n for n in names if n in sliders]
        if not rows:
            return
        out.append(f"## {title}")
        out.append("")
        out.append("| 内部名 | 表示名 | 説明 | 値 | チェイン | 駆動マクロ |" if ja else
                   "| Name | Label | Description | Value | Chains | Driven by |")
        out.append("|---|---|---|---|---|---|")
        for name in rows:
            per_chain = sliders[name]
            text = next(iter(per_chain.values())).get("text", name)
            out.append(f"| `{name}` | {text} | {notes.get(name, '—')} | {value_column(per_chain)} "
                       f"| {'/'.join(per_chain)} | {macro_column(name, macros, ja)} |")
            placed.add(name)
        out.append("")

    for section in sections:
        emit(section["title"] if ja else section.get("title_en", section["title"]),
             section["sliders"])

    leftover = sorted(set(sliders) - placed)
    if leftover:
        emit("未分類（`tools/chain-slider-sections.json` に未登録）" if ja else
             "Unclassified (not listed in `tools/chain-slider-sections.json`)", leftover)

    print("\n".join(out))


if __name__ == "__main__":
    main()
