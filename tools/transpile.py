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
OUT_MEM  = ROOT / "src/gen/mem.h"

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
    0x40B0,  # draw_ah_ground_fill_p2 — AH ground-fill column (flight HUD draw, cache-gated) [native]
    0x40E5,  # draw_altimeter_bars — altimeter terrain/ship bar columns (flight HUD draw, cache-gated) [native]
    0x43C7,  # dispatch_43cb_half_70 — Y=terrain_clearance>>1, tail draw_dial_bar_column (flight HUD draw) [native]
    0x44D6,  # update_altitude_digit_display — altitude digit glyph + colour (flight HUD draw, cache-gated) [native]
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
    0xAB9A,  # raster_scaled_object — nested 12x32 cell fill via terrain_clip_row_top (flight raster #3)
    0xA822,  # terrain_plot_object_a — plot one terrain object, variant A (flight raster #4)
    0xA90A,  # terrain_plot_object_b — plot one terrain object, variant B (flight raster #5)
    0xA63B,  # terrain_plot_object — per-object raster dispatch (flight raster #6)
    0xB33D,  # terrain_column_rasterize — fractal column renderer (flight raster #7, the big one)
    0xB172,  # terrain_subdivide_column — fractal subdivision driver (flight raster #8)
    0xA613,  # terrain_jitter_column — per-frame random terrain jitter (flight top #1)
    0x9E54,  # terrain_frame_setup — terrain gen step 1: view setup + per-column transform (flight top #2)
    0xA11F,  # project_terrain_points — per-object world->screen projection via divide_16x16 (flight top #3)
    0xAE53,  # fill_terrain_silhouette — per-column surface scan + sky/body fill (NOT collision; flight top #4)
    0xA31E,  # terrain_draw_frame — main per-frame terrain driver (flight top #5, the last)
    0x4FF5,  # vbi_handler_flight — the in-flight VBI handler itself (orchestrates the whole flight frame)
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
    0x7AB8,  # alien_attack_tick — enemy PMG update (RANDOM; tail ring_push_marked + jitter_roll_pitch)
    0x3FCD,  # enemy_check — event dispatch ($063D->game_sub_4f3f [transpiled] / $0633->alien_attack_tick)
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
    0x581C,  # sfx_event_load — load a new voice from event tables $56D4..$57F4 (stack-aware; tail game_sub_55FC)
    0x548D,  # sfx_voice_envelope_tick — APEX: per-frame voice/gauge envelope engine + ring drain (Atari VBI tail)
    # --- startup/cinematic de-transpile (2026-06-15): the boot_standby_launch_driver ($5F1D)
    #     subtree that drives the Standby + Doors/Tunnel/Planet cinematic — the
    #     slow part of boot (the flight loop is already native).  Leaves-first. ---
    0x4E84,  # bin_to_bcd — A(0-99)->packed BCD; units->$00C1, tens->Y, BCD->A (pure leaf)
    0x782A,  # copy_title_text_block_to_screen — Standby per-frame: copy 20-byte $5A9F+X block to $32B7..$32CA (leaf, entry Y)
    0x6DDF,  # init_row_coords_9c — store 5 constants into $009C-$00A0 (pure leaf)
    0x6B71,  # clear_scroll_accum — zero $02C0-$02C3, $00A1-$00A5 (pure leaf)
    0x75A5,  # copy_192_to_1800 — copy 192 bytes $350C->$1810..$18CF via $BB/$BC ptr (leaf)
    0x7460,  # build_row_addr_table — build 85-entry $073D/$0793 addr table from $C3:$C4 base + $C1 stride (leaf)
    0x65DF,  # build_line_addr_table_2000 — set base $2000/stride $2E, tail build_row_addr_table
    0x65D2,  # build_line_addr_table_1000_stride — set base $1000/stride=A, tail build_row_addr_table (entry A)
    0x65D0,  # build_line_addr_table_1000 — A=$2E, tail build_line_addr_table_1000_stride
    0x6B85,  # init_object_positions — zero $08D1-$08D3, build 22-entry word array $08A4/$08A5 = $6E2D table + $2EE0 (pure leaf)
    0x712D,  # audio_timer_setup — zero $00E7/$0655/$00E5 + POKEY timers $D201-$D207, AUDCTL=$60 (leaf; POKEY via bus_write)
    0x7148,  # sfx_seq_step — advance SFX theme sequencer $073C through $71DB; load AUDF/AUDC presets -> POKEY; set $073A/$073B
    0x70F9,  # sfx_voice_tick — tick SFX theme: count down $073A (sfx_seq_step on underflow), emit ramped AUDC to voices 1-3
    0x6B47,  # random_terrain_height — 2 RANDOM reads -> sparse height (result in A; seeded-LFSR test)
    0x665D,  # fill_horizontal_span — write pattern $00B9 across a row span into two row ptrs (leaf)
    0x66DE,  # plot_masked_pixel — OR/AND a 2-bit pixel into ($80)+Y via mask tbls $66E9/$66FB (leaf, entry X/Y)
    0x66D5,  # plot_pixel_masked — A=col -> Y/X mask index, tail plot_masked_pixel (entry A)
    0x66C8,  # set_row_ptr — $80/$81 = addr-table[Y] (leaf, entry Y)
    0x66C6,  # set_row_ptr_from_count — Y=$0092, tail set_row_ptr
    0x669C,  # fill_vertical_span — per-row masked plot of cols $9C/$9D via plot_pixel_masked/plot_masked_pixel
    0x6C92,  # plot_pixel_2bpp — 4x ROL 2bpp pack of ($80)+Y, mask via BIT $0082 (leaf, entry Y+carry; preserves X)
    0x6642,  # draw_symmetric_span_loop — $0096x {fill_horizontal_span + fill_vertical_span}, steps coords $9C-$9F
    0x6B2E,  # gen_terrain_column — fill one column (Y) of 4 buffers $0C32/$0D32/$0E32/$0F32 via random_terrain_height
    0x6AE5,  # fill_terrain_columns — 89x gen_terrain_column over Y=$59..1 (RNG; direct buffer writes)
    0x6AEE,  # scroll_field_columns — $0089-gated stars/planet scroll: shift 4 buffers left 1 col + new col (VBI hot path)
    0x6620,  # draw_frame_guide_columns — 86 rows: set_row_ptr_from_count + masked-plot cols $9C/$9D/$A0 (screen ptr)
    0x65FB,  # draw_frame_pattern_seq — per-frame doors/tunnel drawer: 20x draw_symmetric_span_loop + tail draw_frame_guide_columns
    0x6C4D,  # draw_vline_pair — plot a symmetric pair of vertical lines (rows A..$00B8) via plot_pixel_2bpp/bus_write
    0x6BED,  # update_object_distance — clamped 16-bit dist subtract -> $08A4/$08A5[X]; up to 3 draw_vline_pair draws
    0x6BA8,  # advance_object_positions — INC $08D1, +$18 to $08D2/$08D3, then per slot (X=$2A..0 step2) update_object_distance
    # batch — clean pure-mem init/clear/fill/copy leaves:
    0x7F74,  # clear_alien_knock_active — $0632 = 0
    0x3FBF,  # clear_pm_state — fill $00DA-$00DD/$02C0-$02C3/$00D9 with entry A
    0x6B63,  # clear_terrain_lo_buffers — zero $0E32-$0E91/$0F32-$0F91
    0x6899,  # fill_four_bufs_ff — $FF into $0C87/$0D87/$0E87/$0F87 +8..+1
    0x6890,  # fill_buf_08d4 — fill $08D4-$08D9 with entry A
    0x5D3B,  # copy_4byte_table_to_02c4 — copy 4 bytes $5D48+X down into $02C4-$02C7 (entry X)
    0x70E7,  # reset_audctl_flags — $00E7=1, AUDCTL=0, $073A/$0090=0, $073C=$FF
    0x5DDB,  # game_init_first — $0043 = 1
    0x7B74,  # mark_grid_slot_active — $0A00[$28E6] = 1
    0x70A9,  # push_grid_cell — $2500[$0098] = $009C, INC $0098
    0x41DA,  # vobj_pos_to_pmstrip_index — Y = ($DC - $062F) >> 2 (result in Y/A)
    0x45EE,  # copy_terrain_seed_rows — copy 3x8 seed rows $4DD2/$4DDA/$4DE2 -> $0C88/$0D88/$0B88
    0x7483,  # copy_row_addr_subset — copy 48 row-addr entries $073D/$0793[Y] -> $2932/$2962[X]
    0x3C93,  # memset_or_copy — fill $00B7 to dest ptr $C1/$C2, 16-bit count $C3/$C4 (pointer fill)
    0x3C61,  # copy_bytes_to_dst — write entry A to dest ptr $BD/$BE, X times; then INC $BB/$BC (pointer fill)
    # batch — boot_standby_launch_driver-subtree mem-effect leaves:
    0x3FDE,  # draw_compass_heading — copy 4 bytes $4B0B[base..base-3] -> $32E3[3..0] (base from $281C/$2836/$3FF6)
    0x45A1,  # fill_buffer2_region_ff — 8x 32-byte $FF runs from $2098 stride $30
    0x4606,  # game_sub_4606 — init target-state cells $32E3[0..3]/$3355-$3357/$3388/$33DF/$33E0
    0x480F,  # fill_message_buffer — store entry A into $32B6+X down to +1 (entry A/X; X=0 => 256 fill)
    0x4FE0,  # intro_fill_display_params — build $00CF..$00D6 from $4DF1 nibble | $00C2; poke $D019; INC $C2
    0x5B45,  # match_code_sequence — cheat-code matcher; advance/reset $063F, copy payload $5B17->$36AB at 6
    0x68AD,  # init_terrain_dl — fill $2F75..$2FA3=$88 + LMS ptr pairs $300A/$308B = $2F74 every 3rd entry
    0x7238,  # music_init_state — copy 6 bytes $731E[Y..]-> $0657[5..0]; clear $0651/$D208; $0653/$0655=1 (entry Y)
    0x7253,  # music_player_tick — note-stream tune player: tick envelopes + advance cmd stream -> POKEY AUDF/AUDC
    0x75B8,  # count_up_to_level — bump $0604 + binary counter $C3 until $0604 == $006D
    0x811F,  # alien_field1_fill — INC $0081 when $0081 >= $2928 (+ 5-byte copy in the else path)
    0x8168,  # alien_field3_fill — INC $0083 when $0083 >= $A8 (+ 7-byte font copy in the else path)
    # batch — tail-wrappers + RANDOM/compute A-returning leaves:
    0x480B,  # clear_message_buffer — X=$0E,A=0, tail fill_message_buffer (zero $32B7..$32C4)
    0x66D3,  # plot_pixel_col93 — A=$0093, tail plot_pixel_masked
    0x5A59,  # random_digit — POKEY RANDOM -> decimal digit 0-9 (rejection sample; result in A)
    0x5A4D,  # random_alpha_index — POKEY RANDOM -> letter code $21..$3A (rejection sample; result in A)
    0x7047,  # test_marked_neighbor — $0900 marker-map 3-neighbor probe; result in A
    # batch — fill-region wrappers + RANDOM/compute leaves:
    0x3C83,  # fill_region_2000 — seed $C1=$2000/count $0F73, tail memset_or_copy ($00B7 fill)
    0x7F60,  # silence_audio_channels — $0634=A + POKEY AUDF1-4 + AUDCTL=$60, tail clear_alien_knock_active (entry A)
    0x753B,  # init_terrain_render_buffers — $260E..$270D=$FF, then $1070 fill via memset_or_copy
    0x7813,  # init_terrain_col_tables — terrain_col_pixel_mask ($BC00) + clear terrain_col_byte_offset ($BD00) (pure)
    0x7B54,  # game_sub_7B54 — maybe-seed $2849 from (RANDOM|$08)&$3F shifted by 0/1/3
    0x687D,  # rng_signed_jitter — $0085 +/- entry-A magnitude, sign from RANDOM bit7; result in A
    # batch — cockpit/score init + the zero-suppress char plotter:
    0x45C5,  # init_cockpit_bar_cells — seed cockpit bar graphic cells $2107.. = $BE/$AA (pure)
    0x497D,  # add_and_show_bcd_counter — score += delta $0045/$0046 (binary), tail render_bcd_counter
    0x49D9,  # plot_char_bounded — zero-suppress digit plotter via ($C5)+Y; X=suppress flag (entry A/X/Y)
    # batch — BCD digit-pair, slot drivers, and bare-RTS stubs:
    0x49CE,  # emit_bcd_byte_digits — plot hi+lo nibble of packed-BCD A via plot_char_bounded (PHA/PLA)
    0x7B39,  # mark_slot_and_countdown_char — mark_grid_slot_active + countdown_show_char_0620
    0x7B7D,  # mark_slot_and_inc_count — mark_grid_slot_active + set_place_params_inc_count
    0x40AF,  # return_stub_40af — bare RTS (no-op)
    0xA821,  # terrain_obj_skip_return — bare RTS (no-op)
    0x6A26,  # ret_stub_6a26 — bare RTS (no-op)
    0x442D,  # draw_bar_loop_end — bare RTS (no-op)
    0xA63A,  # terrain_plot_return — bare RTS (no-op)
    0xA909,  # terrain_distance_clamp_return — bare RTS (no-op)
    0xAB26,  # plot_line_done — bare RTS (no-op)
    # batch — grid-neighbor scan, ring-push drivers, RLE run fill:
    0x7069,  # scan_grid_neighbors — 4 diagonal test_marked_neighbor probes + push_grid_cell
    0x4FCE,  # intro_reset_score_slots — clear $066A/$0686, $0678=$0C, tail game_sub_55FC (Y=$0D)
    0x7AA8,  # init_event_state_5815_x16 — seed $0044/$3388/$003C, tail ring_push_marked (X=$16, entry A)
    0x3C58,  # rle_run_fill — bump src $BB/$BC, read run byte ($BB)+Y, tail copy_bytes_to_dst (entry A/Y)
    0x678B,  # blit_glyph_8rows — blit 8-row glyph; row ptr walks up $2E/row, bits -> plot_pixel_col93
    # batch — RANDOM-driven intro object-map seeders:
    0x7498,  # intro_seed_object_map — clear $0A00, scan-place entries, stride-$43 RANDOM $64 markers
    0x70B3,  # intro_unmark_random_cells — RANDOM-gated $0900 marker sweep (clear bit7)
    # batch — font/voice init + cockpit message renderer:
    0x5433,  # sfx_engine_reset — clear music/voice tables + seed slots/timers (POKEY via bus_write)
    0x47B8,  # show_cockpit_message — render HUD message (entry Y id) into $32B7 from glyph tables
    0x6811,  # game_sub_6811 — scatter random jittered dots via rng_signed_jitter/set_row_ptr/plot_pixel_masked
    0x692A,  # plot_terrain_span — run of vertical spans via fill_vertical_span; steps cols, shifts row window
    # batch — event/score/message wrappers + object-table shift:
    0x7AA6,  # trigger_effect_4a — A=$4A, tail init_event_state_5815_x16
    0xA6F8,  # terrain_plot_skip_return — bare RTS (no-op)
    0x49AE,  # render_bcd_low_bytes — emit $0602/$0603 digit pairs via emit_bcd_byte_digits
    0x49C5,  # set_zsupp_pos_clear_delta — set $0619=Y, clear $0045/$0046, tail emit_bcd_byte_digits
    0x47B2,  # save_color_clear_y_bit5 — $00D8=A, clear Y bit5, tail show_cockpit_message
    0x6A0F,  # shift_object_table_up — shift DL LMS pairs $3007/$3008 -> $300A/$300B up by 3 (entry A count)
    # batch — glyph blit wrappers + 2x2 glyph draw:
    0x6805,  # set_coord_y_e0 — glyph src $0084/$0085 = $E0(A+$80), tail blit_glyph_8rows
    0x6773,  # glyph_ptr_from_index — glyph src = $E000 + (A<<3), tail blit_glyph_8rows
    0x4099,  # draw_glyph_2rows — 2x2 glyph from $4AE3[A..A+3] | $00BF via ($BB) at cols 0,1,$30,$31
    # batch — RLE expanders:
    0x757B,  # rle_expand_list — expand (count,value) run list via rle_run_fill; 0 count terminates
    0x3C3D,  # rle_decompress — literal/run RLE decompressor ($C0 markers); the 60x-slow cinematic one
    # batch — region-clear loader + PMG bit-table init:
    0x3C00,  # loader_util — clear $32B5/$1000/$0B00 regions via 3x memset_or_copy (pure)
    0x77DF,  # game_init_77DF — build $BE00/$BF00 256-entry bit tables (pure; PHA/PLA -> mask stack)
    0x7D38,  # plot_clipped_pixel — clipped masked HUD/radar pixel via row table + ($C1)+col RMW
    0x74D7,  # unpack_bitmap_4d3e — bit-reversal bitmap unpacker via $4D3E ptr table (8x4 passes)
    0x6FBF,  # intro_random_setup — DFS maze gen on $0900 grid (RANDOM; scan_grid_neighbors/test_marked_neighbor)
    0x68CF,  # emit_dl_coord_pairs — emit DL LMS coord pairs into $300A/$308B from row table, tail plot_terrain_span
    # batch — the largest remaining boot_standby_launch_driver leaf:
    0x75F5,  # compute_stage_display_geometry — derive gauge param block $0617-$062A from $006D (branchy clamps; native bin_to_bcd)
    # batch — the message-text blitters (boot_standby_launch_driver front):
    0x6750,  # blit_label_row — blit 5 glyphs ($6E23[$00C5..] codes) via glyph_ptr_from_index; index base 0/5 from $0004
    0x672D,  # blit_message_block — 11 rows of 3 pixels ($15/$2E/$47) via set_row_ptr_from_count/plot_pixel_masked, tail blit_label_row
    # batch — tail-call wrappers on the boot_standby_launch_driver front (compose native callees):
    0x4095,  # draw_digit_low_nibble — A=(A&$0F)<<2, tail draw_glyph_2rows
    0x4084,  # draw_2digit_value — draw hi nibble glyph, advance dest ptr $BD/$BE->$BB/$BC, tail draw_digit_low_nibble
    0x6802,  # glyph_ptr_shift3 — A<<3, tail set_coord_y_e0
    0x49C0,  # render_bcd_top_byte — Y=5, X=$0600, tail set_zsupp_pos_clear_delta (renders entry-A byte via emit chain)
    # batch — RLE-composer init wrappers (boot_standby_launch_driver front; compose native RLE):
    0x7558,  # unpack_terrain_seed_cols — set src/dst ptrs, 2x rle_expand_list ($4DFA->$0C32, $4E09->$0D32)
    0x7588,  # game_init_7588 — fill $32FD..$332C=$AA, then rle_decompress $6E6E -> $332D
    # batch — score/HUD digit renderers (boot_standby_launch_driver front; compose native glyph/bcd):
    0x49BA,  # render_bcd_digits_supp_all — Y=7,X=0 -> set_zsupp_pos_clear_delta (LDX#0 makes BEQ unconditional)
    0x67C3,  # blit_numeric_readout — $0004!=0: 4 glyphs from $060D-$0610; else BCD of $006D (clamp $63) as 2 glyphs
    # batch — DL LMS fill + cockpit dial-bar column (boot_standby_launch_driver front):
    0x69F1,  # dl_lms_fill — copy $073D/$0793[X=$8B..$0086] pairs into ($C5)+Y (Y+=3), tail shift_object_table_up/ret_stub
    0x43CB,  # draw_dial_bar_column — gate on Y vs $062E/8, set bar params, tail draw_object_column (entry Y)
    # batch — the big lock-on indicator sprite drawer (boot_standby_launch_driver front, 311 bytes):
    0x42A7,  # draw_player3_object — player-3 lock-on sprite: HPOS/size via bus_write, mask blit $0F1E/$0F71, RANDOM
    0x8C58,  # build_player2_sprite — depth-scaled object/explosion P2 sprite builder (per-frame while object_anim_frame != 0)
    0x4467,  # update_p3_indicator_stripe — rewrites the P3 scope-indicator PM buffer ($0F98) 50Hz when a P3 object/target is active
    # batch — DL-build wrappers + initials BCD (now unblocked by dl_lms_fill/render_bcd_digits_supp_all):
    0x69E5,  # dl_lms_build — set $C5/$C6=$300A, $0086=$56, tail dl_lms_fill
    0x76CB,  # game_init_76CB — build the flight display list ($30xx-$32xx) + 2x build_row_addr_table/dl_lms_fill
    0x5A63,  # setup_initials_ptr — $C5/$C6=$3694, BCD of $006D, $3694=0, tail render_bcd_digits_supp_all
    # batch — the score/level HUD refresh driver:
    0x3FFA,  # startup_init — refresh level/score/lives HUD digits (draw_digit_low_nibble/draw_2digit_value, ring_push)
    # batch — DL index wrappers (now unblocked by dl_lms_build):
    0x69E3,  # dl_index_dec — DEC $8B, tail dl_lms_build
    0x69DD,  # dl_index_dec_or_reset — $8B=0 (LDA#0 makes BEQ unconditional), tail dl_lms_build
    # batch — the 2D scaled-shape blitter (last portable boot_standby_launch_driver-front leaf):
    0x7C9A,  # draw_scaled_shape — scale/blit a shape: div-by-subtraction count, nested row/col accum, mask bits -> plot_clipped_pixel
    # batch — empty the front: the deferred/HW leaves (faithfulness, little/no speedup):
    0x8181,  # reorder_cell_bits — interleave A + $0084 via ROL/ROR carry chain (result in A); unblocks alien_field0_fill/2
    0x5A78,  # read_console_trig_delta — A = (CONSOL & 1) - TRIG0 (HW $D01F/$D010 via bus_read; result in A)
    0x5D0D,  # validate_save_state — compare $3700/$3714 + 38-byte $37C7 vs $7BDA; result in Z (no mem writes)
    0x4430,  # cockpit_dial_update — $006F=A, derive $0022 (0 or $4457[A+$0625]), tail draw_cockpit_dial_bar
    # batch — HUD fill fields (unblocked by reorder_cell_bits):
    0x8105,  # alien_field0_fill — pack 5 bytes ($85)+Y via reorder_cell_bits into $8F-$93 (or INC $0080)
    0x8138,  # alien_field2_fill — copy/pack ($89)+Y into $8F-$9A per $292D flag (or INC $0082)
    0x80C5,  # alien_shape_blit — clear $8F-$9F, fill 4 fields, compose cells via $BE00 + ($8B)/($8D), advance ptrs
    # --- NATIVE APEX (2026-06-15): the orchestration apex, hand-written in rof_native.c.
    #     NOT validated by `make validate` (spin-waits on VBI state / live input would hang
    #     the harness) — verified on FS-UAE by behaviour.  Its __t6502 oracle is kept for
    #     reference; the native twin replaces the spin-wait SPINWAIT-hooks with ds_frame().
    0x5F1D,  # boot_standby_launch_driver — main display setup + Standby/attract idle loop + launch cinematic driver
    0x3D48,  # game_main_loop — one-time game init + L_3e0f boot_standby_launch_driver + the in-game flight loop (never returns)
    0x587B,  # standby_scoreboard_render — Standby/Title scoreboard render + input dispatch; tail-calls the live-input standby_level_select_loop / sound_retrigger_random loops + spins on $00E5 (would hang the harness)
    # --- flight-init de-transpile (2026-06-22): the last transpiled orchestrator on the
    #     game/level-init path.  Every leaf it calls is already native; this just sheds the
    #     $73C8 body itself.  Like the apex it calls the wait_timer_4c_frames spin-pacer
    #     (wait_frames) so it is NOT in `make validate` — verified on FS-UAE. ---
    0x73C8,  # init_gameplay_state — per-game/level init: seed heading/arrays, compass, cockpit bars; tail cockpit_dial_update
    # --- HW beam spin, hand-written in rof_native.c.  NOT in `make validate` (it busy-waits on
    #     the live ANTIC VCOUNT / real Amiga beam, which the headless harness can't reproduce);
    #     its __t6502 oracle is kept for reference only. ---
    0x3C7B,  # wait_vcount_ge_7a — spin until VCOUNT ($D40B) >= $7A (beam-sync a DL/colour swap)
    # --- frame-wait spin-pacers, hand-written in rof_native.c (2026-07-05).  NOT in
    #     `make validate` (they busy-wait on RTCLOK, advanced async by the $4FF5 ISR, which
    #     the headless harness can't reproduce); __t6502 oracles kept for reference only.
    #     The clean twins fold the 0x3CB8 SPINWAIT-hook overshoot mitigation into plain C. ---
    0x3CB1,  # wait_frames — PHA, wait frame_wait_count ($4C) frames, PLA (accumulator preserved)
    0x3CB2,  # wait_timer_4c_frames — wait the caller-set frame_wait_count ($4C) count of vertical-blank periods
    # --- enemy lock-on indicator animation cluster (2026-07-06): the 6-light targeting
    #     indicator (#11, cells $3491-$3497).  Driven by both the standby VBI (planet
    #     descent) and the flight VBI (via lock_on_indicator_dispatch); the native twins
    #     raise platform_lockon_changed() at each cell write so the Amiga re-decodes them
    #     (keeps the lights blinking through the descent — faithful to the Atari). ---
    0x4225,  # lock_on_indicator_dispatch — gate on $0043: phase_advance vs lock_on_indicator_tick
    0x4229,  # lock_on_indicator_tick — state machine on $007E (init / step / blink / reverse-fill)
    0x4258,  # lock_on_indicator_fill_cells (lock_on_indicator_fill_cells) — fill the 6 lit glyphs $3492-$3497
    0x4265,  # lock_on_indicator_step — advance one light per timer tick ($007E 1..7)
    0x4285,  # lock_on_indicator_write_cell — write a glyph to $3491+Y, then ring_push_marked X=$12
    0x428D,  # lock_on_indicator_return — empty RTS landing pad (shared exit)
    0x428E,  # lock_on_indicator_phase_advance — reverse-fill phase driver ($007E >= $81)
    # --- standby/launch tunnel-ring + door-scroll cinematic driver (2026-07-11):
    #     these are pure mem[] 6502 logic (not Amiga-specific), so they move out of
    #     rof_native_amiga.cpp into rof_native.c as faithful native twins.  The Amiga
    #     dirty-band glue (g_tun*) draw_ring_frame_step publishes lives under
    #     #ifdef ROF_PLATFORM_AMIGA; advance_history_6a4d's reorder_sprite_slot tail is
    #     skipped on Amiga (#ifndef ROF_PLATFORM_AMIGA) to preserve the confirmed cinematic. ---
    0x6AB5,  # add_multibyte_a1 — multi-byte accumulator add ($00A1..$00A4), returns top byte
    0x6A4D,  # advance_history_6a4d — rotate the 6-byte colour ring $08D4-$08D9 + $0685 bump
    0x670D,  # draw_ring_frame_step — one tunnel-ring frame-clear step (draw_symmetric_span_loop)
    0x6A38,  # step_accum_add_75 — add $75, gate the ring step + ring rotate
    0x6A8F,  # step_accum_sub_7e — reverse ring step: sub $7E, draw_symmetric_span_loop + g_tun* publish
    0x69A9,  # dl_lms_scroll_up — shift top-half DL LMS entries up one slot
    0x69C3,  # dl_lms_scroll_down — shift bottom-half DL LMS entries down one slot
    0x6973,  # dl_lms_push_top — push a fresh top-edge LMS row pointer (X-=3)
    0x698E,  # dl_lms_push_bottom — push a fresh bottom-edge LMS row pointer (Y+=3)
    0x6953,  # scroll_terrain_dl — one door-open step: scroll both DL halves + push edges
    # --- pilot-rescue state machine (2026-07-11): native-ize the cluster around
    #     pilot_render ($7854) to understand + fix the Systems-off/rescue FREEZE
    #     (the L_78d6<->L_792e hold loop stuck on $003D/$003E). Leaves-first. ---
    0x4968,  # clear_pilot_rescue_state — clears $003E + pilot_visible/pilot_prev (entry A)
    0x495F,  # reset_pilot_state_if_no_2830 — clears $003D if $2830==0, tail clear_pilot_rescue_state
    0x4971,  # copy_display_params_to_buffer — copy 16 bytes $00CF..$00DE -> $07E9..$07F8
    0x47A3,  # set_colpf0_from_flag — pick COLPF0 by Y bit5, tail save_color_clear_y_bit5 (entry Y)
    # frame-driven colour-clear sweeps (validated via the opt-in RTCLOK-tick fixture):
    0x6DF4,  # audf2_sweep_clear_colors — AUDF2 pitch sweep over frame_wait_count frames
    0x7A89,  # clear_colors_sweep_5x — 5-pass colour-clear timer gated on $003E (load-bearing exit Z)
    0x7A17,  # animate_clear_colors_timed — RTCLOK-gated colour-clear stepper (nested waits + RANDOM)
    # batch 3 — message drivers (call native show_cockpit_message; entry regs):
    0x4958,  # show_message_with_d8 — $00D8=$48, tail show_cockpit_message (entry Y id)
    0x4956,  # show_message_id_a — $0072=A, tail show_message_with_d8 (entry A,Y)
    0x493D,  # show_ace_or_message — ACE ($3A bit7) vs pilot message driver (entry Y)
    # batch 3 — rescue FX loops (frame-driven; RTCLOK-tick fixture):
    0x7B94,  # level_clear_fx_loop — INC $283C; 15x ring pairs + waits; $3C-iter RANDOM $DB flash
    0x7EC7,  # alien_knock_setup_loop — rescue SFX/zoom setup + descending-pitch sweep (flight-snapshot fixture, $3E==0)
    # batch 4 — the in-flight keyboard-command dispatcher:
    0x4644,  # event_sequence_dispatcher — match keycode vs $4816, dispatch by mode/slot (flight-snapshot fixture)
    # batch 5 — the pilot-rescue state machine (the hold loop lives here):
    0x7854,  # pilot_render — pilot/rescue render + rescue state machine (validated $3E==0 path; loop inspection-only)
    # batch 6 — the alien-creature animation/blit driver (jump-scare hot path):
    0x7F85,  # alien_creature_animate_draw — 3-voice frame sequencer + shape-table setup + the row-blit draw loop
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
    # VCOUNT position wait (wait_vcount_eq $3C75): spin until ANTIC VCOUNT $D40B == A (an EXACT-
    # equality beam sync before a DL-pointer / VDSLST / hardware-register write).  Safe on the
    # Atari (the 6502 polls VCOUNT thousands of times/frame under a SHORT VBI, so it never skips the
    # target value).  On the Amiga the emulated VCOUNT ($D40B = rof_beam_line()>>1) is read far less
    # often AND the loop can be preempted by the HEAVY flight VBI ($4FF5) — which is still the active
    # VBI when boot_standby_launch_driver is re-entered on the mother-ship RETURN path (game_main_loop only sets
    # the light $53CC VBI once at the top, not on the outer-loop re-entry) — so consecutive reads step
    # OVER the target and the exact-equality test HANGS, blocking the $52D7 install (no launch
    # cinematic on the return).  The beam sync guards writes that are copper-irrelevant on the Amiga
    # anyway (cf. wait_vcount_ge_7a, a NO-OP here), so force the match: set cpu.A to the just-read
    # VCOUNT so the following CMP is equal → exit immediately (mirrors the $3CB8 RTCLOK reached-or-
    # passed hook).  Atari/SDL keep the faithful spin.
    0x3C75: 'platform_poll_events();\n#ifdef ROF_PLATFORM_AMIGA\n    cpu.A = bus_read(0xD40B);\n#endif',
    # RTCLOK frame wait (wait_timer_4c_frames $3CB2): "wait N frames" -- STA $14=0 then spin until
    # RTCLOK_LOW($14) reaches target (A=$4C).  The 6502 uses an EXACT-equality exit (CMP $14 /
    # BNE), safe on HW because the CPU polls $14 thousands of times/frame so it never skips a
    # value.  On the Amiga port RTCLOK is advanced ASYNC by the $4FF5 flight ISR, and each spin
    # iteration drives one render that can span several real VBIs (~5 frames) -- so $14 jumps by
    # >1 per iteration and can step OVER the target.  The emitted CMP/BNE then keeps spinning
    # while the ISR drags $14 a full 256-tick lap back to the exact value: a ~256-frame (~5s)
    # stall, hit or missed by pure render-timing alignment = the run-by-run flight-transition
    # variance (init_gameplay_state's 5 push_a_thunk waits).  A SPINWAIT hook can't change the
    # emitted CMP/BNE, so make the exit reached-or-passed by clobbering the THROWAWAY cpu.A
    # (PLA'd at $3CBC, never reused) to equal $14 once target is met/passed -- forcing the next
    # CMP to match.  RTCLOK ($14) itself is left untouched (monotonic; keeps the few-tick
    # overshoot).  While still SHORT, tick one real frame (poll-then-advance).  "Short" = target
    # is 1..127 ahead of $14 ((A-$14)&0xFF < 0x80); else $14 has reached/passed it (incl. target
    # 0, vobj_step_down's gauge-wrap row: $14==0==A -> immediate match, 0-frame wait, as on HW).
    0x3CB8: 'if (mem[0x0014] != cpu.A) { if ((uint8_t)(cpu.A - mem[0x0014]) < 0x80u) { platform_tick_vbi(); platform_render_frame(); } else cpu.A = mem[0x0014]; }',  # RTCLOK frame wait

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
    # L_6578: planet-rise loop in boot_standby_launch_driver — paces FUN_6ba8 every 2 VBI
    # frames off RTCLOK ($14) until $1002==$FF. Without a VBI tick here the
    # frame counter never advances and the loop spins forever (planet never
    # rises into view after the star scroll).
    0x6578: 'platform_tick_vbi(); platform_render_frame();',
    0x79D0: 'platform_tick_vbi(); platform_render_frame();',
    # L_78d6: TOP of pilot_render's whole rescue loop.  pilot_render's body is one big loop
    # (L_78d6 ... L_7a14 -> goto L_78d6) that is entered ONLY while systems are off
    # ($003E != 0) — with systems on, L_78f2 falls through to L_78fd and the function RETURNS
    # instead of looping.  So systems-off during a rescue runs this loop (advancing the landing
    # sequence $003D, playing knock SFX, colour sweeps) and is FAITHFUL: the real Atari does the
    # same (measured 2026-07-12; at $003D==2 it even hard-hold-loops there too).  But unlike the
    # $06FF sound-wait below (which HAS a yield), this loop had none, so on the Amiga renderFrame
    # is never called for most of it and the display/cockpit freeze except during a knock sound.
    # The Atari stays alive because ANTIC shows the persistent field + the VBI updates the cockpit.
    # Restore that: drive one frame per loop iteration whenever the loop is active ($003E set).
    # Gated on $003E so normal (systems-on) passes through $78d6 add NO spurious frames; the yield
    # also lets the VBI process a later S/systems-on press (clears $003E -> loop exits -> resume).
    0x78D6: 'if (mem[0x003E]) { platform_tick_vbi(); platform_render_frame(); }',
    # L_7c08: animate_zoom_sequence's per-phase RTCLOK frame wait ("LDA #3; CMP $14; BCS L_7c08"
    # -> spin until RTCLOK_LOW>=4, then reset $14=0).  This is the rescue "figure walks to the
    # airlock" zoom (called ONLY from pilot_render's systems-off rescue path, L_79a2, so it never
    # runs in normal flight).  Like the L_78d6 hold loop it had NO yield, so on the Amiga the display
    # freezes through the ~4-frame-per-phase x 8-phase zoom -- the cockpit only ever refreshed during
    # the footstep-SFX sound spin (L_79d0, which HAS a yield).  Drive one frame per RTCLOK-wait
    # iteration so the zoom animates + the cockpit/PMG stay live (same fix as the knock hold loop).
    # RTCLOK is advanced by the flight VBI (platform_tick_vbi), so the yield also lets the spin exit.
    0x7C08: 'platform_tick_vbi(); platform_render_frame();',
    # Attract-mode loops: need VBI to fire for animation, audio, and input.
    # L_62EB: outer loop entry — tick VBI to drive the game
    # L_62F6: inner input-poll loop — tick VBI here too so audio/rtclok work
    # L_634A: tightest inner loop (FUN_5A78 check) — poll events to detect keys
    0x62EB: 'platform_tick_vbi(); platform_render_frame();',
    0x62F6: 'platform_tick_vbi(); platform_render_frame();',
    0x634A: 'platform_poll_events();',
    # L_596d: standby_scoreboard_render's game-over / high-score wait — "LDA $00E5; BNE L_596d" spins
    # while the game-over countdown $00E5 (set to 5 by the death teardown $4F76) is nonzero,
    # played out under the $53CC in-game VBI while the game-over jingle runs.  On real HW ANTIC
    # keeps showing screen RAM and the VBI decrements $00E5, so the LAST/HIGH SCORE + level
    # digits (already written into $365B before this spin) are visible the whole time.  This
    # spin had NO yield, so on the Amiga renderFrame never ran during it: the Title-screen value
    # cells (marked dirty by the digit writers) were never decoded into titleScreenBitmap until
    # the spin exited and standby resumed rendering -> the score/level appeared only once the
    # standby music started (measured 2026-07-13, PC frozen at rof_gen.c:5693, g_renderFrameCount
    # stuck, cellLo/Hi=57/119 unconsumed).  Drive one frame per iteration so the game-over screen
    # renders (and decodeTitleCells consumes the dirty range) immediately; the VBI advance also
    # keeps decrementing $00E5 so the spin still exits.
    0x596D: 'platform_tick_vbi(); platform_render_frame();',
}

