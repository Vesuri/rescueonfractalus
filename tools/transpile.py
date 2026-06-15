#!/usr/bin/env python3
"""Transpile the Ghidra 6502 disassembly listing to C.

Reads  disasm/listing.txt  +  disasm/symbols.csv
Writes src/gen/rof_gen.c       (one C function per 6502 routine)
       src/gen/rof_decl.h      (forward declarations)
       src/gen/rof_manual.c    (hand-written replacements for SMC routines)

Design
------
* Each 6502 routine becomes a void C function.
* JSR  → direct function call (always static; no indirect JMPs exist).
* RTS  → return;
* RTI  → platform_rti(); return;
* Branches (BEQ etc.) → if (flag) goto L_xxxx;
* JMP  within same function → goto L_xxxx;
* JMP  to a different function → callee(); return;   (tail call)
* Stack, flags, registers modelled via cpu.h macros.
* Hardware addresses ($D000-$D7FF) → bus_read/bus_write.
* All other addresses → mem[] direct.
* ZP-indexed wraps using (uint8_t) cast.
* Known self-modifying function screen_page_swap ($1A62) is skipped
  and provided hand-written in rof_manual.c.
"""
import re
import sys
from pathlib import Path
from collections import defaultdict

ROOT = Path(__file__).parent.parent
LISTING  = ROOT / "disasm/listing.txt"
SYM_CSV  = ROOT / "disasm/symbols.csv"
OUT_C    = ROOT / "src/gen/rof_gen.c"
OUT_H    = ROOT / "src/gen/rof_decl.h"
OUT_MAN  = ROOT / "src/gen/rof_manual.c"

# Self-modifying functions: skip in generated code, provide manual impl.
MANUAL_FUNCS = {
    0x1a62,  # screen_page_swap
    0x49EE,  # dli_handler_game   — needs INC $C7 after dispatch + cockpit variant
    0x6CC2,  # dli_handler_game2  — same
    # Attract per-frame leaves: the genuine station_init control FLOW runs, but these
    # heavy animation routines must be native (the transpiled CPU-emulation is ~60x too
    # slow on the 68000).  Native versions in rof_manual.c do the same mem[] mutations
    # in plain C.  (display_scroll $1CF7 + station_sub_1f51 $1F51 are folded into these
    # as private helpers — their only callers are these three, so the transpiled copies
    # are dead.)
    0x1D9A,  # station_anim_frame  (calls display_scroll)
    0x1EB4,  # station_sub_1EB4
    0x1F48,  # station_sub_1F48    (walks channels via station_sub_1f51)
}

