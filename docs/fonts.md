# Fonts and character graphics in Rescue on Fractalus

`tools/export_font_strips.py` generates fixed-slot, single-row PNG strips in the current working
directory at 4× nearest-neighbour scale. Generated PNGs are intentionally not stored in the
repository. They use transparent backgrounds. Slot order is the game's native character-code
order, not Unicode or ASCII order, so intentional spaces and unused shapes remain visibly empty.

## Atari system font

`atari-system-font.png` contains the 128 physical 8×8 glyphs at Atari OS address `$E000` in
codes `$00-$7F`. The high code bit used by Atari video hardware is a display attribute rather
than another stored glyph, so inverse-video duplicates are not added.

The game uses this set unusually: it reads glyph bitmaps from OS ROM and draws them into the
launch/door-field bitmap. This produces the `LEVEL` or `DROID` heading and its following numeric
or four-character readout in the green field seen before launch and between levels. It is not the
font used by the title, cockpit message line, results, or high-score table.

## Rescue on Fractalus text font

`rof-text-font.png` contains the game's 64 physical 8×8 glyphs at runtime address `$0400`, codes
`$00-$3F`. This is a custom, heavier redraw of the Atari screen-code repertoire, not merely the
Atari ROM font. In ANTIC modes 6 and 7 the upper two character-code bits select a colour; the lower
six bits select one of these glyphs. Mode 6 doubles each glyph horizontally on screen, and mode 7
also doubles it vertically.

It is used for:

- the `RESCUE ON FRACTALUS!` title/attract card, copyright line, starting-level information,
  results and ranking text;
- the high-score table and initials-entry screen;
- the top cockpit status/message line during standby and flight, including messages such as
  `MANUAL`, `TRANSMITTING`, `AIRLOCK OPENED`, `FIRE BOOSTERS`, and `LEVEL COMPLETE`;
- the small score in the top bar.

## Cockpit/dashboard tile set

`rof-cockpit-tiles.png` contains the 128 physical 8×8 tiles at runtime address `$3800`, codes
`$00-$7F`. These bytes are ANTIC mode-4 two-bit pixel pairs, so the export uses four neutral
values (transparent plus three greys) rather than pretending they are a one-colour alphabet.
The screen character's high bit selects an alternate colour for one of the pixel values; that
attribute is not a second stored tile and is not duplicated in the strip.

This set is primarily graphic construction material, not prose text. It builds the cockpit frame
and instrument faces, the compass, bar-gauge and indicator states, targeting-scope details,
lock-on lights, and the changing numeric cells across the bottom dashboard.

On Amiga, startup expands every mode-4 character/attribute combination, mode-D byte, compass
state, and cockpit-top text mask from the cartridge-loaded glyphs into a BSS planar atlas.
Runtime updates then select an entry and copy its already interleaved four-plane bytes. The
faithful toggle-off renderer retains the original three-plane decoder as its comparison path.

## Derived cockpit digits

`rof-cockpit-digits.png` shows the visually distinct decimal numerals assembled by the table at
`$4AE3`. Each numeral is a 2×2 arrangement of tiles from the `$3800` dashboard set. Slots 0-9 are
defined; the fixed slots A-F are transparent because the renderer only supports decimal BCD
digits. These numerals are used for Range To Pilot, Enemies Destroyed, and Pilot Quota/Rescued.

## Atari display scaling

The stored character data is always eight rows high, but the on-screen geometry depends on the
ANTIC/GTIA mode displaying it:

| Source | Display path | Stored or logical shape | On-screen cell or shape |
|---|---|---:|---:|
| RoF text font | ANTIC mode 6 | 8×8, 1 bpp | 16×8 |
| RoF text font | ANTIC mode 7 | 8×8, 1 bpp | 16×16 |
| Cockpit tiles | ANTIC mode 4 | 4×8, 2 bpp colour samples | 8×8 |
| Derived cockpit digits | 2×2 mode-4 cells | 8×16, 2 bpp colour samples | 16×16 cell area |
| Atari system font | software blit into a GTIA mode-10 field | 8×8 logical pixels | 32×8 |

