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
#   ./prof_flight.sh [--build] [warmup=55] [interval=0.4] [count=300]
#     --build   rebuild out/RoF.exe with PROBES=1 PROFILE_NORING=1 NO_TDRAW_PROF=1 first
#               (PROBES = headless auto-launch; PROFILE_NORING = drop the RF_RING
#                rescue-debug ring whose rfPlaneSum scans otherwise eat ~half the
#                samples; NO_TDRAW_PROF = drop the terrain-draw sub-phase brackets,
#                which put ~6 live-beam chip reads per visible pair ON the object
#                loop's own source lines).  Skip it if out/RoF.exe is already such a
#                build — but note a plain `PROBES=1` build is NOT (it inflates the
#                object-loop bucket).
#
# ⚠ The warmup must actually reach FLIGHT: the boot -> Logo/Station/Standby -> doors ->
# tunnel -> planet cinematic runs ~45 s of wall clock under the gdb stub before the
# flight VBI ($4FF5) takes over, and prof_flight.py keeps ONLY $4FF5 samples.  A 20 s
# warmup with a short count profiles the launch cinematic and reports "0 samples".
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
WARMUP="${1:-55}"; INTERVAL="${2:-0.4}"; COUNT="${3:-300}"

if ! command -v fs-uae >/dev/null 2>&1 || ! command -v m68k-amiga-elf-gdb >/dev/null 2>&1; then
  echo "fs-uae / m68k-amiga-elf-gdb not on PATH — run '. ./env.sh' in this shell first." >&2
  exit 1
fi

if [ "$BUILD" = 1 ]; then
  echo "=== building out/RoF.exe (PROBES=1 PROFILE_NORING=1 NO_TDRAW_PROF=1) ==="
  make clean >/dev/null && make -j4 PROBES=1 PROFILE_NORING=1 NO_TDRAW_PROF=1 \
    || { echo "build failed" >&2; exit 1; }
fi

echo "=== sampling: warmup ${WARMUP}s, ${COUNT} samples @ ${INTERVAL}s ==="
./diag_sample.sh "$WARMUP" "$INTERVAL" "$COUNT" >/dev/null 2>&1 || true

echo
python3 prof_flight.py