# Pre-instruction hook injection.
# Unlike SPINWAIT_HOOKS (emitted at a branch-target LABEL), these inject a C
# statement immediately BEFORE the instruction at the given address, with no
# label.  Used for faithful hardware seams that land mid-instruction-stream
# where no branch label exists (so a forced label would be unreferenced and
# trip -Wunused-label).
# Key: 6502 address of the instruction to inject before.  Value: C statement(s).
PRE_INSN_HOOKS = {
    # $519c — the flight VBI's 1-instruction CLI window (vbi_handler_flight).
    # The 6502 does `LDX #$FF` ($519a) then `CLI`($519c)/`SEI`($519d): if a POKEY
    # KEYBOARD/BREAK IRQ ($D20E IRQEN=$C0, vector irq_handler $462A) fires inside
    # that window it leaves the event id (KBCODE&$3F, or $80 for BREAK) in X, and
    # $51a6 `BMI` skips the dispatch when X stays $FF (no key).  The Amiga has no
    # such IRQ, so we deliver an in-flight command keycode here instead: the CIA-A
    # keyboard handler stashes a pending Atari KBCODE, platform_flight_irq_key()
    # returns+clears it (or $FF if none) — exactly mimicking the handler clobbering
    # X.  No-op everywhere it returns $FF (SDL / validate headless): X stays $FF.
    0x519c: '{ unsigned char _k = platform_flight_irq_key(); if (_k != 0xFFu) cpu.X = _k; }',
    # (The creature-blit capture is NOT a hook here: $80C5 is a native twin (alien_shape_blit),
    #  so the running game never executes the transpiled oracle — the capture lives in the native
    #  twin in rof_native.c instead, gated on $0632 alien_knock_active.)
    # NB: vbi_handler_flight ($4FF5) is now a native twin (rof_native.c) — only its __t6502
    # validation oracle is transpiled here, so the $519c key-injection hook above still applies
    # to the oracle (keeping its keyboard behaviour identical to the native).  The old
    # top/atmo/hud/score/tail PRE_INSN_HOOKS sub-phase profilers were removed when the handler
    # went native; the native is timed as a whole by flight_vbi_native (g_flightProf.isrLines)
    # with integ/proj/sfx still sub-measured by their wrappers.
}

