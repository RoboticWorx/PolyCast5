#!/usr/bin/env bash
# Host tests for the Screen Mirror encoder, decoder and relay. No hardware, no flashing.
#
#   ./run.sh
#
# Needs gcc (MSYS2 UCRT64 works) and node. The relay test also needs `ws`:
#   npm install ws        (anywhere, then export NODE_PATH=<that>/node_modules)
# It is skipped automatically when ws is not resolvable.

set -u

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"
MIRROR="$REPO/components/mirror"
OUT="${MIRROR_TEST_OUT:-$HERE/.out}"
GCC="${CC:-gcc}"

mkdir -p "$OUT"
fails=0

build() {
  # Project headers are the real ones, never copies; only ESP-IDF headers are stubbed.
  # The feature ships switched off in polycast5_macros.h, and these tests exercise it
  "$GCC" -std=c11 -O2 -Wall -Wextra -Wno-unused-parameter \
    -DPOLYCAST5_EN_SCREEN_MIRROR=1 \
    -I"$HERE/stub" -I"$MIRROR/include" -I"$REPO/components/common/include" \
    "$HERE/$1.c" "$MIRROR/src/mirror_capture.c" "$MIRROR/src/mirror_encode.c" \
    -o "$OUT/$1.exe" || { echo "!! $1 failed to compile"; return 1; }
}

run() {
  echo
  echo "### $1"
  "$OUT/$1.exe" "${@:2}" || fails=$((fails + 1))
}

echo "=== building (warnings are failures worth reading) ==="
for t in test_encode test_regress dump_frames; do
  build "$t" || fails=$((fails + 1))
done

run test_encode
run test_regress

echo
echo "### browser decoder vs. the real firmware encoder"
"$OUT/dump_frames.exe" "$OUT/frames.bin" "$OUT/expect.raw" >/dev/null \
  && node "$HERE/test_decode.js" "$OUT/frames.bin" "$OUT/expect.raw" \
  || fails=$((fails + 1))

echo
echo "### thermal channel: device render vs browser port, then a scripted session"
"$GCC" -std=c11 -O2 -Wall -Wextra -Wno-unused-parameter -DPOLYCAST5_EN_SCREEN_MIRROR=1 \
  -I"$HERE/stub" -I"$MIRROR/include" -I"$REPO/components/common/include" \
  -I"$REPO/components/lcd/src" \
  "$HERE/test_thermal.c" "$MIRROR/src/mirror_capture.c" "$MIRROR/src/mirror_encode.c" \
  "$MIRROR/src/mirror_thermal.c" "$REPO/components/lcd/src/lcd_ir_exp_render.c" -lm \
  -o "$OUT/test_thermal.exe" \
  && "$OUT/test_thermal.exe" "$OUT/thermal_render.bin" "$OUT/thermal_stream.bin" \
  && node "$HERE/test_thermal.js" "$OUT/thermal_render.bin" "$OUT/thermal_stream.bin" \
  || fails=$((fails + 1))

echo
echo "### remote button scheduler"
"$GCC" -std=c11 -O2 -Wall -Wextra -Wno-unused-parameter -DPOLYCAST5_EN_SCREEN_MIRROR=1 \
  -I"$HERE/stub" -I"$REPO/components/gpio/src" -I"$REPO/components/common/include" \
  "$HERE/test_remote_buttons.c" "$REPO/components/gpio/src/gpio_remote.c" \
  -o "$OUT/test_remote_buttons.exe" \
  && "$OUT/test_remote_buttons.exe" \
  || fails=$((fails + 1))

echo
echo "### quality controller and ack round trips, on a simulated link"
"$GCC" -std=c11 -O2 -Wall -Wextra -Wno-unused-parameter -DPOLYCAST5_EN_SCREEN_MIRROR=1 \
  -I"$MIRROR/src" -I"$REPO/components/common/include" \
  "$HERE/test_quality.c" "$MIRROR/src/mirror_quality.c" \
  -o "$OUT/test_quality.exe" \
  && "$OUT/test_quality.exe" \
  || fails=$((fails + 1))

echo
echo "### target compile (real ESP-IDF cross-compiler, -fsyntax-only)"
MIRROR_TEST_OUT="$OUT" python "$HERE/syntax_check.py" || fails=$((fails + 1))

echo
echo "### relay end-to-end"
if node -e "require('ws')" 2>/dev/null; then
  MIRROR_TEST_OUT="$OUT" node "$HERE/test_relay.js" || fails=$((fails + 1))
else
  echo "skipped: 'ws' not resolvable (npm install ws, then set NODE_PATH)"
fi

echo
if [ "$fails" -eq 0 ]; then
  echo "ALL SUITES PASS"
else
  echo "$fails SUITE(S) FAILED"
fi
exit "$fails"
