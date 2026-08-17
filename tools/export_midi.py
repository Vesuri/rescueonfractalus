#!/usr/bin/env python3
"""Export Rescue on Fractalus!'s three pieces of music as General MIDI files.

Usage:  python3 tools/export_midi.py [--out build/midi]

The game has exactly three composed pieces, driven by TWO unrelated players:

  standby_theme.mid        the Standby-screen attract tune   -- sfx_voice_tick   $70F9
  game_over_jingle.mid     song 0, played on game over       -- music_player_tick $7253
  level_complete_jingle.mid song 1, level complete / hi-score -- music_player_tick $7253

Nothing else in the binary is a melody: station_audio $1B5B is clock-modulated sweeps with no
pitched content, and the 33-event SFX engine (docs/sfx-events.md) is envelope-driven effects.

Both players are emulated tick by tick rather than having their data re-read as a "grammar",
because in both cases the audible result is not a note list -- see the two decoder docstrings.
Pitches come from the AUDF/AUDC writes each player makes, converted through the SAME POKEY
waveform model the Amiga port uses (see wave_period: a poly-distortion voice does NOT sound at
its AUDF frequency).  Requires nothing but the stdlib.
"""
import argparse
import math
import os
import struct

ROM = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "disasm", "rof_mem.bin")

# ---------------------------------------------------------------------------
# POKEY -> pitch
# ---------------------------------------------------------------------------
# ⚠ TV SYSTEM SETS BOTH THE TEMPO AND THE PITCH, and it is NOT a detail: both players are driven
# from the VBI, so the field rate IS the tempo, and the same master clock divides down to the
# tone frequency.  PAL is the default here because that is what both references run: the
# project's atari800 ground truth (DEFAULT_TV_MODE=PAL) and the Amiga target (run.sh passes
# --ntsc_mode=0 to an A500+).  Reading the game as NTSC makes every piece play 20% too fast --
# the Standby theme comes out at 37.5 BPM instead of 31.2.
TV = {                       # master clock Hz, scanlines per frame
    'pal':  (1773447.0, 312),
    'ntsc': (1789790.0, 262),
}
MASTER_HZ, LINES = TV['pal']
FRAME_HZ = MASTER_HZ / (114 * LINES)        # PAL 49.8607 Hz / NTSC 59.9233 Hz


def set_tv(system):
    """Switch between 'pal' and 'ntsc'.  Rebinds the module-level clock constants that both
    decoders and every frequency read through."""
    global MASTER_HZ, LINES, FRAME_HZ, THEME_HZ
    MASTER_HZ, LINES = TV[system]
    FRAME_HZ = MASTER_HZ / (114 * LINES)
    THEME_HZ = FRAME_HZ / 2


TICKS_PER_BEAT = 480                        # MIDI resolution

# POKEY's polynomial bit streams (atari800 pokey.c; mirrored in PlatformAmiga.cpp).
KBIT4 = (1, 1, 1, 1, 0, 0, 0, 1, 0, 0, 1, 1, 0, 1, 0)
KBIT5 = (1, 1, 1, 1, 0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0,
         0, 0, 0, 1, 1, 1, 0, 0, 1, 0, 0, 0, 1, 0, 1, 0)

# AUDC distortion bits
PURETONE = 0x20
POLY4 = 0x40
NOTPOLY5 = 0x80


def freq(audf):
    """Divider output frequency for an 8-bit channel with AUDCTL=0 (64 kHz base).  BOTH players
    write AUDCTL=0 -- music_init_state at $7248 and the theme's arm at $70EC -- so no channel is
    ever 16-bit-joined or on the 15 kHz clock, and this one formula covers all eight voices.
    PAL runs 0.9% flat of NTSC (0.16 semitones), which never crosses a rounding boundary here."""
    return MASTER_HZ / (56 * (audf + 1)) if audf is not None else None


def wave_period(audf, audc):
    """Repetition period, in output samples, of this channel's waveform.

    ⚠ This is the correction that makes a distorted voice's pitch come out right.  A PURE tone
    emits one cycle per two divider underflows, so it sounds at freq().  A poly-distortion voice
    does NOT: POKEY's poly counters run continuously off the master clock and are only SAMPLED at
    each underflow, so between samples the counter advances by stride = (AUDF+1)*28 and the
    visible sequence is the poly bits stepped by stride % len.  Its period is therefore a
    function of AUDF, and the audible fundamental is freq()*2/period -- often octaves below
    freq().  This mirrors exactly how PlatformAmiga.cpp picks poly4_wave[stride%15] /
    poly5_wave[stride%31], which is why the port plays these voices correctly.
    """
    poly4 = not (audc & PURETONE) and (audc & POLY4)
    poly5tone = (audc & PURETONE) and not (audc & NOTPOLY5)
    if not poly4 and not poly5tone:
        return 2
    stride = (audf + 1) * 28
    if poly4:
        step, bits, mod = stride % 15, KBIT4, 15
    else:
        step, bits, mod = stride % 31, KBIT5, 31
    out, p, seen = 0, 0, {}
    for i in range(4 * mod + 4):
        st = (p, out)
        if st in seen:
            return i - seen[st]
        seen[st] = i
        p = (p + step) % mod
        if (bits[p] == (out ^ 1)) if poly4 else (bits[p] != 0):
            out ^= 1
    return 2