# ---------------------------------------------------------------------------
# Parse symbols.csv → addr_int → name
# ---------------------------------------------------------------------------
# addr_int → note (5th CSV column); populated by load_symbols, consumed when
# emitting each function so the symbol's note becomes a C doc-comment.
SYMBOL_NOTES = {}

# addr_int → snake_case name, restricted to non-hardware *var* rows of
# symbols.csv.  These are the named RAM/state addresses; the emitter rewrites a
# direct single-byte access mem[0xADDR] → mem[MEM_<name>] (defined in the
# generated AtariMem.h) so the transliterated C reads as named state rather than
# raw hex.  Indexed / indirect / 16-bit-pointer accesses keep raw hex.
VAR_NAMES = {}

def load_symbols(path):
    sym = {}
    SYMBOL_NOTES.clear()
    VAR_NAMES.clear()
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
        # Collect named RAM state for mem[MEM_*] substitution + AtariMem.h.
        if len(parts) >= 4 and parts[2].strip() == 'var' and parts[3].strip() == '0':
            VAR_NAMES[addr_i] = name
    return sym

def mem_alias(addr):
    """Bare lvalue alias (e.g. `level_stage`) for a named non-hardware address,
    else None.  Used for DIRECT single-byte accesses, which read cleanest as
    `level_stage = cpu.A` (the alias expands to mem[MEM_level_stage] via the
    ROF_MEM_ALIASES block of mem.h)."""
    return VAR_NAMES.get(addr)

