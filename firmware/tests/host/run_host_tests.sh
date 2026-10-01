#!/bin/sh
# Host tests for the FM transmitter control (no hardware, no ESP-IDF needed). Needs gcc.
set -e
cd "$(dirname "$0")"
out=${TMPDIR:-/tmp}
flags="-std=gnu11 -Wall -Wextra -Wno-unused-function -Imock -I../../main"
gcc $flags -o "$out/test_adf4002" test_adf4002.c -lm
gcc $flags -o "$out/test_fm_sequence" test_fm_sequence.c -lm
gcc $flags -DCONFIG_FM_ALLOW_RF_OUTPUT=1 -o "$out/test_fm_sequence_rf" test_fm_sequence.c -lm
for t in test_adf4002 test_fm_sequence test_fm_sequence_rf; do
    "$out/$t" > "$out/$t.log" || { cat "$out/$t.log"; echo "$t FAILED"; exit 1; }
    echo "$t: $(grep -c '^ok' "$out/$t.log") ok, 0 failed"
done
