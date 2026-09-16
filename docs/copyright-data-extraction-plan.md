# Externalising the original game data

## Decision

The release executable must not contain original *Rescue on Fractalus!* binary data.
The Atari XEGS 64 KB cartridge image (`rof.rom`, v5.0) is the user-supplied source for
the game data.  The port continues to implement the v4.1 computer version: only data
already established to be identical between v4.1 and v5.0 is taken from the cartridge.
Console input, the five console-specific glyph changes, version text, and other v5.0
behaviour changes are not adopted.

The Atari OS character set (`amiga/assets/atari_charset.bin`) is explicitly outside this
change and remains embedded.

This follows the Stunt Car Racer arrangement:

* the normal/release executable declares the original-data region as BSS;
* the WHDLoad installer copies the user's complete `rof.rom`;
* the slave reads only the required ranges from that ROM into the executable before entry;
* an explicit standalone build embeds an extracted package generated from a user-supplied
  ROM.

Neither `rof.rom` nor the generated package is committed or included in a release archive.

## Data contract

One versioned descriptor, retained in an initialised executable hunk, describes the data
region.  It contains a magic value, layout version, destination pointer, total size, and
mode.  The storage itself has identical symbols and offsets in both builds:

* **external-data build (default):** uninitialised BSS;
* **standalone build:** initialised from the generated package.

The descriptor lets the WHDLoad slave find the region without depending on link addresses.
The game validates the package before touching any supplied data and refuses to start if an
external-data executable is launched without the slave having populated it. WHDLoad rejects a
missing, wrong-sized, or wrong-data ROM before entry.

The package is a deterministic concatenation of three audited cartridge ranges. Its layout is
described outside the package by generated, copyright-free constants and the boot copy recipe.
Derived Amiga lookup data is generated into BSS at startup rather than stored as another
copyrighted payload.

## Source assets to retire

The migration covers all original-game data presently compiled into the executable:

* `amiga/assets/rof_boot_image.bin`;
* `amiga/assets/cockpit_planar.bin`;
* the generated Logo fields in `src/rof_logo_field.h`;
* the factory high-score bytes in `src/rof_hiscore_factory.h`.

The extraction audit also examines the final ELF/hunk data sections for copied signatures or
tables, so moving a byte array to another source file cannot accidentally satisfy the check.

## Implementation sequence

1. Define the package format and one shared range manifest.  Generate C and assembler
   constants from that manifest so the extractor, executable, and slave cannot drift.
2. Add an extractor which accepts only the known 65,536-byte XEGS image, verifies its SHA-256,
   copies the required ROM ranges, applies only the address/layout transformations needed by
   the v4.1 port, and writes deterministic output.  Add `--check` and `--describe` modes.
3. Replace the embedded sparse XEX image with the dual-mode data region and a staged loader
   backed by the package.  Preserve all four boot-stage memory snapshots expected by the
   v4.1 code.
4. Build the cockpit planar tables at startup from the loaded game glyphs.  Source the Logo
   fields and factory high-score block through the package rather than compiled arrays.
5. Make the Amiga default build external-data/BSS.  Add an explicit standalone target which
   requires `ROM=...`, runs the extractor, and embeds the ignored generated package.
6. Extend `RoFSlave.s` to locate the descriptor, validate `rof.rom`, and fill the BSS region
   with `resload_LoadFileOffset` before calling the executable.
7. Change the WHDLoad installer to ask for and copy the complete 64 KB `rof.rom`.  Keep the ROM
   out of `create_release.sh` and make release creation reject a standalone/data-bearing
   executable.
8. Update the README, tool documentation, build documentation, and WHDLoad manual/readme.

## Verification gates

* Extractor rejects wrong size/hash and reproduces byte-identical output on repeated runs.
* Host tests prove extracted and slave-style range copies produce the same package.
* The replacement loader reproduces the expected v4.1 memory at every boot-stage boundary;
  known v5.0-only differences must not appear.
* Standalone and externally populated builds reach Logo, Station, Standby, launch, and flight.
* Existing native validation and host proofs remain green.
* A release audit parses the executable hunks and proves that the retired game assets and
  known source signatures are absent, while the expected storage is BSS.
* The release archive contains no `rof.rom` and no generated package.

## Repository history

This change removes the derived assets from the current tree.  It does not rewrite existing
Git history; history removal would be a separate, explicitly authorised operation.

## Implementation result (2026-09-16)

Completed. `tools/rof_data_layout.py` is the shared range manifest and generates the C, GNU
assembler, and BASM constants. `tools/extract_rom_data.py` verifies the exact v5.0 ROM before
creating an ignored standalone package; `tools/verify_rom_recipe.py` proves all four v4.1 stage
hashes plus the runtime cockpit atlas and both Logo phases. The default Amiga ELF places
`rof_game_data` and `rof_cockpit_planar` in `.bss`, while `make standalone ROM=...` places only
the package in `.rodata`. `tools/audit_release_data.py` rejects that standalone form and is a
mandatory gate in `whdload/create_release.sh`.

The WHDLoad installer copies the complete ROM. The slave checks its 64 KB size, loads the three
ranges, verifies the package CRC32, and then marks the descriptor ready. The old sparse boot
image, baked cockpit atlas, Logo arrays, factory-score array, and their obsolete generators have
been removed from the current tree. `atari_charset.bin` remains embedded as agreed.

The standalone build also passed the on-target `amiga/data_smoke.gdb` check under headless
FS-UAE: it passed the descriptor/CRC gate, reconstructed memory, and reached scene
initialisation with ready marker `RDF!` and package size 42,726.