# Functions being reimplemented natively, validated against the transliteration.
# For each address here the transpiler still emits the faithful transliterated
# body, but DEFINES it under a `<name>__t6502` suffix instead of the plain name.
# The plain `<name>()` — the one all call sites invoke — is provided by a
# hand-written native version in src/gen/rof_native.c.  Both coexist so the
# validation harness (tools/validate_native.c) can run them on the same input
# state and diff the full machine state.  This is the regen-safe strangler-fig
# seam: drop an address in, write the native twin, prove equivalence, ship it;
# everything not listed here stays transliterated and fully regenerable.
VALIDATE_FUNCS = {
    0x9D6F,  # divide_16x16 — restoring 16-bit divide (prototype target)
    0xAD5F,  # clear_terrain_column — clear a terrain column band + object-table cells (flight leaf #1)
    0x9C97,  # signed_mul_8x16 — fixed-point signed 8x16 multiply (flight leaf #2)
    0x9C55,  # sine_table_lookup — quarter-wave sine/cos table lookup (flight leaf #3a)
    0x9BDB,  # trig_interp_lookup — sine interpolation angle..angle+1 (flight leaf #3b)
    0xAD2B,  # compute_row_xspans — per-row horizontal span endpoints (flight leaf #4)
    0xAC42,  # check_target_in_window — 2-consecutive-hit target latch (flight leaf #5)
    0x4E58,  # obj_table_set_active — promote first eligible object slot (flight leaf #6)
    0x55FF,  # ring_push_0719 — push to event ring + restore caller X (flight leaf #7)
    0x9BA0,  # compute_heading_sincos — sin/cos of 16-bit heading (flight mid #1)
    0xA0A3,  # build_view_transform_matrix — rotated view components (flight mid #2)
    0xAC93,  # setup_projection_params — per-frame projection/view setup (flight mid #3)
    0xAB7B,  # set_plot_mask_and_halve_step — plot base ptr + step/4 (flight mid #4)
    0xA8AF,  # terrain_point_distance — Manhattan dist, nearest-point latch (flight mid #5)
    0xB2CC,  # terrain_midpoint_displace — fractal midpoint subdivision (flight mid #6)
    0xA6D3,  # terrain_plot_pixel — OR voxel mask into terrain bitmap (flight raster #1)
    0xA6CB,  # terrain_clip_row_top — clip column top vs row limit (flight raster #2)
    0xAB9A,  # raster_fill_region — nested 12x32 cell fill via terrain_clip_row_top (flight raster #3)
    0xA822,  # terrain_plot_object_a — plot one terrain object, variant A (flight raster #4)
    0xA90A,  # terrain_plot_object_b — plot one terrain object, variant B (flight raster #5)
    0xA63B,  # terrain_plot_object — per-object raster dispatch (flight raster #6)
    0xB33D,  # terrain_column_rasterize — fractal column renderer (flight raster #7, the big one)
    0xB172,  # terrain_subdivide_column — fractal subdivision driver (flight raster #8)
    0xA613,  # terrain_jitter_column — per-frame random terrain jitter (flight top #1)
    0x9E54,  # terrain_frame_setup — terrain gen step 1: view setup + per-column transform (flight top #2)
    0xA11F,  # project_terrain_points — per-object world->screen projection via divide_16x16 (flight top #3)
    0xAE53,  # terrain_collision — collision row scan + silhouette fill + column raster (flight top #4)
    0xA31E,  # terrain_draw_frame — main per-frame terrain driver (flight top #5, the last)
    # --- flight ISR de-transpile (2026-06-11): eliminate transpiled code on the
    #     per-frame VBI path.  Small subtrees first; the flight_control_integrate
    #     tree (~27 fns) is deferred. ---
    0x49A0,  # render_bcd_counter — score BCD render (self-contained $49A0-$49ED fall-through chain)
    # update_terrain_scanline_proj subtree, leaves first:
    0x9B87,  # init_proj_scratch_pointers — set game_state + 3 ZP ptr bytes (trivial leaf)
    0x5815,  # ring_push_marked — push X|$80 to the $0719 event ring (tail to ring_push_0719)
    0x55FC,  # game_sub_55FC — push Y to the $0719 event ring (falls into ring_push_0719)
    0x9A36,  # sample_terrain_height_bilerp — bilinear height sample over the $0900 map (leaf)
    0x451D,  # game_sub_451d — 14-iter table-fill into $2159/$2189 (leaf; called by update_terrain_horizon_lr)
    0x9B0D,  # enter_terrain_special_state — set flags + ring events (calls game_sub_55FC/ring_push_marked)
    0x9B4C,  # exit_terrain_special_state — inverse of enter (calls game_sub_55FC)
    0x992D,  # update_terrain_horizon_lr — L/R horizon update (calls sample_terrain_height_bilerp + game_sub_451d)
    0x9833,  # update_terrain_scanline_proj — TOP of subtree: map coords + depth + horizon (calls all the above)
    # --- flight_control_integrate subtree (2026-06-12): the last transpiled code on
    #     the flight VBI path (~31 fns). Leaves-first. ---
    0x4E98,  # reset_flags_ff — set $006A/$0063/$2826 = $FF (leaf, mem-only)
    0x94BF,  # load_velocity_from_param_block — seed vel accums $2854-$2863 from param block (tail ring_push_marked)
    0x7B88,  # bcd_inc_counter_0641 — $0641 += 1 (binary; cpu core ignores decimal mode)
    0x7B80,  # set_place_params_inc_count — $0045=0,$0046=1, tail bcd_inc_counter_0641
    0x96D9,  # trigger_object_explosion — set explosion sprite ptrs + INC $0041, tail ring_push_marked X=$0F
    0x9677,  # reset_object_slot — $0036=$80, tail ring_push_marked X=$0E
    0xB756,  # enqueue_indicator_event — write indicator HUD params, tail game_sub_55FC Y=$08
    0x930E,  # object_integrate_position — 24-bit world-pos integrate + blip $2821/$2824 (mem-only)
    0xAA95,  # jitter_roll_pitch — random-walk pitch/roll accums $0029/$0026 + decay $002E (RANDOM, mem-only)
    0x9821,  # mul_u8 — shift-add multiply, result in cpu.A (consumes $006B/$28D6)
    0x9713,  # compute_target_blip_position — derive blip $0021/$0027 from range/depth/parallax (mem-only)
    0x4E1C,  # obj_table_scan_replace — random-start stride-$43 scan to place entry val in a free obj slot (RANDOM)
    # batch 2 — shallow drivers (1 transpiled callee each):
    0x4E18,  # obj_table_scan_y1_c8 — set Y=1, tail obj_table_scan_a_c8
    0x4E1A,  # obj_table_scan_a_c8 — set A=$C8, tail obj_table_scan_replace
    0x4EA2,  # store_676_init — $0676=A, tail set_hud_fields_678_679
    0x4EA5,  # set_hud_fields_678_679 — $0678/$0679=A, tail refresh_hud_field_0b
    0x4EAB,  # refresh_hud_field_0b — Y=$0B game_sub_55FC, tail refresh_hud_field_0d_entry
    0x4EB0,  # refresh_hud_field_0d_entry — Y=$0D, tail refresh_hud_fields_0d_0e
    0x4EB2,  # refresh_hud_fields_0d_0e — game_sub_55FC at Y, INY, game_sub_55FC
    0x9473,  # step_object_along_axes — step $0023/$0024 by $14, depth dec / tail reset_flags_ff (mem-only)
    0xB786,  # reset_indicator_event — $0035=0, tail enqueue_indicator_event
    0x97A0,  # compute_obj_rel_angle_scale — 10-bit angle build + 2x mul_u8 -> $002B/$2881 (reads ENTRY CARRY)
    0x43E8,  # draw_object_column — draw PMG dial-bar column via $4581 ptr table (absorbs bare-RTS draw_bar_loop_end $442D)
    0x444A,  # setup_dial_bar_draw — set bar params, tail draw_object_column
    0x4447,  # game_sub_4447 — A+=8, tail setup_dial_bar_draw
    # batch 3 — mid drivers:
    0x7B3C,  # countdown_show_char_0620 — place countdown glyph in obj slot + DEC $0620
    0x93BD,  # check_object_in_target_box — in-box trigger ($003B/$2892/$3355/$2891)
    0x9680,  # check_player_proximity_hit — hit test vs player -> pickup/explosion (reads ENTRY CARRY)
    # batch 4 — apex:
    0x9552,  # object_step_and_collide — integrate pos + terrain/obj collision + pickup dispatch (PHA/PLA stack)
    0x8E5B,  # flight_control_integrate — THE flight VBI root: joystick+throttle integrate, HUD, ring rotate
    # --- flight main-loop de-transpile (2026-06-12): the last transpiled code on
    #     the flight per-frame path (game_state_update + enemy_check, called by
    #     flight_frame_native).  Leaves-first.  plot_line_done $AB26 is a bare RTS
    #     and is ABSORBED (native callers just return); game_sub_4f3f $4F3F is the
    #     0-callers event/teardown closure (reaches the whole program) and is NOT
    #     ported — enemy_check's $063D branch tail-calls it transpiled. ---
    0xAB27,  # plot_scanline_up — Bresenham point plotter, walks up (calls native terrain_plot_pixel)
    0xAAD4,  # plot_scanline_down — line-plot loop walking down (calls native terrain_plot_pixel)
    0xAACF,  # plot_scanline_rand_dir — RANDOM picks up vs down
    0xA99C,  # game_state_update — flight state machine (scanline plots + ring events)
    0x7AB8,  # pmg_enemy_update — enemy PMG update (RANDOM; tail ring_push_marked + jitter_roll_pitch)
    0x3FCD,  # enemy_check — event dispatch ($063D->game_sub_4f3f [transpiled] / $0633->pmg_enemy_update)
    # --- in-game SFX engine (2026-06-12): the $548D voice/gauge engine, run each
    #     flight VBI (Atari VBI tail $534D), drains the $0719 event ring the native
    #     flight code already fills.  Leaves-first.  POKEY writes via bus_write ->
    #     Paula on Amiga (the indexed $D1FE+X writes are masked in validation). ---
    0x5667,  # sfx_voice_write_freq — write AUDF for voice Y to POKEY $D1FE+X (skip if reg idx 0)
    0x5673,  # sfx_voice_write_freq_ctrl — write AUDF + AUDC (prio|distortion) for voice Y
    0x568A,  # sfx_pick_top_voice — scan 12 slots, latch max-priority active voice -> $0714/0715/0716
    0x56AF,  # sfx_pick_next_voice — scan for next-best priority excluding $0715 -> $0716/0717
    0x5553,  # sfx_engine_step — explosion/noise engine (RANDOM x2, descending-pitch via $55DC); entry A=$0634
    0x5614,  # reorder_sprite_slot — voice-priority mixer (calls 5673/568a/56af); entry X/Y, Y restored
    0x581C,  # input_init — load a new voice from event tables $56D4..$57F4 (stack-aware; tail game_sub_55FC)
    0x548D,  # update_gauge_digits — APEX: per-frame voice/gauge envelope engine + ring drain (Atari VBI tail)
    # --- startup/cinematic de-transpile (2026-06-15): the display_setup ($5F1D)
    #     subtree that drives the Standby + Doors/Tunnel/Planet cinematic — the
    #     slow part of boot (the flight loop is already native).  Leaves-first. ---
    0x4E84,  # bin_to_bcd — A(0-99)->packed BCD; units->$00C1, tens->Y, BCD->A (pure leaf)
    0x782A,  # copy_altitude_graphic_to_screen — Standby per-frame: copy 20-byte $5A9F+X block to $32B7..$32CA (leaf, entry Y)
    0x6DDF,  # init_row_coords_9c — store 5 constants into $009C-$00A0 (pure leaf)
    0x6B71,  # clear_scroll_accum — zero $02C0-$02C3, $00A1-$00A5 (pure leaf)
    0x75A5,  # copy_192_to_1800 — copy 192 bytes $350C->$1810..$18CF via $BB/$BC ptr (leaf)
    0x7460,  # build_row_addr_table — build 85-entry $073D/$0793 addr table from $C3:$C4 base + $C1 stride (leaf)
    0x65DF,  # build_line_addr_table_2000 — set base $2000/stride $2E, tail build_row_addr_table
    0x65D2,  # build_line_addr_table_1000_stride — set base $1000/stride=A, tail build_row_addr_table (entry A)
    0x65D0,  # build_line_addr_table_1000 — A=$2E, tail build_line_addr_table_1000_stride
    0x6B85,  # init_object_positions — zero $08D1-$08D3, build 22-entry word array $08A4/$08A5 = $6E2D table + $2EE0 (pure leaf)
    0x712D,  # audio_timer_setup — zero $00E7/$0655/$00E5 + POKEY timers $D201-$D207, AUDCTL=$60 (leaf; POKEY via bus_write)
    0x6B47,  # random_terrain_height — 2 RANDOM reads -> sparse height (result in A; seeded-LFSR test)
    0x665D,  # fill_horizontal_span — write pattern $00B9 across a row span into two row ptrs (leaf)
    0x66DE,  # plot_glyph_pixel_masked — OR/AND a 2-bit pixel into ($80)+Y via mask tbls $66E9/$66FB (leaf, entry X/Y)
    0x66D5,  # plot_pixel_masked — A=col -> Y/X mask index, tail plot_glyph_pixel_masked (entry A)
}
VALIDATE_SUFFIX = '__t6502'