def mem_index(addr):
    """Standalone address expression: MEM_<name> when the address has a
    symbols.csv var row, otherwise the raw 0xADDR literal.  Used where an
    *address* (not an lvalue) is needed — INC_M/DEC_M targets, bus_write()."""
    name = VAR_NAMES.get(addr)
    return f'MEM_{name}' if name else f'0x{addr:04X}'

def mem_base(addr):
    """Base expression for an *indexed* access (additive context, e.g.
    `MEM_foo+cpu.X`): MEM_<name> when named, else the hex literal parenthesised
    to dodge the C preprocessor's E+/P+ pp-number tokenisation quirk."""
    name = VAR_NAMES.get(addr)
    return f'MEM_{name}' if name else f'(0x{addr:04X})'

def write_atari_mem_header(path):
    """Generate AtariMem.h from VAR_NAMES (symbols.csv var rows).

    Emits MEM_<name> = 0xADDR offset macros (usable in C and C++), plus an
    OPT-IN block of bare `<name> -> mem[MEM_<name>]` lvalue aliases gated on
    ROF_MEM_ALIASES (C files only; they would textually clobber any local of the
    same name, so each consumer opts in deliberately)."""
    items = sorted(VAR_NAMES.items())  # by address
    width = max((len(n) for n in VAR_NAMES.values()), default=1)
    lines = [
        '#pragma once',
        '',
        '// AUTO-GENERATED by tools/transpile.py from disasm/symbols.csv (var rows).',
        '// Do NOT edit by hand — regenerate with `make gen`.',
        '//',
        '// Named offsets into the shared mem[65536] snapshot of the Atari 6502',
        '// address space.  symbols.csv is the source of truth for the names.',
        '//',
        '//   MEM_<name>   numeric offset (C and C++).  Use as mem[MEM_level_stage].',
        '//   <name>       OPT-IN bare lvalue alias for mem[MEM_<name>]; enable with',
        '//                `#define ROF_MEM_ALIASES` before including (C only — a macro',
        '//                of a plain name would clobber same-named locals / C++ members).',
        '//',
        '// These are general RAM / OS-shadow addresses, NOT just zero page (e.g.',
        '// $08D4 color_ring, $2885 heading_lo) — named after the mem[] snapshot',
        '// they index, not "zero page" (the old AtariZp.h was a misnomer).',
        '',
    ]
    for addr, name in items:
        note = SYMBOL_NOTES.get(addr, '')
        if len(note) > 64:
            note = note[:61] + '...'
        comment = f'  // ${addr:04X}{(" " + note) if note else ""}'
        lines.append(f'#define MEM_{name:<{width}} 0x{addr:04X}{comment}')
    lines += [
        '',
        '#ifdef ROF_MEM_ALIASES',
        '// Bare lvalue aliases: write `level_stage` for `mem[MEM_level_stage]`.',
    ]
    for addr, name in items:
        lines.append(f'#define {name:<{width}} mem[MEM_{name}]')
    lines += ['#endif /* ROF_MEM_ALIASES */', '']
    path.write_text('\n'.join(lines))
    print(f'Wrote {path}  ({len(items)} named addresses)')

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
    alias = mem_alias(addr)
    return alias if alias else f'mem[0x{addr:04X}]'

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
    """C expression that reads the source value.  Direct (zp/abs) named accesses
    read as the bare alias (level_stage); indexed accesses keep the MEM_<name>
    base in the index arithmetic (mem[MEM_foo+cpu.X])."""
    if mode == 'imm':   return f'0x{addr:02X}'
    if mode in ('zp','abs'):
        return addr_read(addr)
    if mode == 'zpx':   return f'mem[(uint8_t)({mem_base(addr)}+cpu.X)]'
    if mode == 'zpy':   return f'mem[(uint8_t)({mem_base(addr)}+cpu.Y)]'
    if mode == 'absx':  return f'bus_read(0x{addr:04X}+cpu.X)' if is_hw(addr) else f'mem[{mem_base(addr)}+cpu.X]'
    if mode == 'absy':  return f'bus_read(0x{addr:04X}+cpu.Y)' if is_hw(addr) else f'mem[{mem_base(addr)}+cpu.Y]'
    if mode == 'indy':  return f'bus_read(ZP_IND_Y(0x{addr:02X}))'
    if mode == 'indx':  return f'bus_read(ZP_IND_X(0x{addr:02X}))'
    return '0'

