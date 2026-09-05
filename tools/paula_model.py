#!/usr/bin/env python3
"""Decode the AUDIO_TRACE capture (amiga/audio_state.gdb -> /tmp/as_*.bin).

Per frame, per channel: Paula period, loop length and volume, as the ISR last wrote them.
Loop length is the waveform identity:
    1 = wave_pure   15 = poly4   31 = poly5   465/511 = poly_dist   4096 = noise_buf

Volume matters because only AUDxVOL 64 gives a plain sample-and-hold.  At 1..63 Paula runs a
PWM cycle on a fixed 64-tick raster (3546895/64 = 55420 Hz) that re-quantises every waveform
edge, so `period % 64 != 0` turns into periodic edge jitter -> in-band sidebands.
"""
import struct, sys, os

CCK = 3546895.0                      # PAL colour clock (Hz)
RASTER = CCK / 64.0                  # 55420 Hz volume-PWM raster
WAVE = {0: "-", 1: "pure", 15: "poly4", 31: "poly5", 465: "pdist", 511: "pdist", 4096: "noise"}

def load(path, fmt, count):
    with open(path, "rb") as f:
        raw = f.read()
    n = struct.calcsize(fmt)
    return [struct.unpack_from(fmt, raw, i * n)[0] for i in range(min(count, len(raw) // n))]

def main():
    d = sys.argv[1] if len(sys.argv) > 1 else "/tmp"
    N = 600
    idx = load(f"{d}/as_idx.bin", ">H", 1)[0]
    vbi = load(f"{d}/as_vbi.bin", ">H", N)
    per = load(f"{d}/as_per.bin", ">H", N * 4)
    ln  = load(f"{d}/as_len.bin", ">H", N * 4)
    vol = load(f"{d}/as_vol.bin", ">B", N * 4)
    print(f"frames captured: {idx}/{N}\n")
    hdr = "  vbi | " + " | ".join(f"ch{c}: per  wave  vol  Hz    rast" for c in range(4))
    print(hdr); print("-" * len(hdr))
    prev = None
    for i in range(idx):
        row = []
        for c in range(4):
            p, L, v = per[i*4+c], ln[i*4+c], vol[i*4+c]
            # CCK/p is the DMA SAMPLE rate, not the pitch: a loop of L words holds 2*L samples,
            # so the tone is one full cycle per loop.  For wave_pure (L=1, a 2-sample square)
            # printing CCK/p reads a full octave high.
            hz = CCK / (p * 2 * L) if (p and L) else 0.0
            # ' ' = exact on the raster (no edge jitter); '!' = edges get snapped
            rast = "." if v >= 64 or v == 0 else ("ok" if p % 64 == 0 else "JIT")
            row.append(f"{p:5d} {WAVE.get(L,str(L)):>5} {v:4d} {hz:7.0f} {rast:>3}")
        line = f"{vbi[i]:5d} | " + " | ".join(row)
        if line[6:] != (prev or ""):     # collapse runs of identical state
            print(line)
        prev = line[6:]

    # Summary: which channels ever ran below full scale (i.e. rastered)?
    print("\nper-channel volume range over the capture:")
    for c in range(4):
        vs = [vol[i*4+c] for i in range(idx)]
        act = [v for v in vs if v]
        if not act:
            print(f"  ch{c}: silent"); continue
        sub = sum(1 for v in act if v < 64)
        print(f"  ch{c}: min={min(act):2d} max={max(act):2d}  "
              f"frames active={len(act):3d}  of those SUB-64 (rastered)={sub:3d} "
              f"({100.0*sub/len(act):.0f}%)")

if __name__ == "__main__":
    main()