HW_BASE, HW_END = 0xD000, 0xD800   # bus_read/bus_write range

# Spin-wait hook injection.
# When a backward branch loops back to one of these addresses, inject the
# listed platform call(s) so the SDL event loop / VBI can fire.  Without
# these, tight C spin-wait loops starve the platform and VBI never fires.
# Key: 6502 address of the loop-back label. Value: C statement(s) to inject.
SPINWAIT_HOOKS = {
    # L_1A18: station_init attract loop — spins on the $0080 sync flag set by the
    # attract VBI ($1B30).  Drive a frame each iteration so the attract animates and
    # the VBI fires (sets $0080 + RTCLOK); without it the loop is a frozen tight spin.
    0x1A18: 'platform_tick_vbi(); platform_render_frame();',
    0x3C75: 'platform_poll_events();',           # VCOUNT position wait
    0x3CB8: 'platform_tick_vbi(); platform_render_frame();',  # RTCLOK frame wait
    # L_3eba: main flight loop in FUN_3d48 — one full frame of terrain gen,
    # collision, enemy + game-state update per iteration, loops until the
    # flight phase ($72) reaches 2. On real HW the VBI fires asynchronously;
    # here it only fires when we tick it, so without a hook nothing renders
    # during flight (display frozen) and VBI-driven state never advances.
    0x3EBA: 'platform_tick_vbi(); platform_render_frame();',
    0x3F6D: 'platform_tick_vbi(); platform_render_frame();',
    0x4F43: 'platform_tick_vbi(); platform_render_frame();',
    0x5C4B: 'platform_tick_vbi(); platform_render_frame();',
    0x61C6: 'platform_tick_vbi(); platform_render_frame();',
    0x63D7: 'platform_tick_vbi(); platform_render_frame();',
    0x645B: 'platform_tick_vbi(); platform_render_frame();',
    0x646C: 'platform_tick_vbi(); platform_render_frame();',
    0x6478: 'platform_tick_vbi(); platform_render_frame();',
    0x656E: 'platform_tick_vbi(); platform_render_frame();',
    # L_6578: planet-rise loop in display_setup — paces FUN_6ba8 every 2 VBI
    # frames off RTCLOK ($14) until $1002==$FF. Without a VBI tick here the
    # frame counter never advances and the loop spins forever (planet never
    # rises into view after the star scroll).
    0x6578: 'platform_tick_vbi(); platform_render_frame();',
    0x79D0: 'platform_tick_vbi(); platform_render_frame();',
    # Attract-mode loops: need VBI to fire for animation, audio, and input.
    # L_62EB: outer loop entry — tick VBI to drive the game
    # L_62F6: inner input-poll loop — tick VBI here too so audio/rtclok work
    # L_634A: tightest inner loop (FUN_5A78 check) — poll events to detect keys
    0x62EB: 'platform_tick_vbi(); platform_render_frame();',
    0x62F6: 'platform_tick_vbi(); platform_render_frame();',
    0x634A: 'platform_poll_events();',
}

# ---------------------------------------------------------------------------
# Parse symbols.csv → addr_int → name
# ---------------------------------------------------------------------------
# addr_int → note (5th CSV column); populated by load_symbols, consumed when
# emitting each function so the symbol's note becomes a C doc-comment.
SYMBOL_NOTES = {}

def load_symbols(path):
    sym = {}
    SYMBOL_NOTES.clear()
    for line in path.read_text().splitlines():
        line = line.strip()
        if not line or line.startswith('#'): continue
        parts = line.split(',', 4)
        if len(parts) < 2: continue
        addr_s = parts[0].strip().lstrip('$')
        name   = parts[1].strip()
        try: addr_i = int(addr_s, 16)
        except ValueError: continue
        sym[addr_i] = name
        if len(parts) >= 5 and parts[4].strip():
            SYMBOL_NOTES[addr_i] = parts[4].strip()
    return sym

# ---------------------------------------------------------------------------
# func_lo: lowest 6502 address that belongs to a function's body.
# Normally the function start, but functions that absorbed an orphan
# prefix run (see attach_orphan_runs) begin lower than their named entry.
# Use this for range/containment tests so branches into the prefix resolve
# as local labels, not stub calls.
# ---------------------------------------------------------------------------
def func_lo(f):
    return f.get('body_start', f['start'])