def voice_freq(audf, audc):
    """Audible fundamental of one voice, distortion accounted for."""
    if audf is None:
        return None
    return 2.0 * freq(audf) / wave_period(audf, audc)


def midi_note(f):
    return round(69 + 12 * math.log2(f / 440.0)) if f else None


# ---------------------------------------------------------------------------
# MIDI file plumbing (format 1, no dependencies)
# ---------------------------------------------------------------------------

def vlq(n):
    out = [n & 0x7F]
    n >>= 7
    while n:
        out.append((n & 0x7F) | 0x80)
        n >>= 7
    return bytes(reversed(out))


def track_chunk(events):
    """events = [(abs_midi_tick, raw_event_bytes)]"""
    events = sorted(events, key=lambda e: e[0])
    out = bytearray()
    last = 0
    for t, b in events:
        out += vlq(t - last) + b
        last = t
    out += vlq(0) + b"\xff\x2f\x00"
    return b"MTrk" + struct.pack(">I", len(out)) + bytes(out)


def tempo_event(us_per_quarter):
    return b"\xff\x51\x03" + struct.pack(">I", us_per_quarter)[1:]


def write_midi(path, tracks):
    with open(path, "wb") as f:
        f.write(b"MThd" + struct.pack(">IHHH", 6, 1, len(tracks), TICKS_PER_BEAT))
        for t in tracks:
            f.write(t)


def note_track(channel, program, notes, to_midi, velocity):
    """notes = [(on_vbi_tick, off_vbi_tick, midi_note, level)]"""
    evs = [(0, bytes([0xC0 | channel, program]))]
    for on, off, note, level in notes:
        if note is None:
            continue
        a, b = to_midi(on), to_midi(off)
        if b <= a:
            b = a + 1
        evs.append((a, bytes([0x90 | channel, note, velocity(level)])))
        evs.append((b, bytes([0x80 | channel, note, 0])))
    return track_chunk(evs)


# ---------------------------------------------------------------------------
# Player 1 -- the two jingles (music_init_state $7238 / music_player_tick $7253)
# ---------------------------------------------------------------------------
# Song table: two 6-byte headers at $731E = (stream_lo, stream_hi, level_loud, level_soft,
# attack_delta, release_delta).  Song 0's stream is $7346, song 1's $732A.
SONG_TABLE = 0x731E
INSTR_TABLE = 0x7375        # 4 AUDF bytes per instrument, VOICE 6 FIRST
DIST_ADDR = 0x73C1          # per-voice AUDC distortion, stride 2

JINGLE_VOICES = (0, 2, 4, 6)   # the 6502 indexes voices by X = 0/2/4/6, not 0..3


