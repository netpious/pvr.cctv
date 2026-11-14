#!/usr/bin/env bash
set -euo pipefail

BUILD_DIR="${BUILD_DIR:-build}"
KODI_INCLUDE_DIR="${KODI_INCLUDE_DIR:-}"

if [[ -z "${KODI_INCLUDE_DIR}" ]]; then
  for candidate in \
    "$(dirname "$0")/kodi-dev-kit/include" \
    "$(dirname "$0")/../kodi-dev-kit/include"; do
    if [[ -f "${candidate}/kodi/addon-instance/PVR.h" ]]; then
      KODI_INCLUDE_DIR="${candidate}"
      break
    fi
  done
fi

if [[ -z "${KODI_INCLUDE_DIR}" || ! -f "${KODI_INCLUDE_DIR}/kodi/addon-instance/PVR.h" ]]; then
  cat >&2 <<'EOF'
Kodi development headers were not found.

Download the Kodi 21 (Omega) source and use:
  xbmc/addons/kodi-dev-kit/include

Then run:
  KODI_INCLUDE_DIR=/path/to/kodi-dev-kit/include ./build.sh
EOF
  exit 1
fi

cmake -S "$(dirname "$0")" -B "${BUILD_DIR}" \
  -DCMAKE_BUILD_TYPE=Release \
  -DKODI_INCLUDE_DIR="${KODI_INCLUDE_DIR}"

cmake --build "${BUILD_DIR}" -j "$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"

echo
echo "Built: ${BUILD_DIR}/addon/pvr.cctv.(so|dylib)"