# operand_read now handles every mode (incl. abs,X/abs,Y via mem_base), so the
# old _fixed variant is a thin alias kept for call-site compatibility.
def operand_read_fixed(mode, addr, idx):
    return operand_read(mode, addr, idx)

def operand_addr_expr(mode, addr, idx):
    """C expression giving the effective *address* (for INC_M/DEC_M targets and
    indexed write bases).  Always an address, never a bare lvalue alias."""
    if mode in ('zp','abs'):  return mem_index(addr)
    if mode == 'zpx':  return f'(uint8_t)({mem_base(addr)}+cpu.X)'
    if mode == 'zpy':  return f'(uint8_t)({mem_base(addr)}+cpu.Y)'
    if mode == 'absx': return f'{mem_base(addr)}+cpu.X'
    if mode == 'absy': return f'{mem_base(addr)}+cpu.Y'
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
    if mode in ('zp','abs'):
        if needs_bus_write(addr):
            return f'bus_write({mem_index(addr)}, {val_expr})'
        alias = mem_alias(addr)
        if alias:
            return f'{alias} = {val_expr}'           # bare lvalue: level_stage = ...
        return f'mem[0x{addr:04X}] = {val_expr}'
    ea = operand_addr_expr(mode, addr, idx)
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
# Peephole: register/flag liveness + load-immediate→store folding
# ---------------------------------------------------------------------------
# Cleans up the transliteration's `LDA(#imm); STA addr;` two-step (a faithful
# but ugly 6502 idiom) into a direct `addr = imm;` when the loaded register and
# the N/Z flags it set are provably dead afterwards.  Gated on a real liveness
# analysis so the fold can never drop a value/flag a later instruction (or the
# function's exit, treated as live for all regs/flags) still needs.
PEEPHOLE = True

