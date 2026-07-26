#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# Flight per-function PC-profiler — one-shot recipe (build → sample → analyze).
#
# Statistical PC sampling of the main thread in steady flight: reach flight
# headlessly (auto-launch), fire SIGINTs, resolve each $pc to a function, and
# bucket by logical group.  Tells you where the flight loop spends its time and
# — via prof_flight.py's native?/asm? columns — which hot functions are still
# raw transliteration (rof_gen.c) vs clean native twins vs hand-asm.
#
# Usage:
#   . ./env.sh                       # fs-uae + m68k-amiga-elf-gdb on PATH (REQUIRED)
#   ./prof_flight.sh [--build] [warmup=20] [interval=0.3] [count=250]
#     --build   rebuild out/RoF.exe with PROBES=1 PROFILE_NORING=1 first
#               (PROBES = headless auto-launch; PROFILE_NORING = drop the RF_RING
#                rescue-debug ring whose rfPlaneSum scans otherwise eat ~half the
#                samples).  Skip it if out/RoF.exe is already such a build.
#
# After it prints the table:
#   python3 prof_flight.py --by-symbol            # raw per-symbol breakdown
#   python3 prof_flight.py --drill "FUNC"         # resolve a bucket's hot PCs to src lines
#       (C++ names need the full signature, e.g. "RescueOnFractalus::renderFlightDirect()")
# ---------------------------------------------------------------------------
set -uo pipefail
cd "$(dirname "$0")"

BUILD=0
if [ "${1:-}" = "--build" ]; then BUILD=1; shift; fi
WARMUP="${1:-20}"; INTERVAL="${2:-0.3}"; COUNT="${3:-250}"

if ! command -v fs-uae >/dev/null 2>&1 || ! command -v m68k-amiga-elf-gdb >/dev/null 2>&1; then
  echo "fs-uae / m68k-amiga-elf-gdb not on PATH — run '. ./env.sh' in this shell first." >&2
  exit 1
fi

if [ "$BUILD" = 1 ]; then
  echo "=== building out/RoF.exe (PROBES=1 PROFILE_NORING=1) ==="
  make clean >/dev/null && make -j4 PROBES=1 PROFILE_NORING=1 || { echo "build failed" >&2; exit 1; }
fi

echo "=== sampling: warmup ${WARMUP}s, ${COUNT} samples @ ${INTERVAL}s ==="
./diag_sample.sh "$WARMUP" "$INTERVAL" "$COUNT" >/dev/null 2>&1 || true

echo
python3 prof_flight.py
