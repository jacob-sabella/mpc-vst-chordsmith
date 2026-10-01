#!/usr/bin/env bash
# Build Chordsmith as an MPC OS VST2 instrument with mpc-vst-plugins' generic port builder (vst.json).
#   vst/build/chordsmith.so, vst/build/skin/<folder>/, vst/build/pluginlist-entry.xml
# Needs Docker (QEMU for arm32v7) and an mpc-vst-plugins checkout (MPC_VST, default: next to this repo).
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
MPC_VST="${MPC_VST:-$here/../../mpc-vst-plugins}"
[ -x "$MPC_VST/tools/build_port.sh" ] || { echo "need an mpc-vst-plugins checkout (MPC_VST)" >&2; exit 1; }
mkdir -p "$here/build"
cp "$MPC_VST/wrapper/engine.h" "$here/build/"   # the engine interface; build/ is on the include path
exec "$MPC_VST/tools/build_port.sh" "$here/vst.json"