# ---------------------------------------------------------------------------
# attach_orphan_runs: fix Ghidra function-boundary mis-detection.
#
# Ghidra occasionally clips a routine's entry to a later instruction (e.g. a
# loop *test*), leaving the loop *body* that precedes it stranded in the gap
# between two functions.  Those orphan instructions belong to no function, so
# a backward branch into them would otherwise resolve to an empty FUN_xxxx
# stub + return (silently dropping the loop).  The live instance is FUN_9d6f:
# the 8-byte normalization loop at $9D67-$9D6E was orphaned before the $9D6F
# entry, breaking the terrain projection divide.
#
# Fix: find each maximal run of contiguous orphan instructions that FALLS
# THROUGH (no terminator) directly into a known function's start, and prepend
# that run to the function as a body prefix.  The function's named entry stays
# at its original start (callers still enter there); translate_func emits a
# `goto L_<entry>` so a normal call skips the prefix, while an internal branch
# into the prefix becomes a local `goto`.
# ---------------------------------------------------------------------------
def attach_orphan_runs(all_insns, funcs, func_ranges):
    insns_sorted = sorted(all_insns, key=lambda i: i['addr'])

    def contained(addr):
        for s, e, n in func_ranges:
            if s <= addr <= e:
                return True
        return False

    funcs_by_start = {f['start']: f for f in funcs}
    orphans = [i for i in insns_sorted if not contained(i['addr'])]
    if not orphans:
        return

    # Group into maximal contiguous runs (each insn abuts the next).
    runs = []
    cur = [orphans[0]]
    for prev, nxt in zip(orphans, orphans[1:]):
        if prev['addr'] + len(prev['bytes']) == nxt['addr']:
            cur.append(nxt)
        else:
            runs.append(cur); cur = [nxt]
    runs.append(cur)

    TERMINATORS = {'RTS', 'RTI', 'JMP', 'BRK'}
    for run in runs:
        last  = run[-1]
        after = last['addr'] + len(last['bytes'])
        lo, hi = run[0]['addr'], last['addr']
        if last['mnem'] in TERMINATORS:
            print(f'[orphan] run ${lo:04X}-${hi:04X} ends in {last["mnem"]} '
                  f'(no fall-through) — left as-is')
            continue
        tgt = funcs_by_start.get(after)
        if tgt is None:
            print(f'[orphan] run ${lo:04X}-${hi:04X} falls through to ${after:04X} '
                  f'(not a function start) — left as-is')
            continue
        tgt['insns']      = run + tgt['insns']
        tgt['body_start'] = lo
        tgt['skip_to']    = tgt['start']   # named entry; prefix runs above it
        print(f'[orphan] attached run ${lo:04X}-${hi:04X} as prefix of '
              f'{tgt["name"]} (entry ${tgt["start"]:04X})')

# ---------------------------------------------------------------------------
# Parse listing into a list of functions, each with instructions.
# Returns:
#   funcs: list of {start, end, name, insns: [{addr, bytes, mnem, op, raw}]}
#   func_by_addr: addr → function index
# ---------------------------------------------------------------------------
def parse_listing(path, symbols):
    funcs = []
    func_ranges = []   # (start, end) from function-summary header

    # Pass 1: collect function ranges from the header summary.
    with open(path) as f:
        for line in f:
            m = re.match(r'^; FUNC (\S+)\s+([0-9a-f]+) - ([0-9a-f]+)', line)
            if m:
                name  = m.group(1)
                start = int(m.group(2), 16)
                end   = int(m.group(3), 16)
                func_ranges.append((start, end, name))

    # Build addr → (start, end, name) lookup.
    func_ranges.sort()
    func_start_set = {r[0] for r in func_ranges}
    func_info = {r[0]: r for r in func_ranges}

    # Pass 2: parse instructions, grouped into functions by address range.
    insn_re = re.compile(
        r'^([0-9a-f]{4})\s+((?:[0-9A-F]{2} )*[0-9A-F]{2})\s+'
        r'([A-Z]{2,3})\s*(.*?)\s*$'
    )

    current_func = None
    func_insns   = {}   # start_addr → list of insn dicts
    all_insns    = []   # every decoded instruction, regardless of function

    with open(path) as f:
        for line in f:
            m = insn_re.match(line)
            if not m: continue
            addr  = int(m.group(1), 16)
            bs    = bytes(int(x, 16) for x in m.group(2).split())
            mnem  = m.group(3)
            op    = m.group(4).strip()
            ins   = {'addr': addr, 'bytes': bs, 'mnem': mnem, 'op': op}
            all_insns.append(ins)

            # Determine enclosing function by address range.
            fn_start = None
            for (s, e, n) in func_ranges:
                if s <= addr <= e:
                    fn_start = s
                    break
            if fn_start is None:
                continue

            if fn_start not in func_insns:
                func_insns[fn_start] = []
            func_insns[fn_start].append(ins)

    # Assemble final list in address order.
    for (start, end, name) in func_ranges:
        insns = func_insns.get(start, [])
        if not insns: continue
        # Override name from symbols if present.
        final_name = symbols.get(start, name)
        funcs.append({'start': start, 'end': end,
                      'name': final_name, 'insns': insns})

    # Attach orphan instruction runs (code Ghidra left in inter-function gaps)
    # to the function they fall through into.  See attach_orphan_runs.
    attach_orphan_runs(all_insns, funcs, func_ranges)

    # func_by_addr: addr → function dict
    func_by_addr = {}
    for f in funcs:
        for ins in f['insns']:
            func_by_addr[ins['addr']] = f

    return funcs, func_by_addr

# ---------------------------------------------------------------------------
# Operand → C expression
# ---------------------------------------------------------------------------
def is_hw(addr):
    return HW_BASE <= addr < HW_END

def addr_read(addr):
    if is_hw(addr):
        return f'bus_read(0x{addr:04X})'
    return f'mem[0x{addr:04X}]'

def addr_write(addr, val):
    if is_hw(addr):
        return f'bus_write(0x{addr:04X}, {val})'
    return f'mem[0x{addr:04X}] = {val}'

def parse_operand(op, nbytes, symbols):
    """Return (mode, addr_or_imm, index) where mode is one of:
       imm, zp, abs, zpx, zpy, absx, absy, indy, indx, acc, impl
    """
    op = op.strip()
    if not op:
        return ('impl', 0, None)

    # Immediate: #0x12
    m = re.match(r'^#0x([0-9a-fA-F]+)$', op)
    if m:
        return ('imm', int(m.group(1), 16), None)

    # (zp),Y  post-indexed
    m = re.match(r'^\(0x([0-9a-fA-F]+)\),Y$', op)
    if m:
        return ('indy', int(m.group(1), 16), 'Y')

    # (zp,X)  pre-indexed
    m = re.match(r'^\(0x([0-9a-fA-F]+),X\)$', op)
    if m:
        return ('indx', int(m.group(1), 16), 'X')

    # (abs) or (zp)  — absolute/ZP indirect, used by JMP (DLI chain pattern)
    m = re.match(r'^\(0x([0-9a-fA-F]+)\)$', op)
    if m:
        return ('jmpind', int(m.group(1), 16), None)

    # abs,X or abs,Y or zp,X or zp,Y
    m = re.match(r'^0x([0-9a-fA-F]+),([XY])$', op)
    if m:
        addr = int(m.group(1), 16)
        idx  = m.group(2)
        mode = ('zpx' if nbytes == 2 and idx=='X' else
                'zpy' if nbytes == 2 and idx=='Y' else
                'absx' if idx=='X' else 'absy')
        return (mode, addr, idx)

    # abs or zp (bare address)
    m = re.match(r'^0x([0-9a-fA-F]+)$', op)
    if m:
        addr = int(m.group(1), 16)
        mode = 'zp' if nbytes == 2 else 'abs'
        return (mode, addr, None)

    # Accumulator (e.g. "ASL A" in some disassemblers)
    if op == 'A':
        return ('acc', 0, None)

    return ('impl', 0, None)

