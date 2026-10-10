#!/bin/sh
set -eu

log="${1:-/Users/szx/Library/Application Support/CrossOver/Bottles/test/drive_c/RE9/re2_framework_log.txt}"

if [ ! -f "$log" ]; then
    echo "missing REFramework log: $log" >&2
    exit 2
fi

if ! grep -Fq 'Render frame:' "$log"; then
    echo "REFramework did not reach the first rendered frame: $log" >&2
    exit 1
fi

if ! grep -Fq 'REFramework initialized' "$log"; then
    echo "REFramework did not finish initialization: $log" >&2
    exit 1
fi

echo "REFramework startup log reached first frame and initialization"