def run_jingle(data, header_base):
    """Emulate music_player_tick one VBI tick at a time and record every AUDF/AUDC write.

    Emulating rather than parsing matters here: the stream is (duration byte, voice-command
    byte) pairs interleaved with instrument selects, and the pointer advance at $72BB-$72BF
    skips BOTH bytes.  Advancing by only the duration byte re-reads each command byte as the
    next event's leader, which misaligns everything after the first event.

    Envelope model: on a new event every voice gets 4 attack ticks (delta = attack_delta) and
    then ALL voices switch to release_delta for the event's remaining `duration` ticks, so an
    event lasts 4 + duration ticks.  Per tick, accum += delta as an 8-bit ADC; if the result
    looks negative it clamps to 0 ($7309 BPL), and AUDC = (accum >> 3) ^ distortion -- i.e. the
    volume nibble IS the envelope.
    """
    stream_lo, stream_hi, lvl_loud, lvl_soft, attack_delta, release_delta = \
        data[header_base:header_base + 6]
    ptr = (stream_hi << 8) | stream_lo

    pitch = dict.fromkeys(JINGLE_VOICES, 0)     # staged AUDF, $0650 + X
    accum = dict.fromkeys(JINGLE_VOICES, 0)     # envelope level, $0648 + X
    delta = dict.fromkeys(JINGLE_VOICES, 0)     # envelope slope, $0649 + X
    attack_ctr = 0                              # $0651, music_init_state leaves it 0
    dur_ctr = 1                                 # $0653, left at 1 so tick 1 loads event 1
    tick = 0
    trace = {X: [] for X in JINGLE_VOICES}      # (tick, 'AUDF'|'AUDC', value)
    events = []                                 # (start_tick, length_ticks)

    def load_event():
        nonlocal ptr, attack_ctr, dur_ctr
        y = 0
        while True:
            b = data[ptr + y]
            if b == 0:
                return False                    # $00 ends the song
            if b < 0xC0:
                break
            # Instrument select: table[(~cmd)*4] -> the four staged pitches, voice 6 FIRST
            base = INSTR_TABLE + (((0xFF ^ b) << 2) & 0xFF)
            for i, X in enumerate((6, 4, 2, 0)):
                pitch[X] = data[base + i]
            y += 1
        dur_ctr, attack_ctr = b, 4
        events.append((tick, 4 + b))
        vcmd = data[ptr + y + 1]
        v = vcmd
        for X in (6, 4, 2, 0):
            code = v & 0xC0
            v = (v << 2) & 0xFF
            if code == 0x00:                    # voice off
                accum[X] = delta[X] = 0
            elif code == 0x40:                  # hold / tie: nothing touched
                pass
            else:                               # $C0 loud / $80 soft retrigger
                accum[X] = lvl_loud if code == 0xC0 else lvl_soft
                delta[X] = attack_delta
                trace[X].append((tick, 'AUDF', pitch[X]))
        ptr += y + 2
        return True

    while True:
        if attack_ctr:
            attack_ctr -= 1
            if attack_ctr == 0:
                for X in JINGLE_VOICES:
                    delta[X] = release_delta
        else:
            dur_ctr -= 1
            if dur_ctr == 0 and not load_event():
                break
        for X in JINGLE_VOICES:
            a = (accum[X] + delta[X]) & 0xFF
            accum[X] = 0 if a & 0x80 else a
            trace[X].append((tick, 'AUDC', (accum[X] >> 3) ^ data[DIST_ADDR + X]))
        tick += 1

    return trace, events, tick


def jingle_notes(trace, audc_dist):
    """Turn one voice's write trace into notes.  Sounding = AUDC volume nibble nonzero; pitch =
    the last AUDF written, read through voice_freq so a poly4 voice lands on its real octave."""
    notes, start, note, level, cur_audf, prev_vol = [], None, None, 0, None, 0
    for tick, kind, val in trace:
        if kind == 'AUDF':
            cur_audf = val
            if note is not None:                # retrigger while sounding: split the note
                notes.append((start, tick, note, level))
                start, note, level = tick, midi_note(voice_freq(cur_audf, audc_dist)), 0
        else:
            vol = val & 0x0F
            if vol and not prev_vol:
                start, note, level = tick, midi_note(voice_freq(cur_audf, audc_dist)), vol
            elif prev_vol and not vol:
                if note is not None:
                    notes.append((start, tick, note, level))
                note = None
            elif note is not None:
                level = max(level, vol)
            prev_vol = vol
    if note is not None:
        notes.append((start, tick + 1, note, level))
    return notes


NOMINALS = (0.125, 0.25, 0.5, 1.0, 2.0, 4.0)     # note values, in quarter notes