def operand_read(mode, addr, idx):
    """C expression that reads the source value."""
    if mode == 'imm':   return f'0x{addr:02X}'
    if mode in ('zp','abs'):
        return addr_read(addr)
    if mode == 'zpx':   return f'mem[(uint8_t)(0x{addr:02X}+cpu.X)]'
    if mode == 'zpy':   return f'mem[(uint8_t)(0x{addr:02X}+cpu.Y)]'
    if mode == 'absx':  return addr_read(addr) if not is_hw(addr) else f'bus_read(0x{addr:04X}+cpu.X)'
    # For abs,X with non-HW: mem[0xXXXX + cpu.X] — no wrapping for abs indexed
    if mode == 'absx':  return f'mem[0x{addr:04X}+cpu.X]'
    if mode == 'absy':  return f'mem[0x{addr:04X}+cpu.Y]'
    if mode == 'indy':  return f'bus_read(ZP_IND_Y(0x{addr:02X}))'
    if mode == 'indx':  return f'bus_read(ZP_IND_X(0x{addr:02X}))'
    return '0'

def operand_read_fixed(mode, addr, idx):
    """Like operand_read but non-HW abs uses mem[] directly.
    Parenthesise the hex constant to avoid the C preprocessor E+/P+
    tokenisation quirk (pp-numbers include 'E+' and 'E-' sequences)."""
    if mode == 'absx':  return f'mem[(0x{addr:04X})+cpu.X]'
    if mode == 'absy':  return f'mem[(0x{addr:04X})+cpu.Y]'
    return operand_read(mode, addr, idx)

def operand_addr_expr(mode, addr, idx):
    """C expression giving the effective address (for write targets).
    Parenthesise hex constants to avoid the C preprocessor E+/P+ quirk."""
    if mode in ('zp','abs'):  return f'0x{addr:04X}'
    if mode == 'zpx':  return f'(uint8_t)((0x{addr:02X})+cpu.X)'
    if mode == 'zpy':  return f'(uint8_t)((0x{addr:02X})+cpu.Y)'
    if mode == 'absx': return f'(0x{addr:04X})+cpu.X'
    if mode == 'absy': return f'(0x{addr:04X})+cpu.Y'
    if mode == 'indy': return f'ZP_IND_Y(0x{addr:02X})'
    if mode == 'indx': return f'ZP_IND_X(0x{addr:02X})'
    return '0'

def needs_bus_write(addr):
    """True for addresses that must go through bus_write() so the
    platform layer is notified.  Hardware ($D000-$D7FF) is always
    routed.  OS page-2 shadow registers ($0200-$02FF) must also go
    through bus_write so platform_shadow_write() is called — without
    this, writes to VVBLKI ($0222/$0223), VDSLST ($0200/$0201),
    SDMCTL ($022F), SDLSTL/H ($0230/$0231) etc. are silent and the
    VBI/DLI handler dispatch never updates."""
    return is_hw(addr) or (0x0200 <= addr < 0x0300)

def write_expr(mode, addr, idx, val_expr):
    ea = operand_addr_expr(mode, addr, idx)
    if mode in ('zp','abs') and needs_bus_write(addr):
        return f'bus_write(0x{addr:04X}, {val_expr})'
    if mode in ('zp','abs'):
        return f'mem[0x{addr:04X}] = {val_expr}'
    if mode in ('absx','absy','zpx','zpy'):
        return f'mem[{ea}] = {val_expr}'
    # indirect modes
    return f'bus_write({ea}, {val_expr})'

# ---------------------------------------------------------------------------
# Translate one instruction to C statement(s)
# ---------------------------------------------------------------------------
BRANCH_FLAGS = {
    'BEQ': 'cpu.Z', 'BNE': '!cpu.Z',
    'BCS': 'cpu.C', 'BCC': '!cpu.C',
    'BMI': 'cpu.N', 'BPL': '!cpu.N',
    'BVS': 'cpu.V', 'BVC': '!cpu.V',
}

