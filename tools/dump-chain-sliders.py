#!/usr/bin/env python3
"""Generate the added-parameters document from the vector chain JSONs.

The chain files are the only place a slider's name, label, default, range, step, macro wiring and
Advanced flag exist, so the table is generated from them rather than transcribed. A hand-written
list went stale twice; this one cannot.

Section order and membership come from tools/chain-slider-sections.json. A slider that is not
listed there lands in a trailing "unclassified" section, which is how a newly added slider
announces itself - move it into the map to place it.

    python3 tools/dump-chain-sliders.py > docs/vecbeammame/added-parameters.ja.md
"""

import argparse
import json
import os
import sys

CHAINS = (("vector-color.json", "color"),
          ("vector-monochrome.json", "mono"),
          ("vector-vectrex.json", "vectrex"))

MODE_JA = {"scale": "倍率", "curve": "折れ線", "enable": "0/1"}


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


def macro_column(name, macros):
    """Which macros drive this slider, and how. Empty for a slider nothing drives."""
    parts = []
    for macro_name, mode in sorted(macros.get(name, [])):
        parts.append(f"{MODE_JA.get(mode, mode)}: {macro_name}")
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
    args = parser.parse_args()

    sliders, macros = collect(args.chains)
    with open(args.sections, encoding="utf-8") as handle:
        sections = json.load(handle)["sections"]

    placed = set()
    out = []
    out.append("# 追加パラメータ一覧")
    out.append("")
    out.append("**このファイルは `tools/dump-chain-sliders.py` が "
               "`bgfx/chains/vector/*.json` から生成しています。手で編集しないでください。**")
    out.append("")
    out.append("スライダーは MAME のスライダーメニュー（既定では `Tab` → Sliders）から操作し、"
               "変更した値はゲームごとに `cfg/<game>.cfg` に保存されます。")
    out.append("**[M] が付くものはマクロ**で、複数のパラメータをまとめて動かします。"
               "マクロ以外は `Advanced Parameters` を On にすると出てきます"
               "（`Brightness Threshold (T)` だけは例外で、常に表示されます）。")
    out.append("")
    out.append("起動時オプション（`-vector_*` など、ini ファイルに書くもの）は "
               "[startup-options.ja.md](startup-options.ja.md) を参照してください。")
    out.append("")
    out.append("凡例: 値の列は `既定値 [下限, 上限] /刻み`。"
               "チェインによって違う場合はチェインごとに並べています。")
    out.append("")

    def emit(title, names):
        rows = [n for n in names if n in sliders]
        if not rows:
            return
        out.append(f"## {title}")
        out.append("")
        out.append("| 内部名 | 表示名 | 値 | チェイン | 駆動マクロ |")
        out.append("|---|---|---|---|---|")
        for name in rows:
            per_chain = sliders[name]
            text = next(iter(per_chain.values())).get("text", name)
            out.append(f"| `{name}` | {text} | {value_column(per_chain)} "
                       f"| {'/'.join(per_chain)} | {macro_column(name, macros)} |")
            placed.add(name)
        out.append("")

    for section in sections:
        emit(section["title"], section["sliders"])

    leftover = sorted(set(sliders) - placed)
    if leftover:
        emit("未分類（`tools/chain-slider-sections.json` に未登録）", leftover)

    print("\n".join(out))


if __name__ == "__main__":
    main()
