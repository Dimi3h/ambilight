#!/bin/sh
# Native unit tests (host compiler; no TV needed): sh tests/run.sh
set -e
cd "$(dirname "$0")/.."
out=${TMPDIR:-/tmp}/ambilight-tests
mkdir -p "$out"
for t in tests/test_*.c; do
    n=$(basename "$t" .c)
    cc -O2 -Wall -Wextra -Wno-unused-function -I src -o "$out/$n" "$t" -lm
done
"$out/test_ui_read"
"$out/test_compose"
"$out/test_png" "$out/test.png"
python3 -I tests/check_png.py "$out/test.png"
for f in service/*.sh service/ambilightctl tools/*.sh build.sh; do sh -n "$f"; done
echo "shell syntax ok"
if command -v node > /dev/null 2>&1; then
    node --check service/helper.js
    node --check ui/app.js
    node tests/test_feed.js
else
    echo "node not found: JavaScript checks skipped (they run in CI)"
fi
echo "tests passed"