def translate_insn(insn, func, all_funcs_by_start, symbols, local_targets,
                   external_entries=None, wrapper_names=None):
    """external_entries: {addr → container_func} from main pass-1 analysis.
    wrapper_names:      {addr → C name} for mid-function entry wrappers."""
    if external_entries is None:  external_entries = {}
    if wrapper_names   is None:  wrapper_names    = {}

    addr  = insn['addr']
    mnem  = insn['mnem']
    op    = insn['op']
    nbytes = len(insn['bytes'])
    mode, val, idx = parse_operand(op, nbytes, symbols)

    lines = [f'    /* {addr:04x} */']

    def resolve_target_name(target):
        """Return the C name to call for a branch/JMP to target.
        Checks: local function start, wrapper, symbol, fallback."""
        if target in wrapper_names:
            return wrapper_names[target]
        if target in all_funcs_by_start:
            return all_funcs_by_start[target]['name']
        return symbols.get(target, f'FUN_{target:04x}')

    # --- Branches ---
    if mnem in BRANCH_FLAGS:
        target = val
        flag   = BRANCH_FLAGS[mnem]
        if target in local_targets:
            # Hooks are injected at the label definition; don't duplicate here.
            lines.append(f'    if ({flag}) goto L_{target:04x};')
        else:
            # Cross-function branch → conditional tail call.
            name = resolve_target_name(target)
            lines.append(f'    if ({flag}) {{ {name}(); return; }}')
        return lines

    # --- JMP ---
    if mnem == 'JMP':
        if mode == 'jmpind':
            # Indirect JMP via ZP pointer (DLI chain: JMP ($E0) etc.)
            # Dereference ZP at runtime and dispatch via the VBI/DLI table.
            lines.append(f'    {{ uint16_t _t = (uint16_t)(mem[0x{val:04X}] | '
                         f'((uint16_t)mem[0x{(val+1)&0xFF:04X}] << 8)); '
                         f'platform_indirect_jmp(_t); return; }}')
        else:
            target = val
            if target in local_targets:
                lines.append(f'    goto L_{target:04x};')
            else:
                name = resolve_target_name(target)
                lines.append(f'    {name}(); return;')
        return lines

    # --- JSR ---
    if mnem == 'JSR':
        target = val
        name = resolve_target_name(target)
        lines.append(f'    {name}();')
        return lines

    # --- RTS / RTI ---
    if mnem == 'RTS':
        lines.append('    return;')
        return lines
    if mnem == 'RTI':
        lines.append('    PLP(); return;')
        return lines

    # --- NOP / BRK ---
    if mnem == 'NOP':
        lines.append('    NOP();')
        return lines
    if mnem == 'BRK':
        lines.append('    /* BRK: software interrupt — ignored in C translation */;')
        return lines

    # --- Flag ops ---
    simple_flag = {'CLC':'CLC()','SEC':'SEC()','CLI':'CLI()','SEI':'SEI()',
                   'CLD':'CLD()','SED':'SED()','CLV':'CLV()'}
    if mnem in simple_flag:
        lines.append(f'    {simple_flag[mnem]};')
        return lines

    # --- Stack ---
    if mnem == 'PHA': lines.append('    PHA();'); return lines
    if mnem == 'PLA': lines.append('    PLA();'); return lines
    if mnem == 'PHP': lines.append('    PHP();'); return lines
    if mnem == 'PLP': lines.append('    PLP();'); return lines

    # --- Transfer ---
    for mn, mac in [('TAX','TAX()'),('TAY','TAY()'),('TXA','TXA()'),('TYA','TYA()'),
                    ('TSX','TSX()'),('TXS','TXS()'),
                    ('INX','INX()'),('INY','INY()'),('DEX','DEX()'),('DEY','DEY()')]:
        if mnem == mn:
            lines.append(f'    {mac};')
            return lines

    # --- Load ---
    if mnem in ('LDA','LDX','LDY'):
        reg = mnem[2]  # A, X, or Y
        src = operand_read_fixed(mode, val, idx)
        lines.append(f'    LD{reg}({src});')
        return lines

    # --- Store ---
    if mnem in ('STA','STX','STY'):
        reg = mnem[2]
        stmt = write_expr(mode, val, idx, f'cpu.{reg}')
        lines.append(f'    {stmt};')
        return lines

    # --- ADC / SBC ---
    if mnem == 'ADC':
        src = operand_read_fixed(mode, val, idx)
        lines.append(f'    ADC({src});')
        return lines
    if mnem == 'SBC':
        src = operand_read_fixed(mode, val, idx)
        lines.append(f'    SBC({src});')
        return lines

    # --- Compare ---
    cmp_map = {'CMP': 'CMP', 'CPX': 'CPX', 'CPY': 'CPY'}
    if mnem in cmp_map:
        src = operand_read_fixed(mode, val, idx)
        lines.append(f'    {cmp_map[mnem]}({src});')
        return lines

    # --- Logical ---
    for mn, mac in [('AND','AND'),('ORA','ORA'),('EOR','EOR')]:
        if mnem == mn:
            src = operand_read_fixed(mode, val, idx)
            lines.append(f'    {mac}({src});')
            return lines

    # --- BIT ---
    if mnem == 'BIT':
        src = operand_read_fixed(mode, val, idx)
        lines.append(f'    BIT({src});')
        return lines

    # --- INC / DEC memory ---
    if mnem == 'INC':
        ea = operand_addr_expr(mode, val, idx)
        lines.append(f'    INC_M({ea});')
        return lines
    if mnem == 'DEC':
        ea = operand_addr_expr(mode, val, idx)
        lines.append(f'    DEC_M({ea});')
        return lines

    # --- Shift / Rotate ---
    if mnem == 'ASL':
        if mode == 'impl' or mode == 'acc':
            lines.append('    ASL_A();')
        else:
            ea = operand_addr_expr(mode, val, idx)
            lines.append(f'    ASL_M({ea});')
        return lines
    if mnem == 'LSR':
        if mode == 'impl' or mode == 'acc':
            lines.append('    LSR_A();')
        else:
            ea = operand_addr_expr(mode, val, idx)
            lines.append(f'    LSR_M({ea});')
        return lines
    if mnem == 'ROL':
        if mode == 'impl' or mode == 'acc':
            lines.append('    ROL_A();')
        else:
            ea = operand_addr_expr(mode, val, idx)
            lines.append(f'    ROL_M({ea});')
        return lines
    if mnem == 'ROR':
        if mode == 'impl' or mode == 'acc':
            lines.append('    ROR_A();')
        else:
            ea = operand_addr_expr(mode, val, idx)
            lines.append(f'    ROR_M({ea});')
        return lines

    # --- Unknown ---
    lines.append(f'    /* TODO: {mnem} {op} */')
    return lines

