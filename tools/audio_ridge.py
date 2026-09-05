#!/usr/bin/env python3
"""Find and track tonal ridges in a raw PCM dump of the emulator's audio output.

Capture the stream with SDL2's disk audio driver (no sound card involved, so the file is exactly
what FS-UAE produced):
    SDL_AUDIODRIVER=disk SDL_DISKAUDIOFILE=/tmp/rof.raw ... ./run.sh
Format is whatever FS-UAE opened the device with — 44100 Hz, S16LSB, 2 channels (see
~/.local/share/fs-uae/fs-uae.log, "[AUD] Opened audio device").

Usage: audio_ridge.py FILE.raw [t0 t1]
Prints, per time slice, the strongest partials, and flags any ridge whose frequency FALLS while
the engine spools — the whine is reported as descending, so a descending ridge is the target.
"""
import sys
import numpy as np

SR = 44100

def main():
    path = sys.argv[1]
    t0 = float(sys.argv[2]) if len(sys.argv) > 3 else 0.0
    t1 = float(sys.argv[3]) if len(sys.argv) > 3 else 1e9

    x = np.fromfile(path, dtype="<i2").astype(np.float32)
    x = x.reshape(-1, 2).mean(axis=1)          # mono sum; the artefact is on both
    dur = len(x) / SR
    print(f"{path}: {len(x)} frames = {dur:.2f} s")
    a, b = int(t0 * SR), min(len(x), int(t1 * SR))
    x = x[a:b]
    if len(x) < 4096:
        print("too short"); return

    N, HOP = 8192, 4096                         # ~5.4 Hz bins, ~93 ms slices
    win = np.hanning(N)
    freqs = np.fft.rfftfreq(N, 1.0 / SR)
    nsl = (len(x) - N) // HOP + 1
    print(f"\n{'t(s)':>6} | strongest partials below 12 kHz (Hz @ dB)")
    print("-" * 78)
    tracks = []
    for i in range(nsl):
        seg = x[i * HOP: i * HOP + N] * win
        S = np.abs(np.fft.rfft(seg))
        db = 20 * np.log10(S + 1e-9)
        band = freqs < 12000
        fb, dbb = freqs[band], db[band]
        # local maxima, strongest first
        pk = [j for j in range(2, len(dbb) - 2)
              if dbb[j] > dbb[j-1] and dbb[j] >= dbb[j+1] and dbb[j] > dbb.max() - 42]
        pk.sort(key=lambda j: -dbb[j])
        top = [(fb[j], dbb[j]) for j in pk[:6]]
        t = t0 + i * HOP / SR
        tracks.append((t, top))
        print(f"{t:6.2f} | " + "  ".join(f"{f:7.1f}@{d:5.1f}" for f, d in top))

    # Which partials move DOWN over the capture?  Follow each slice's peaks to the nearest
    # peak in the next slice and report the net drift of the longest chains.
    print("\ndescending ridges (nearest-peak chaining, >200 Hz net fall):")
    used = set()
    for si, (t, top) in enumerate(tracks):
        for f0, d0 in top:
            if (si, round(f0)) in used:
                continue
            chain, f = [(t, f0)], f0
            for sj in range(si + 1, len(tracks)):
                tj, tp = tracks[sj]
                if not tp:
                    break
                nf, nd = min(tp, key=lambda p: abs(p[0] - f))
                if abs(nf - f) > 300:            # lost the ridge
                    break
                used.add((sj, round(nf)))
                chain.append((tj, nf)); f = nf
            if len(chain) >= 4 and chain[0][1] - chain[-1][1] > 200:
                print(f"  {chain[0][0]:5.2f}s {chain[0][1]:7.1f} Hz  ->  "
                      f"{chain[-1][0]:5.2f}s {chain[-1][1]:7.1f} Hz   "
                      f"(fell {chain[0][1]-chain[-1][1]:6.1f} Hz over {chain[-1][0]-chain[0][0]:.2f}s)")

if __name__ == "__main__":
    main()
