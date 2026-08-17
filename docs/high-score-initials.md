# The high-score initials entry — and why it never appears

`name_entry_loop $5B6C` (with `render_text_cell $5CA7` as its glyph blitter) is the initials entry
for a beaten high score. It is **called on every game over** — and it returns immediately, on the
Atari as much as on the Amiga. This file records why, because the answer is not in the code we
execute but in the code that was removed from the binary we port.

## The gate

`name_entry_loop`'s first instruction is `JSR validate_save_state $5D0D`, and it bails to `L_5b7e`
(`restore_display_if_E7`; RTS) unless that returns Z. The check is:

| test | note |
|---|---|
| `mem[$3700] == $28` | |
| `mem[$3714] == $EE` | |
| `mem[$37C7+Y] == mem[$7BDA+Y]` for `Y = $26..1` | `$7BDA` = `"XCopyright (c) 1987 Lucasfilm Ltd. v4.1"`. The loop is `DEY / BNE`, so **`Y=0` is never compared** — `$37C7` itself is a don't-care |

Past the gate, one of `attract_timer $00E2` bit7, `game_var_37F4` or `game_var_E4` must be non-zero;
the results path satisfies that (`standby_scoreboard_render` sets `$37F4 = $00E5`).

## Where the block was supposed to come from: DISK

`game_init_5D50 $5D50` — called early in `game_entry` — builds two SIO DCBs:

```
5d74  STA $0304 / 5d77 LDA #$37 / 5d79 STA $0305   ; DBUFLO/DBUFHI = $3700
5d7c  LDA #$CE  / 5d7e STA $030A ... STA $030B     ; DAUX1/DAUX2   = sector $02CE (718)
5d86  EA EA EA                                     ; <-- the sector read, NOPped out
5d89  ...                                          ; DBUF = $3780, sector $02CF (719)
5d9d  EA EA EA                                     ; <-- the second read, NOPped out
```

So the save block — high scores, initials and the copyright signature that proves the disk is
genuine — was read from two sectors of the game disk, and **both reads are three NOPs in
`rof.xex`**, patched out when the disk game was converted to an executable. `$3700-$37FF` is
therefore all zeros, the signature never matches, and the entry can never run.

Confirmed against the original, not inferred: **all 51 `a800dumps/*.a8s` have `$3700`, `$3714` and
`$37C7..$37ED` = 0.** `rof_mem.bin` (the loaded image) has them zero too — nothing in the binary
writes them (raw-binary scan for `STA $37C7,Y` / `LDA $7BDA,Y`: only the compare at `$5D1D`).

Consequence for the port: skipping the entry is **faithful**, and the only faithful behaviour
available — there is no disk to read. Reproducing the sector contents would be an addition to the
game, not a port fix; it is the user's call, not a bug to close.

## Testing it anyway: `make NAME_ENTRY=1`

Reaches the screen by writing what the removed read would have delivered. ~4 s into flight it seeds
a current score, writes `$28`/`$EE`/the `$7BDA` signature into the save block, and arms the
energy-out death cinematic `$063D`; the faithful binary then runs the death teardown, the results
card, the game-over tune, and the entry. Needs no `PROBES`, so `out/RoF` stays playable and the
letters can be picked by hand (also on real hardware). Headless:
`make PROBES=1 NAME_ENTRY=1` + `GDBSCRIPT=name_entry.gdb ./diag_run.sh 120`, whose probe counters
separate the two outcomes that look identical from the outside:

```
probes: enter=1 pass=1 wait=0 glyph=2818
        ^ called   ^ past the gate      ^ render_text_cell's per-glyph input wait, iterating
```

`enter=1 pass=0` = the gate rejected the block (the normal, un-seeded behaviour).

## ⚠ The three spins in this path were hangs

`render_text_cell`'s wait on the console sample `$0049`, `name_entry_loop`'s wait on `$0003` /
`$0091 == $C0`, and `sound_retrigger_random`'s wait for the game-over tune to end (`$00E5`) all had
no yield in their bodies, so on the Amiga GCC hoisted the load and emitted `bra.s .` — the same bug
class as the level-complete lift (`docs/boost-cinematic-plan.md`, last section). They now carry
`SPINWAIT_HOOKS` entries (`tools/transpile.py`). Of the three, only the `sound_retrigger_random`
one was reachable in normal play (the results card with `$0004 != 0`); the other two sat behind this
gate, which is why nobody hit them.
