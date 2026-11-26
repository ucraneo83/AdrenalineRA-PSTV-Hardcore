#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/out/pstv-release}"
if [[ -z "${VITASDK:-}" || ! -f "$VITASDK/share/vita.toolchain.cmake" ]]; then
  echo "ERROR: VITASDK must point to a working VitaSDK installation" >&2; exit 2
fi
for command in cmake ninja python3; do
  command -v "$command" >/dev/null || { echo "Missing: $command" >&2; exit 2; }
done
python3 "$ROOT/tools/static_guardrails.py"
cmake -S "$ROOT/third_party/Adrenaline" -B "$BUILD_DIR" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$VITASDK/share/vita.toolchain.cmake"
# The upstream README requires the fetched graphics dependencies first.
cmake --build "$BUILD_DIR" --target libvita2dfbo vitashadercollection -j4
cmake --build "$BUILD_DIR" --target Adrenaline.vpk-vpk -j4
VPK="$BUILD_DIR/bubble/Adrenaline.vpk"
test -s "$VPK" || { echo "VPK missing: $VPK" >&2; exit 3; }
python3 "$ROOT/tools/audit_psp_prx.py" "$ROOT/third_party/Adrenaline/user/flash0" --vpk "$VPK"
sha256sum "$VPK" > "$VPK.sha256"
echo "Built: $VPK"
