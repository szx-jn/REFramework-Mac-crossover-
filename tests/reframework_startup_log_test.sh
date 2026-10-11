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

handle_line=$(grep -nF '[LooseFileLoader][RE9-DIAG] handle_path' "$log" | sed -n '1s/:.*//p')
detector_line=$(grep -nF '[FaultyFileDetector]: Initialized successfully' "$log" | sed -n '1s/:.*//p')

if [ -z "$handle_line" ]; then
    echo "REFramework did not run loose-file path diagnostics: $log" >&2
    exit 1
fi

if [ -z "$detector_line" ]; then
    echo "FaultyFileDetector did not initialize: $log" >&2
    exit 1
fi

if [ "$detector_line" -ge "$handle_line" ]; then
    echo "FaultyFileDetector initialized after loose-file path checks began: $log" >&2
    exit 1
fi

echo "REFramework startup log reached first frame and initialization"
echo "FaultyFileDetector initialized before loose-file path checks"
