#!/usr/bin/env bash
# Offline tests (x86, ASan/UBSan), before anything goes to a device:
#   1. tests/theory_test.c: chord theory and the engine end to end through a capture sink
#   2. mpc-vst-plugins tools/test_port.sh: the generic wrapper's host test (instances, params, chunk)
# Needs an mpc-vst-plugins checkout (MPC_VST, default: next to this repo).
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
MPC_VST="${MPC_VST:-$here/../../mpc-vst-plugins}"
[ -x "$MPC_VST/tools/test_port.sh" ] || { echo "need an mpc-vst-plugins checkout (MPC_VST)" >&2; exit 1; }
mkdir -p "$here/build"
cp "$MPC_VST/wrapper/engine.h" "$here/build/"
cd "$here/.."
ALSA=""
[ -f /usr/include/alsa/asoundlib.h ] && ALSA="-DHAVE_ALSA"   # checks seq_out.c's hand-written ALSA ABI
gcc -std=gnu11 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer -g -O1 $ALSA \
    -Isrc -Ivst/build tests/theory_test.c src/chordsmith.c src/theory.c src/seq_out.c src/synth.c \
    -lm -ldl -lpthread -o vst/build/theory_test
vst/build/theory_test
"$MPC_VST/tools/test_port.sh" "$here/vst.json"