def ritardando_map(events, rate_hz):
    """Split the jingles' raw tick durations into (note value, tempo).

    The player has no notion of tempo -- an event just names a length in VBI ticks -- so the
    game-over jingle's ritardando is a run of lengths (49,49,49,57,65,73,81,81) that is musically
    ONE repeated note value getting slower.  Writing those literally at a fixed tempo notates
    them as meaningless tuplets.  Instead each event takes the note value closest in log space to
    a RUNNING reference length, and whatever is left over becomes a MIDI tempo change; the
    reference then follows the event, so a gradual slowdown stays one repeated value instead of
    creeping into the next.  Which value the prevailing length is called is free, so a power of
    two is chosen to land the prevailing tempo in a readable 60..180 BPM.

    Returns (vbi_tick -> midi_tick, [(midi_tick, us_per_quarter)]).
    """
    lengths = [n for _, n in events]
    modal = max(set(lengths), key=lambda n: (lengths.count(n), n))

    def pass_over(scale):
        ref = modal
        out, midi_t = [], 0
        for start, length in events:
            nominal = min(NOMINALS, key=lambda q: abs(math.log2(length / (ref * q))))
            ref = length / nominal
            beats = nominal * scale
            span = int(round(beats * TICKS_PER_BEAT))
            us = int(round(length / rate_hz / beats * 1e6))
            out.append((start, length, midi_t, span, us))
            midi_t += span
        return out, midi_t

    # Anchor the note-value scale on the MEDIAN tempo, not the first event's: the reference
    # adapts as it goes, so judging by the opening length alone can leave the rest of the piece
    # stranded at 300 BPM.  Timing is unaffected either way -- span and tempo compensate exactly.
    # The window is deliberately wide: a game-over dirge genuinely sits in the 40s, and forcing
    # it up into a "normal" range only renames its note values.
    scale = 1.0
    for _ in range(8):
        rows, _end = pass_over(scale)
        bpms = sorted(60e6 / us for *_r, us in rows)
        median = bpms[len(bpms) // 2]
        if median >= 200.0:
            scale /= 2.0
        elif median < 40.0:
            scale *= 2.0
        else:
            break

    rows, midi_end = pass_over(scale)
    bounds, tempos = [], []
    for start, length, midi_t, span, us in rows:
        if not tempos or tempos[-1][1] != us:
            tempos.append((midi_t, us))
        bounds.append((start, length, midi_t, span))

    def to_midi(t):
        for vs, vl, ms, ml in bounds:
            if t < vs + vl:
                return ms if t <= vs else ms + int(round((t - vs) / vl * ml))
        return midi_end
    return to_midi, tempos


def export_jingle(data, out_path, song, program=80):
    trace, events, end = run_jingle(data, SONG_TABLE + 6 * song)
    to_midi, tempos = ritardando_map(events, FRAME_HZ)
    tracks = [track_chunk([(t, tempo_event(us)) for t, us in tempos])]
    for ch, X in enumerate(JINGLE_VOICES):
        notes = jingle_notes(trace[X], data[DIST_ADDR + X])
        tracks.append(note_track(ch, program, notes, to_midi,
                                 lambda lvl: max(20, min(127, 8 * lvl + 20))))
    write_midi(out_path, tracks)
    bpm = " ".join("%.0f" % (60e6 / us) for _, us in tempos)
    print("  %-26s %5.2f s  tempo %s BPM" % (os.path.basename(out_path), end / FRAME_HZ, bpm))


# ---------------------------------------------------------------------------
# Player 2 -- the Standby theme (sfx_voice_tick $70F9 / sfx_seq_step $7148)
# ---------------------------------------------------------------------------
THEME_AUDF = {1: 0x71AB, 2: 0x719E, 3: 0x7191, 4: 0x71B8}   # channel -> chord preset table
THEME_AUDC4 = 0x71C5
THEME_DUR = 0x71D2
THEME_STREAM = 0x71DB

# ⚠ The theme ticks every OTHER frame, not every frame: its only call site, $5356, is gated by
# `LDA $00E7 / BIT $062D / BNE`, and $00E7 = 1 while the theme plays, so the call happens only
# when bit 0 of the per-frame counter $062D is clear.  Getting this wrong makes the tune play at
# double speed.  The jingles are NOT gated ($5359-$535E) and do run every frame.
THEME_HZ = FRAME_HZ / 2      # rebound by set_tv()

# The theme holds a steady pulse -- unlike the game-over jingle it never changes speed, and its
# 17/3/4-tick figure is a dotted rhythm (17+3+4 = 12+12 = 24 ticks).  So it gets ONE tempo and a
# literal tick mapping, which keeps the timing exact: 48 ticks per quarter puts the melody's
# 24-tick unit on the eighth note.
THEME_TICKS_PER_BEAT = 48


def run_theme(data):
    """Emulate sfx_voice_tick/sfx_seq_step one theme tick at a time.

    The score is not a note list, which is what makes this driver easy to misread:
      * A byte with bit7 SET is a CHORD preset (index = byte & $1F, 13 of them).  It loads all
        four AUDF registers at once from four parallel tables plus AUDC4, then keeps scanning.
        So the harmony moves in blocks, independently of the notes.
      * A byte with bit7 CLEAR is a note: the low 5 bits index the duration table, and the HIGH
        NIBBLE selects which register gets emphasised via `STA $D1FF,Y` -- Y = 2/4/6 is
        AUDC1/AUDC2/AUDC3.  A high nibble of 0 mutes all three (a rest).
      * Channels 1-3 sound together the whole time at the same volume; the MELODY is whichever
        one is 2 louder.  There is no channel that carries the tune on its own.
      * Channel 4 is never touched by the tick -- a drone whose pitch and waveform change only
        on a chord preset.
      * $00 terminates and the player restarts at index 0, so the theme loops.  We emit one pass.

    Volume per tick: v = min(timer >> 1, 2), forced to 3 while the timer is high (the CMP #3
    carry feeds the ADC), so a note holds at 3 and decays 2,2,1,1,0,0 over its last six ticks.
    """
    audf = {1: 0, 2: 0, 3: 0, 4: 0}
    audc4 = 0
    seq = 0xFF          # $073C: $70F5 leaves it at -1 so the first INX lands on index 0
    timer = 0           # $073A
    sel = 0             # $073B
    tick = 0
    events = []
    cur = {c: None for c in (1, 2, 3, 4)}       # (start_tick, note, max_level)
    notes = {c: [] for c in (1, 2, 3, 4)}

    def close(ch, at):
        if cur[ch]:
            s, n, lvl = cur[ch]
            if at > s:
                notes[ch].append((s, at, n, lvl))
        cur[ch] = None

    def open_or_hold(ch, note, level):
        if cur[ch] and cur[ch][1] != note:
            close(ch, tick)
        if note is None:
            close(ch, tick)
        elif cur[ch] is None:
            cur[ch] = (tick, note, level)
        else:
            s, n, lvl = cur[ch]
            cur[ch] = (s, n, max(lvl, level))

    def seq_step():
        nonlocal seq, timer, sel, audc4
        x = seq
        while True:
            x = (x + 1) & 0xFF
            b = data[THEME_STREAM + x]
            if b == 0:
                return False
            if b & 0x80:
                idx = b & 0x1F
                for ch, tbl in THEME_AUDF.items():
                    audf[ch] = data[tbl + idx]
                audc4 = data[THEME_AUDC4 + idx]
                if audc4 == 0:
                    break               # a zero AUDC4 falls through as a rest
                continue
            break
        seq = x
        timer = data[THEME_DUR + (b & 0x1F)]
        sel = b >> 4
        return True

    while True:
        timer -= 1
        if timer < 0:
            prev4 = (audf[4], audc4)
            if not seq_step():
                break
            events.append((tick, timer + 1))    # DEC/BPL => the note lasts timer+1 ticks
            if (audf[4], audc4) != prev4:
                close(4, tick)
        v = 3 if (timer >> 1) >= 3 else (timer >> 1)
        for ch in (1, 2, 3):
            level = 0 if sel == 0 else v + (2 if sel == 2 * ch else 0)
            # channels 1-3 are always pure tone ($A0 | volume)
            open_or_hold(ch, midi_note(freq(audf[ch])) if level else None, level)
        open_or_hold(4, midi_note(voice_freq(audf[4], audc4)) if audc4 & 0x0F else None,
                     audc4 & 0x0F)
        tick += 1

    for ch in (1, 2, 3, 4):
        close(ch, tick)
    return notes, events, tick


def export_theme(data, out_path, program=80):
    notes, events, end = run_theme(data)
    per_vbi = TICKS_PER_BEAT // THEME_TICKS_PER_BEAT
    us = int(round(THEME_TICKS_PER_BEAT / THEME_HZ * 1e6))
    tracks = [track_chunk([(0, tempo_event(us))])]
    for ch, chan in enumerate((1, 2, 3, 4)):
        tracks.append(note_track(ch, program, notes[chan], lambda t: t * per_vbi,
                                 lambda lvl: max(20, min(127, 20 * lvl + 20))))
    write_midi(out_path, tracks)
    print("  %-26s %5.2f s  tempo %.1f BPM (one pass; the theme loops)"
          % (os.path.basename(out_path), end / THEME_HZ, 60e6 / us))


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--out", default="build/midi", help="output directory (default build/midi)")
    ap.add_argument("--rom", default=ROM, help="path to disasm/rof_mem.bin")
    ap.add_argument("--tv", choices=("pal", "ntsc"), default="pal",
                    help="TV system: sets both tempo and pitch (default pal -- what atari800 "
                         "and the Amiga target both run)")
    args = ap.parse_args()

    set_tv(args.tv)
    with open(args.rom, "rb") as f:
        data = f.read()
    os.makedirs(args.out, exist_ok=True)
    print("Exporting to %s/ (%s %.4f Hz; theme ticks at %.4f Hz)"
          % (args.out, args.tv.upper(), FRAME_HZ, THEME_HZ))
    export_theme(data, os.path.join(args.out, "standby_theme.mid"))
    export_jingle(data, os.path.join(args.out, "game_over_jingle.mid"), song=0)
    export_jingle(data, os.path.join(args.out, "level_complete_jingle.mid"), song=1)


if __name__ == "__main__":
    main()
