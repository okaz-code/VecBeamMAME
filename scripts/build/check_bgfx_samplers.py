#!/usr/bin/python3
##
## license:BSD-3-Clause
## copyright-holders:VecBeamMAME
##
## Check that every BGFX chain binds its texture inputs at the positions the
## fragment shaders declare.
##
## SAMPLER2D(name, unit) in a .sc becomes [[texture(unit)]] in the Metal
## translation, but a chain binds its input list positionally
## (bgfx::setTexture(list_index, ...)). When a sampler's list position differs
## from its declared unit, the Metal build silently reads the wrong texture --
## GL and D3D hide it behind a sampler uniform, so the same chain looks correct
## everywhere else. Nothing in the JSON says which unit a name wants, so the
## mismatch survives review; this walks the pair and says so.
##
## Usage: scripts/build/check_bgfx_samplers.py [mame-root]

import glob
import json
import os
import os.path
import re
import sys


def load_json(path):
    # The chain and effect files carry // comments and are sometimes BOM-marked.
    with open(path, encoding='utf-8-sig') as f:
        return json.loads(re.sub(r'//.*', '', f.read()))


def declared_units(shaders, fragment):
    path = os.path.join(shaders, fragment + '.sc')
    if not os.path.isfile(path):
        return None
    with open(path) as f:
        return {m.group(1): int(m.group(2)) for m in
                re.finditer(r'^\s*SAMPLER\w*\s*\(\s*(\w+)\s*,\s*(\d+)\s*\)', f.read(), re.M)}


if __name__ == '__main__':
    root = sys.argv[1] if len(sys.argv) > 1 else '.'
    shaders = os.path.join(root, 'src/osd/modules/render/bgfx/shaders')
    effects = os.path.join(root, 'bgfx/effects')
    chains = os.path.join(root, 'bgfx/chains')

    # effect name as a chain writes it ("vector/vector_phosphor") -> {sampler: unit}
    units = { }
    for path in glob.glob(os.path.join(effects, '**', '*.json'), recursive=True):
        try:
            u = declared_units(shaders, load_json(path).get('fragment', ''))
        except (OSError, ValueError):
            continue
        if u is not None:
            units[os.path.relpath(path, effects)[:-5]] = u

    errors = warnings = passes = 0
    for path in sorted(glob.glob(os.path.join(chains, '**', '*.json'), recursive=True)):
        try:
            chain = load_json(path)
        except (OSError, ValueError):
            continue
        name = os.path.relpath(path, root)
        for chain_pass in chain.get('passes', [ ]):
            effect = chain_pass.get('effect', '')
            declared = units.get(effect)
            if declared is None:
                continue
            passes += 1
            bound = set()
            for index, entry in enumerate(chain_pass.get('input', [ ])):
                sampler = entry.get('sampler')
                bound.add(sampler)
                if sampler not in declared:
                    sys.stderr.write('%s: %s: %s is bound but the shader does not declare it\n'
                            % (name, effect, sampler))
                    errors += 1
                elif declared[sampler] != index:
                    sys.stderr.write('%s: %s: %s sits at list index %d but is declared as unit %d\n'
                            % (name, effect, sampler, index, declared[sampler]))
                    errors += 1
            for sampler, unit in sorted(declared.items(), key=lambda kv: kv[1]):
                if sampler not in bound:
                    # Not wrong by itself: the read may sit behind a uniform this chain pins to
                    # zero. It is undefined if the branch can run, so each one wants an eye.
                    sys.stderr.write('%s: %s: %s (unit %d) is declared but not bound\n'
                            % (name, effect, sampler, unit))
                    warnings += 1

    sys.stderr.write('%d pass(es) checked, %d error(s), %d warning(s)\n' % (passes, errors, warnings))
    sys.exit(1 if errors else 0)
