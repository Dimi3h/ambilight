#!/bin/sh
# Native unit tests (host compiler; no TV needed): sh tests/run.sh
set -e
cd "$(dirname "$0")/.."
out=${TMPDIR:-/tmp}/ambilight-tests
mkdir -p "$out"
for t in tests/test_*.c; do
    n=$(basename "$t" .c)
    cc -O2 -Wall -Wextra -Wno-unused-function -I src -o "$out/$n" "$t" -lm
    "$out/$n"
done
if command -v node > /dev/null 2>&1; then
    node --check service/helper.js
    node --check ui/app.js
else
    echo "node not found: JavaScript syntax check skipped"
fi
echo "tests passed"