ALL_LIVE = frozenset({'A', 'X', 'Y', 'N', 'Z', 'C', 'V'})

# Branch mnemonic → the flag it reads.
_BRANCH_READ = {'BEQ': 'Z', 'BNE': 'Z', 'BCS': 'C', 'BCC': 'C',
                'BMI': 'N', 'BPL': 'N', 'BVS': 'V', 'BVC': 'V'}

def _mode_index_reg(mode):
    """The register an addressing mode reads to form the effective address."""
    if mode in ('zpx', 'absx', 'indx'): return 'X'
    if mode in ('zpy', 'absy', 'indy'): return 'Y'
    return None

def insn_effects(mnem, mode):
    """Return (reads, writes) sets over {A,X,Y,N,Z,C,V} for one instruction.
    Conservative: anything unmodelled reads+writes everything (so nothing around
    it is ever considered dead).  The index register of an indexed/indirect mode
    is always an additional read (it is needed to compute the address)."""
    rd, wr = _insn_effects_base(mnem, mode)
    ireg = _mode_index_reg(mode)
    if ireg:
        rd = rd | {ireg}
    return (rd, wr)

def _insn_effects_base(mnem, mode):
    if mnem in ('LDA','LDX','LDY'): return (set(),            {mnem[2], 'N', 'Z'})
    if mnem in ('STA','STX','STY'): return ({mnem[2]},        set())
    if mnem == 'TAX': return ({'A'}, {'X','N','Z'})
    if mnem == 'TAY': return ({'A'}, {'Y','N','Z'})
    if mnem == 'TXA': return ({'X'}, {'A','N','Z'})
    if mnem == 'TYA': return ({'Y'}, {'A','N','Z'})
    if mnem == 'TSX': return (set(), {'X','N','Z'})
    if mnem == 'TXS': return ({'X'}, set())
    if mnem in ('INX','DEX'): return ({'X'}, {'X','N','Z'})
    if mnem in ('INY','DEY'): return ({'Y'}, {'Y','N','Z'})
    if mnem in ('ADC','SBC'): return ({'A','C'}, {'A','N','Z','C','V'})
    if mnem in ('AND','ORA','EOR'): return ({'A'}, {'A','N','Z'})
    if mnem == 'CMP': return ({'A'}, {'N','Z','C'})
    if mnem == 'CPX': return ({'X'}, {'N','Z','C'})
    if mnem == 'CPY': return ({'Y'}, {'N','Z','C'})
    if mnem == 'BIT': return (set(), {'N','Z','V'})
    if mnem in ('INC','DEC'): return (set(), {'N','Z'})
    if mnem in ('ASL','LSR'):
        if mode in ('acc','impl'): return ({'A'}, {'A','N','Z','C'})
        return (set(), {'N','Z','C'})
    if mnem in ('ROL','ROR'):
        if mode in ('acc','impl'): return ({'A','C'}, {'A','N','Z','C'})
        return ({'C'}, {'N','Z','C'})
    if mnem in _BRANCH_READ: return ({_BRANCH_READ[mnem]}, set())
    if mnem in ('CLC','SEC'): return (set(), {'C'})
    if mnem == 'CLV': return (set(), {'V'})
    if mnem in ('CLD','SED','CLI','SEI','NOP'): return (set(), set())
    if mnem == 'PHA': return ({'A'}, set())
    if mnem == 'PLA': return (set(), {'A','N','Z'})
    if mnem == 'PHP': return ({'N','Z','C','V'}, set())
    if mnem == 'PLP': return (set(), {'N','Z','C','V'})
    if mnem in ('JMP','RTS','RTI','BRK'): return (set(), set())  # control flow via succ/sink
    # JSR and anything else: assume it reads args + clobbers everything.
    return (set(ALL_LIVE), set(ALL_LIVE))