Mode 6 displays each of the eight glyph bits twice horizontally. It is used for the normal-height
title-card rows, the high-score column header and entries, and the cockpit's top status/message and
score line. Mode 7 applies the same horizontal doubling and repeats each source scanline twice
vertically. It is used for the large first/banner row of the full-screen title/results card and
for `HIGHEST SCORING ACES` on the high-score screen.

Mode 4 does not treat a charset byte as eight independent monochrome pixels. Each row contains
four two-bit colour values, and ANTIC displays every value two pixels wide. This applies to the
top label/compass strip and the entire dashboard. There is no vertical scaling: one glyph row is
one scanline. The apparent cockpit numeral face is assembled in software/data as two columns by
two rows of these already-expanded mode-4 cells; the hardware adds no further scaling.

The Atari system glyphs are not sent through an ANTIC text mode in this game. The game reads each
8×8 ROM glyph and plots it into the `$2000` launch/door field one logical pixel at a time. That
field is displayed in GTIA mode 10, where each four-bit field pixel occupies four 320-wide output
pixels. Thus `LEVEL`, `DROID`, and their readout glyphs are four times wider but retain their eight
scanline height.

Character colour-selection bits and the mode-4 alternate-colour/high bit change palette choice,
not geometry. The generator's final 4× nearest-neighbour enlargement is only for PNG readability
and is separate from all of the Atari scaling above.

## Higher-quality replacement assets

The original 8×8 glyph designs are sound; their coarse appearance comes from displaying the same
source pixels at several incompatible hardware scales. A faithful higher-resolution replacement
should therefore preserve the character codes and designs but provide three separately authored
faces rather than scaling one replacement at runtime:

- **16×8 RoF text**, replacing ANTIC mode-6 horizontal doubling for normal text, messages, scores
  and high-score rows;
- **16×16 RoF text**, replacing ANTIC mode-7 horizontal and vertical doubling for large headings;
- **32×8 Atari-system text**, replacing the GTIA mode-10 expansion used by `LEVEL nn` and the
  `DROID A0A0`-pattern display.

These should be treated as distinct raster assets even where they depict the same character. In
particular, deriving 16×16 by doubling the 16×8 face would preserve the very vertical scaling
artifact the replacement is intended to remove. The `$3800` cockpit set is different: it is
purpose-built multicolour tile artwork whose 2-bit samples define its intended 8×8 display cells,
not a monochrome font accidentally enlarged by a text mode.

## Minimal Atari system-font subset

Only **36 of the 128 physical Atari system glyphs are reachable: `0-9` and `A-Z`**.

| Screen-code range | Glyphs | Why reachable |
|---|---|---|
| `$10-$19` | `0-9` | The normal `LEVEL nn` display and the two random Demo Droid digits |
| `$21-$3A` | `A-Z` | `LEVEL`, `DROID`, and the two random Demo Droid letters |

There are no system-font spaces or punctuation in this rendering path: spacing and the surrounding
frame are plotted directly into the destination bitmap. The fixed label table at `$6E23` contains
only `LEVEL` and `DROID`. The normal level-number path can produce every decimal digit. In Demo mode,
`random_alpha_index` rejection-samples all 26 letter codes `$21-$3A`, and `random_digit`
rejection-samples all ten digits; the four-character readout is letter-digit-letter-digit. Thus no
alphanumeric glyph can be removed while Demo Droid remains available.

If Demo mode were deliberately excluded, the subset would be 17 glyphs: `0-9` plus `D E I L O R V`.
With the current fixed `$E000 + code*8` addressing and a simple prefix overlay, trimming the file
without changing the loader can only reduce it to `$1D8` bytes (through the final row of code `$3A`,
`Z`). A genuinely packed 36-glyph asset would be 288 bytes, but would require scattering the digit
and letter ranges into their original addresses or changing the glyph lookup.

## Minimal RoF text-font subset

