# Function rename backlog

Functions whose current name (in `disasm/symbols.csv` → propagated everywhere by the
transpiler) contradicts what the code actually does. Collected for a future batch rename.
**Do not hand-rename in generated files** — fix `disasm/symbols.csv` (the source of truth)
and regenerate. Each entry: address, current name, what it really does, suggested name.

| Addr | Current name | What it actually does | Suggested |
|------|--------------|------------------------|-----------|
| `$3FDE` | `terrain_lookup` | Draws the **compass heading indicator**: copies 4 glyph bytes from table `$4B0B` (indexed by heading `($281C + $3FF6[$2836]) & $FF`) into the compass cells `$32E3-$32E6` (the mode-4 line below the title text, screen x=144 y=32). Nothing to do with terrain. | `draw_compass_heading` |
| `$548D` | `update_gauge_digits` | Per-frame **SFX voice/gauge envelope engine**: runs `sfx_engine_step`, advances the 14 voice/gauge slots' freq/duration/priority envelopes (emits AUDF via `sfx_voice_write_freq`, re-queues finished slots), then **drains the `$0719` event ring** (`input_init` / `reorder_sprite_slot`). Has nothing to do with "digits". | `sfx_voice_envelope_tick` |
| `$3FFA` | `update_cockpit_digits_native` | Comment in NativeHandlers.cpp says it's a direct translation of **`startup_init` @ `$3FFA`** — name/address mismatch; it writes the cockpit 2×2 digit blocks but the canonical label is `startup_init`. Verify which is correct. | (confirm `$3FFA` identity) |
| `$5433` | `font_display_init` | flight_native.cpp comment: "**sfx_engine_reset** $5433 (mislabelled font_display_init in symbols.csv)". It resets the SFX engine, not fonts. | `sfx_engine_reset` |
| `$5367` | `sound_event_dispatch` | Named "sound event" but is the per-frame **scroll/animation priority dispatcher**: fires `step_accum_add_75` (tunnel ring), `scroll_terrain_columns`, `dl_index_dec`, or `scroll_terrain_dl` (door scroll) in priority order. Not a sound dispatcher. | `scroll_event_dispatch` |
| `$782A` | `copy_altitude_graphic_to_screen` | Copies the **Standby title text banner** — a 20-byte block selected by `$0091` (`$5A9F` "RESCUE ON FRACTALUS!" or `$5AB3` "©1985 LUCASFILM LTD") — into screen RAM `$32B7-$32CA`, and sets `$00D8=$44` for the copyright block. Nothing to do with "altitude": the name derives from `$0091` (`altitude_threshold`), which the SFX sequencer repurposes as the title-block selector during Standby. | `copy_title_text_block_to_screen` |

## Notes
- `$4229` `lock_on_indicator_tick` was *previously* mislabelled `update_gauge_digits`
  (already corrected in symbols.csv) — kept here only as context for the `$548D` confusion.