def compute_liveness(insns, symbols, local_targets):
    """Backward CFG liveness over regs/flags.  Returns live_out[index] (a set).
    Exits (RTS/RTI/BRK, tail-calls, indirect jumps, fall-through) are sinks where
    every reg/flag is live — so a value reaching an exit is never folded away."""
    n = len(insns)
    idx_by_addr = {ins['addr']: i for i, ins in enumerate(insns)}
    eff = []
    succ = []
    for i, ins in enumerate(insns):
        mnem, op, nbytes = ins['mnem'], ins['op'], len(ins['bytes'])
        eff.append(insn_effects(mnem, parse_operand(op, nbytes, symbols)[0]))
        s = []
        if mnem in BRANCH_FLAGS:
            _, val, _ = parse_operand(op, nbytes, symbols)
            s.append(idx_by_addr[val] if val in local_targets and val in idx_by_addr else 'EXIT')
            s.append(i + 1 if i + 1 < n else 'EXIT')         # fall-through
        elif mnem == 'JMP':
            _, val, _ = parse_operand(op, nbytes, symbols)
            s.append(idx_by_addr[val] if val in local_targets and val in idx_by_addr else 'EXIT')
        elif mnem in ('RTS', 'RTI', 'BRK'):
            s.append('EXIT')
        else:  # JSR, loads/stores, ALU, … fall through to next
            s.append(i + 1 if i + 1 < n else 'EXIT')
        succ.append(s)

    live_in = [set() for _ in range(n)]
    live_out = [set() for _ in range(n)]
    changed = True
    while changed:
        changed = False
        for i in range(n - 1, -1, -1):
            out = set()
            for s in succ[i]:
                out |= ALL_LIVE if s == 'EXIT' else live_in[s]
            rd, wr = eff[i]
            inn = (out - wr) | rd
            if out != live_out[i] or inn != live_in[i]:
                live_out[i], live_in[i] = out, inn
                changed = True
    return live_out