For the v4.1-derived `$0400` font, **52 of the 64 physical glyphs are reachable**:

| Screen code(s) | Glyphs or purpose | Source of use |
|---|---|---|
| `$00` | space | Text, tables, initials |
| `$01` | `!` | Title and cockpit messages |
| `$07` | apostrophe | `IT'S THE ACE!` |
| `$0A-$0F` | `* + , - . /` | Initials input; `*`, `.`, and `/` also occur in messages/input |
| `$10-$19` | `0-9` | Scores, levels, dates, high scores and initials |
| `$1A-$1E` | `: ; < = >` | `:` in fixed labels; the others are accepted by the original initials translation table |
| `$20` | custom copyright symbol | Copyright/date title row |
| `$21-$3A` | `A-Z` | Fixed text, messages, names and initials |
| `$3F` | underline cursor | Initials-entry cursor |

The 12 unreachable slots are `$02-$06`, `$08-$09`, `$1F`, and `$3B-$3E`: the stored shapes
correspond to `" # $ % & ( ) ? [ \\ ] ^` in normal Atari screen-code terminology. Return and
backspace are controls and do not add glyphs; erasing writes the already-counted space glyph.

The original `$5E50` initials table can emit space, `* + , - . /`, `0-9`, `; < = >`, and `A-Z`.
The current Amiga keyboard map deliberately exposes a smaller punctuation subset: space,
`* + , . / ;`, plus all digits and letters. This does not shrink the game-data minimum above if
the original input behaviour remains in scope.

The semantic minimum occupies 416 bytes of glyph rows. It cannot be obtained by merely truncating
the existing `$0400` asset, because the initials cursor is glyph `$3F`, the final eight bytes of the
512-byte set. Packing or scattering the used glyphs, changing the lookup, or generating the cursor
would be required for an actual storage reduction.

## Provenance

The `$0400` and `$3800` data come from the repository's verified 64 KiB runtime image,
`disasm/rof_mem.bin`; the same bytes were checked against the saved Atari states for title,
standby, launch, and flight. The OS set comes from `amiga/assets/atari_charset.bin`.

The current Amiga build sources the game-specific rows through `rof_game_data`; only the Atari
OS character set remains embedded through `src/platform/amiga/incbin.s`:

| Export | Default embedded file | File offset(s) | Full-XEX file offset |
|---|---|---|---|
| Atari system font | `amiga/assets/atari_charset.bin` | `$0000-$03FF` | separate asset in both builds |
| RoF text font | user `rof.rom` package | reconstructed at `$0400-$05FF` | v4.1 data mapping |
| Cockpit tiles | user `rof.rom` package | reconstructed at `$3800-$3BFF` | v4.1 data mapping |
| Cockpit digit map | user `rof.rom` package | reconstructed at `$4AE3-$4B0A` | v4.1 data mapping |
| Amiga cockpit planar atlas | BSS | generated at startup from the rows above | consumed by Enhanced Graphics |

The retired `rof_boot_image.bin` was a sparse load stream rather than a flat memory image. Its four-byte
chunk headers hold a destination address and length, and zero bytes omitted from the stream are
supplied by the loader's initial RAM clear. Consequently, the two full character sets do not have
one contiguous offset in the default file. The retained payload fragments are:

- RoF text font: `$2333-$233D`, `$2342-$2364`, `$2369-$2386`, `$238B-$2391`,
  `$2396-$2514`, `$2519`.
- Cockpit tiles: `$25B4-$264D`, `$2652-$2653`, `$2658-$26D0`, `$26D5-$2706`,
  `$270B-$271B`, `$2720`, `$2725-$2885`, `$288A-$288C`, `$2891-$2990`.

The corresponding runtime destinations are `$0400-$05FF`, `$3800-$3BFF`, and `$4AE3-$4B0A`.
These offsets are retained only as provenance for the old proof; neither sparse nor full XEX is
an available shipping build now.

Regenerate all four images with `python3 tools/export_font_strips.py`.