# ---------------------------------------------------------------------------
# Translate one function
# ---------------------------------------------------------------------------
def translate_func(func, all_funcs_by_start, symbols,
                   external_entry_labels=None,
                   external_entries=None, wrapper_names=None):
    """Translate one 6502 function to C.

    external_entry_labels: set of addresses within this function that are
    entered from outside (e.g. JSR/JMP to a mid-body address).  These are
    used as split-points: the function body stops at each one and emits a
    tail-call to the corresponding wrapper, so that external callers can
    invoke the correct code slice directly.  They are NOT emitted as goto
    labels in the container — instead they become function calls.
    """
    start = func['start']
    name  = func['name']
    insns = func['insns']
    if external_entry_labels is None: external_entry_labels = set()
    if external_entries       is None: external_entries      = {}
    if wrapper_names           is None: wrapper_names         = {}

    if start in MANUAL_FUNCS:
        return [f'/* {name} @ ${start:04X}: manual implementation in rof_manual.c */']

    func_end = func['end']
    # body_lo: lowest body address (start, or lower if an orphan prefix was
    # absorbed).  skip_to: the named entry to jump to past the prefix (None
    # when there is no prefix).
    body_lo = func.get('body_start', start)
    skip_to = func.get('skip_to')
    # local_targets: branch/JMP targets that stay in this function's body.
    # Exclude external entry labels — those become tail-calls, not gotos.
    # Also exclude targets at or beyond the first split point: those are now
    # in a different C function and cannot be reached via goto.
    first_split = min(external_entry_labels) if external_entry_labels else None
    local_targets = set()
    for insn in insns:
        mnem, op, nbytes = insn['mnem'], insn['op'], len(insn['bytes'])
        if mnem in BRANCH_FLAGS or mnem == 'JMP':
            mode, val, idx = parse_operand(op, nbytes, symbols)
            if body_lo <= val <= func_end and val not in external_entry_labels:
                if first_split is None or val < first_split:
                    local_targets.add(val)
    # The named entry needs an L_ label so the prefix-skipping goto can reach it.
    if skip_to is not None:
        local_targets.add(skip_to)

    TERMINATORS = {'RTS', 'RTI', 'JMP', 'BRK'}

    lines = []
    # Emit the symbol's note (symbols.csv col 5) as a doc-comment, plus the
    # original 6502 entry address for provenance (Phase 5 traceability).
    note = SYMBOL_NOTES.get(start)
    if note:
        lines.append(f'/* {name} @ ${start:04X}: {note} */')
    # When validating a native reimplementation, define the transliterated body
    # under the `__t6502` reference name; the plain name is the native version.
    def_name = name + VALIDATE_SUFFIX if start in VALIDATE_FUNCS else name
    if start in VALIDATE_FUNCS:
        lines.append(f'/* faithful transliteration kept as the validation oracle; '
                     f'native {name}() lives in rof_native.c (see VALIDATE_FUNCS) */')
    lines.append(f'void {def_name}(void) {{')
    # Orphan-prefix functions: the named entry is mid-body, so callers must
    # skip the prefix (which is reachable only via an internal backward branch).
    if skip_to is not None:
        lines.append(f'    goto L_{skip_to:04x};  /* enter past orphan-prefix loop body */')
    hit_split = False
    last_insn = None
    for insn in insns:
        addr = insn['addr']
        # Split point: stop the current function and tail-call the wrapper.
        if addr in external_entry_labels:
            wname = wrapper_names.get(addr, f'FUN_{addr:04x}')
            lines.append(f'    {wname}(); return;')
            hit_split = True
            break   # do not translate addr or any subsequent instructions
        if addr in local_targets:
            # Inject spin-wait hook at the label itself so ALL paths
            # reaching this label (including unconditional JMPs) get it.
            hook = SPINWAIT_HOOKS.get(addr, '')
            if hook:
                lines.append(f'L_{addr:04x}:; {hook}')
            else:
                lines.append(f'L_{addr:04x}:;')
        stmt_lines = translate_insn(insn, func, all_funcs_by_start, symbols,
                                    local_targets, external_entries, wrapper_names)
        lines.extend(stmt_lines)
        last_insn = insn

    # Fall-through detection: if the function did not end with a terminator
    # (RTS/RTI/JMP/BRK) and was not cut by a split point, the 6502 code
    # falls through into the next function.  Emit a tail-call to it.
    if not hit_split and last_insn is not None:
        last_mnem = last_insn['mnem']
        if last_mnem not in TERMINATORS:
            next_addr = last_insn['addr'] + len(last_insn['bytes'])
            # Prefer a known function start; else check wrapper table.
            next_func = all_funcs_by_start.get(next_addr)
            if next_func and next_func['start'] not in MANUAL_FUNCS:
                lines.append(f'    {next_func["name"]}(); return;')
            elif next_addr in wrapper_names:
                lines.append(f'    {wrapper_names[next_addr]}(); return;')
            elif next_func:
                next_name = symbols.get(next_addr, f'FUN_{next_addr:04x}')
                lines.append(f'    {next_name}(); return;')

    lines.append('}')
    lines.append('')
    return lines

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
def find_containing_func(addr, funcs):
    """Return the function whose address range contains addr, or None.
    Uses func_lo so absorbed orphan prefixes count as part of the body."""
    for f in funcs:
        if func_lo(f) <= addr <= f['end']:
            return f
    return None