def compute_imm_store_folds(insns, symbols, local_targets, external_entry_labels,
                            blocked_addrs):
    """Fold a `LD{R}(value)` into the run of consecutive `ST{R}` that follows it,
    turning each store into `<store> = value;` and dropping the load.  Returns
    (skip_loads, store_vals): load indices to omit, store index→value-expr.

    Folds handled:
      * load-immediate, single OR multiple stores:  LDA #0; STA a; STA b -> a=0; b=0;
      * load-memory, single store only:             LDA $x; STA $y       -> $y = $x;
        (single-store only: a 2nd store in the run could write $x and change what a
         later store should read.)

    Safe iff: the load and EVERY store in the run are straight-line (not a branch
    target / split / injected-hook addr — so each store is reachable only via the
    load, guaranteeing cpu.R holds `value` there), and R + N + Z are all dead in
    live_out of the LAST store (nothing later needs the register or its flags)."""
    skip_loads, store_vals = set(), {}
    if not PEEPHOLE:
        return skip_loads, store_vals
    live_out = compute_liveness(insns, symbols, local_targets)
    n = len(insns)

    def straight_line(a):
        return not (a in local_targets or a in external_entry_labels or a in blocked_addrs)

    i = 0
    while i < n - 1:
        ld = insns[i]
        if ld['mnem'] not in ('LDA', 'LDX', 'LDY') or not straight_line(ld['addr']):
            i += 1; continue
        reg = ld['mnem'][2]
        lmode, lval, lidx = parse_operand(ld['op'], len(ld['bytes']), symbols)
        if lmode == 'imm':
            val_expr, allow_multi = f'0x{lval:02X}', True
        elif lmode in ('zp', 'abs', 'zpx', 'zpy', 'absx', 'absy', 'indx', 'indy'):
            val_expr, allow_multi = operand_read_fixed(lmode, lval, lidx), False
        else:
            i += 1; continue
        # Maximal run of consecutive same-register stores.
        run = []
        j = i + 1
        while j < n and insns[j]['mnem'] == 'ST' + reg:
            run.append(j); j += 1
        if not run or (not allow_multi and len(run) > 1):
            i += 1; continue
        # Every store in the run must be straight-line; the last store must leave
        # R and the N/Z flags dead.
        if not all(straight_line(insns[k]['addr']) for k in run):
            i += 1; continue
        if {reg, 'N', 'Z'} & live_out[run[-1]]:
            i += 1; continue
        skip_loads.add(i)
        for k in run:
            st = insns[k]
            smode, sval, sidx = parse_operand(st['op'], len(st['bytes']), symbols)
            store_vals[k] = write_expr(smode, sval, sidx, val_expr)
        i = run[-1] + 1
    return skip_loads, store_vals

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

    # Peephole: fold `LD{R}(#imm); ST{R} addr;` → `addr = imm;` (liveness-checked).
    blocked_addrs = set(PRE_INSN_HOOKS) | set(SPINWAIT_HOOKS)
    skip_loads, store_vals = compute_imm_store_folds(
        insns, symbols, local_targets, external_entry_labels, blocked_addrs)

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
    for idx, insn in enumerate(insns):
        addr = insn['addr']
        # Peephole: a folded load is dropped entirely (its value moves into the
        # following store); the store is rewritten to assign the literal directly.
        if idx in skip_loads:
            last_insn = insn
            continue
        # Split point: stop the current function and tail-call the wrapper.
        if addr in external_entry_labels:
            wname = wrapper_names.get(addr, f'FUN_{addr:04x}')
            lines.append(f'    {wname}(); return;')
            hit_split = True
            break   # do not translate addr or any subsequent instructions
        if idx in store_vals:
            lines.append(f'    /* {addr:04x} */')
            lines.append(f'    {store_vals[idx]};')
            last_insn = insn
            continue
        if addr in local_targets:
            # Inject spin-wait hook at the label itself so ALL paths
            # reaching this label (including unconditional JMPs) get it.
            hook = SPINWAIT_HOOKS.get(addr, '')
            if hook:
                lines.append(f'L_{addr:04x}:; {hook}')
            else:
                lines.append(f'L_{addr:04x}:;')
        pre = PRE_INSN_HOOKS.get(addr, '')
        if pre:
            lines.append(f'    {pre}')
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
    write_atari_mem_header(OUT_MEM)
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
        '#define ROF_MEM_ALIASES  /* enable bare lvalue aliases (level_stage = ...) */',
        '#include "mem.h"   /* MEM_<name> offsets + bare aliases for named RAM/state */',
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
