#!/usr/bin/env bash
# Regenerates compile_commands.json for the C/C++ IntelliSense provider.
# Run after changing platformio.ini (build flags, lib_deps, envs).
#
# PlatformIO emits one database per env and always writes it to the project
# root, so each env is generated in turn and the results are merged.
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$PROJECT_DIR"

ENVS=$(python3 -c "
import configparser
c = configparser.ConfigParser()
c.read('platformio.ini')
print(' '.join(s.split(':', 1)[1] for s in c.sections() if s.startswith('env:')))
")

TMPDIR_DB=$(mktemp -d)
trap 'rm -rf "$TMPDIR_DB"' EXIT

for env in $ENVS; do
    pio run -t compiledb -e "$env" >/dev/null
    mv compile_commands.json "$TMPDIR_DB/$env.json"
done

python3 - "$TMPDIR_DB" <<'PY'
import json, pathlib, sys

out, seen = [], set()
for path in sorted(pathlib.Path(sys.argv[1]).glob('*.json')):
    for entry in json.loads(path.read_text()):
        if entry['file'] in seen:
            continue
        seen.add(entry['file'])
        out.append(entry)

pathlib.Path('compile_commands.json').write_text(json.dumps(out, indent=1))
print(f'compile_commands.json: {len(out)} entries')
PY