def main():
    symbols = load_symbols(SYM_CSV)
    funcs, func_by_addr = parse_listing(LISTING, symbols)
    funcs_by_start = {f['start']: f for f in funcs}

    print(f'Parsed {len(funcs)} functions, '
          f'{sum(len(f["insns"]) for f in funcs)} instructions')

    # -----------------------------------------------------------------------
    # Pass 1: collect cross-function branch/JMP targets.
    # For each branch/JMP whose target T is NOT in the current function's
    # address range but IS within another function's range (and not at that
    # function's start), record T as an "external entry" needing a wrapper.
    # -----------------------------------------------------------------------
    # func_addr_ranges: list of (start, end) for range containment tests.
    func_addr_ranges = [(f['start'], f['end'], f) for f in funcs]

    # external_entries: target_addr → containing_func
    external_entries = {}
    # external_labels_for_func: func_start → set of addr needing L_xxxx labels
    external_labels_for_func = defaultdict(set)

    for func in funcs:
        for insn in func['insns']:
            mnem, op, nbytes = insn['mnem'], insn['op'], len(insn['bytes'])
            if mnem not in BRANCH_FLAGS and mnem not in ('JMP', 'JSR'):
                continue
            mode, val, _ = parse_operand(op, nbytes, symbols)
            if val == 0:
                continue
            # Is target within THIS function's range (incl. orphan prefix)?
            if func_lo(func) <= val <= func['end']:
                continue
            # Target is outside. Find which function contains it.
            container = find_containing_func(val, funcs)
            if container is None:
                continue
            if val == container['start']:
                continue  # it's a normal tail call to another function's start
            # Mid-function entry: needs a wrapper and a label in the container.
            external_entries[val] = container
            external_labels_for_func[container['start']].add(val)

    wrapper_names = {}  # target_addr → wrapper function name
    for addr, container in external_entries.items():
        wname = symbols.get(addr, f'FUN_{addr:04x}')
        wrapper_names[addr] = wname

    # -----------------------------------------------------------------------
    # Cascading split fixpoint.
    # When a function is split at address S, code in any segment before S
    # may have branches/JMPs that target addresses inside the post-split
    # tail (i.e. >= S within the same container).  Those target addresses
    # also need to become split functions so they can be called by name.
    # Iterate until no new entries are discovered.
    # -----------------------------------------------------------------------
    def segments_for_container(c):
        """Return [(seg_start, seg_end)] for all segments of container c."""
        splits = sorted(
            a for a, cont in external_entries.items() if cont['start'] == c['start']
        )
        # First segment starts at func_lo so an absorbed orphan prefix counts
        # as part of segment 0 (a backward branch into it is intra-segment, not
        # a spurious cross-segment split).
        starts = [func_lo(c)] + splits
        ends   = splits + [c['end'] + 1]
        return list(zip(starts, ends))

    changed = True
    while changed:
        changed = False
        for container in funcs:
            for seg_start, seg_end in segments_for_container(container):
                for insn in container['insns']:
                    if insn['addr'] < seg_start:
                        continue
                    if insn['addr'] >= seg_end:
                        break
                    mnem, op, nbytes = insn['mnem'], insn['op'], len(insn['bytes'])
                    if mnem not in BRANCH_FLAGS and mnem != 'JMP':
                        continue
                    mode, target, _ = parse_operand(op, nbytes, symbols)
                    if target <= 0:
                        continue
                    # Branch/JMP from within this segment to AFTER this segment
                    # but still within the container — needs a new split point.
                    # Branch crosses segment boundary: target is in the container's
                    # range but outside the current segment (forward OR backward).
                    # Skip targets that are already Ghidra function starts — those
                    # are handled as normal tail-calls, not split wrappers.
                    in_container = func_lo(container) <= target <= container['end']
                    in_segment   = seg_start <= target < seg_end
                    already_func = target in funcs_by_start
                    if in_container and not in_segment and not already_func and target not in external_entries:
                        external_entries[target] = container
                        external_labels_for_func[container['start']].add(target)
                        wname = symbols.get(target, f'FUN_{target:04x}')
                        wrapper_names[target] = wname
                        changed = True

    # -----------------------------------------------------------------------
    # Collect all branch/JMP/JSR targets that fall in no function range.
    # Ghidra sometimes misses functions for gap addresses. Generate stubs.
    # -----------------------------------------------------------------------
    jsr_targets_unknown = set()
    all_call_mnems = set(BRANCH_FLAGS.keys()) | {'JMP', 'JSR'}
    for func in funcs:
        for insn in func['insns']:
            if insn['mnem'] not in all_call_mnems: continue
            mode, val, _ = parse_operand(insn['op'], len(insn['bytes']), symbols)
            # Indirect JMP (e.g. JMP ($E0)) is emitted as platform_indirect_jmp;
            # the pointer address ($00E0) is data, not a call target — don't stub it.
            if mode == 'jmpind': continue
            if val == 0: continue
            if val in funcs_by_start: continue
            if val in external_entries: continue
            if val in wrapper_names: continue
            # Check if it falls in any function range
            if find_containing_func(val, funcs) is not None: continue
            jsr_targets_unknown.add(val)

    # -----------------------------------------------------------------------
    # Forward declarations header — includes wrapper function names.
    # -----------------------------------------------------------------------
    decl_lines = [
        '#ifndef ROF_DECL_H', '#define ROF_DECL_H',
        '/* Auto-generated by tools/transpile.py — do not edit */',
        '#include <stdint.h>', '',
        '/* Forward declarations for all 6502 routines */',
    ]
    for f in funcs:
        decl_lines.append(f'void {f["name"]}(void);')
        # Validated funcs: the plain name (declared above) is the native version
        # in rof_native.c; also declare the transliterated reference twin.
        if f['start'] in VALIDATE_FUNCS:
            decl_lines.append(f'void {f["name"]}{VALIDATE_SUFFIX}(void);')
    # Wrappers for mid-function entry points.
    decl_lines.append('')
    decl_lines.append('/* Wrappers for cross-function branch/JMP entry points */')
    for addr, wname in sorted(wrapper_names.items()):
        decl_lines.append(f'void {wname}(void);')
        # A validated mid-function entry: its split body is emitted under the
        # `__t6502` twin (translate_func, def_name) with the plain name native;
        # declare the twin too so the validation harness can reach it.
        if addr in VALIDATE_FUNCS:
            decl_lines.append(f'void {wname}{VALIDATE_SUFFIX}(void);')
    # Stubs for unlisted JSR targets.
    decl_lines.append('')
    decl_lines.append('/* Stubs for JSR targets without a Ghidra-detected function */')
    for addr in sorted(jsr_targets_unknown):
        name = symbols.get(addr, f'FUN_{addr:04x}')
        decl_lines.append(f'void {name}(void);')
    decl_lines += ['', '#endif /* ROF_DECL_H */']
    OUT_H.write_text('\n'.join(decl_lines) + '\n')
    print(f'Wrote {OUT_H}')

    # -----------------------------------------------------------------------
    # Pass 2: generate C.
    # -----------------------------------------------------------------------
    header = [
        '/* Auto-generated by tools/transpile.py — do not edit */',
        '#include "../cpu/cpu.h"',
        '#include "../cpu/bus.h"',
        '#include "rof_decl.h"',
        '#include "../platform/platform_c.h"',
        '',
    ]
    body = []
    for f in funcs:
        ext_labels = external_labels_for_func.get(f['start'], set())
        body.extend(translate_func(f, funcs_by_start, symbols,
                                   external_entry_labels=ext_labels,
                                   external_entries=external_entries,
                                   wrapper_names=wrapper_names))

    # Stubs for JSR targets Ghidra didn't create functions for.
    if jsr_targets_unknown:
        body.append('/* === Stubs for JSR targets without a known function === */')
        body.append('/* TODO: investigate each — may be data misidentified as code. */')
        for addr in sorted(jsr_targets_unknown):
            name = symbols.get(addr, f'FUN_{addr:04x}')
            body.append(f'void {name}(void) {{ /* stub: no instructions found at ${addr:04X} */ }}')
        body.append('')

    # Emit split functions for mid-function entry points.
    # Each wrapper is the code from the entry address to the end of its
    # container function — a faithful slice, not an approximation.
    body.append('/* === Split functions for cross-function entry points === */')
    body.append('/* Each function starts at the labelled address inside its container. */')
    for addr in sorted(external_entries):
        container  = external_entries[addr]
        wname      = wrapper_names[addr]

        # Build an instruction index for the container.
        insns = container['insns']
        addr_to_idx = {insn['addr']: i for i, insn in enumerate(insns)}
        start_idx   = addr_to_idx.get(addr)

        if start_idx is None:
            # Address not found as an instruction start — emit a plain comment.
            body.append(f'void {wname}(void) {{')
            body.append(f'    /* entry ${addr:04X} not found in {container["name"]} */')
            body.append(f'}}')
            body.append('')
            continue

        # Build a synthetic function dict for the slice.
        sliced_insns = insns[start_idx:]
        sliced_func  = {
            'start': addr,
            'end':   container['end'],
            'name':  wname,
            'insns': sliced_insns,
        }

        # External entry labels within the slice (strictly after addr).
        slice_ext_labels = {
            a for a in external_labels_for_func.get(container['start'], set())
            if a > addr
        }

        body.extend(translate_func(sliced_func, funcs_by_start, symbols,
                                   external_entry_labels=slice_ext_labels,
                                   external_entries=external_entries,
                                   wrapper_names=wrapper_names))

    OUT_C.write_text('\n'.join(header + body) + '\n')
    print(f'Wrote {OUT_C}  ({len(header)+len(body)} lines)')

    # -----------------------------------------------------------------------
    # Manual implementations stub (only written once).
    # -----------------------------------------------------------------------
    if not OUT_MAN.exists():
        manual = [
            '/* Hand-written implementations for self-modifying / special-case routines.',
            '   These are NOT auto-generated; edit this file freely. */',
            '#include "../cpu/cpu.h"',
            '#include "../cpu/bus.h"',
            '#include "rof_decl.h"',
            '#include <string.h>',
            '',
            '/* screen_page_swap ($1A62): swaps 5 x 256-byte pages between $40xx and $06xx.',
            '   The original code is self-modifying: it patches the high bytes of its own',
            '   LDA/STA instructions to cycle through pages $40-$44 and $06-$0A.',
            '   The semantics are straightforward so we translate the intent directly. */',
            'void screen_page_swap(void) {',
            '    int page;',
            '    for (page = 0; page < 5; page++) {',
            '        uint8_t *a = mem + ((0x40 + page) << 8);',
            '        uint8_t *b = mem + ((0x06 + page) << 8);',
            '        uint8_t tmp[256];',
            '        memcpy(tmp, a,   256);',
            '        memcpy(a,   b,   256);',
            '        memcpy(b,   tmp, 256);',
            '    }',
            '}',
        ]
        OUT_MAN.write_text('\n'.join(manual) + '\n')
        print(f'Wrote {OUT_MAN}  (manual stubs)')
    else:
        print(f'Skipped {OUT_MAN}  (already exists)')

if __name__ == '__main__':
    main()
