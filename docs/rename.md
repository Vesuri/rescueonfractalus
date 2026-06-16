# Function rename backlog

Functions whose current name (in `disasm/symbols.csv` → propagated everywhere by the
transpiler) contradicts what the code actually does. Collected for a future batch rename.
**Do not hand-rename in generated files** — fix `disasm/symbols.csv` (the source of truth)
and regenerate. Each entry: address, current name, what it really does, suggested name.

| Addr | Current name | What it actually does | Suggested |
|------|--------------|------------------------|-----------|
| `$548D` | `update_gauge_digits` | Per-frame **SFX voice/gauge envelope engine**: runs `sfx_engine_step`, advances the 14 voice/gauge slots' freq/duration/priority envelopes (emits AUDF via `sfx_voice_write_freq`, re-queues finished slots), then **drains the `$0719` event ring** (`input_init` / `reorder_sprite_slot`). Has nothing to do with "digits". | `sfx_voice_envelope_tick` |
| `$3FFA` | `update_cockpit_digits_native` | Comment in NativeHandlers.cpp says it's a direct translation of **`startup_init` @ `$3FFA`** — name/address mismatch; it writes the cockpit 2×2 digit blocks but the canonical label is `startup_init`. Verify which is correct. | (confirm `$3FFA` identity) |
| `$5433` | `font_display_init` | flight_native.cpp comment: "**sfx_engine_reset** $5433 (mislabelled font_display_init in symbols.csv)". It resets the SFX engine, not fonts. | `sfx_engine_reset` |
| `$5367` | `sound_event_dispatch` | Named "sound event" but is the per-frame **scroll/animation priority dispatcher**: fires `step_accum_add_75` (tunnel ring), `scroll_terrain_columns`, `dl_index_dec`, or `scroll_terrain_dl` (door scroll) in priority order. Not a sound dispatcher. | `scroll_event_dispatch` |

## Notes
- `$4229` `lock_on_indicator_tick` was *previously* mislabelled `update_gauge_digits`
  (already corrected in symbols.csv) — kept here only as context for the `$548D` confusion.
