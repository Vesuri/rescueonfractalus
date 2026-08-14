/* Link `.MEMF_CHIP` as an UNINITIALISED (chip BSS) hunk instead of a data hunk.
 *
 * Everything the port puts in chip RAM statically -- the five POKEY->Paula waveform
 * buffers behind SASCCompat.h's `__chip` (noise_buf, poly_dist_buf, poly5_wave,
 * poly4_wave, wave_pure) -- is FILLED AT RUNTIME by PlatformAmiga::audioInit().  GCC
 * still emits a section-attributed variable into a PROGBITS section whether or not it
 * has an initialiser, so those 14,654 bytes shipped in RoF as literal zeros:
 * measured 100.0% zero (14,652 of 14,654), i.e. 4.8% of the whole executable spent
 * storing nothing.
 *
 * `(NOLOAD)` marks the output section SHT_NOBITS, and elf2hunk turns a NOBITS
 * `.MEMF_CHIP` into HUNK_BSS with the MEMF_CHIP flag -- so AmigaDOS still AllocMem()s it
 * from chip RAM and still clears it (a BSS hunk is zeroed by LoadSeg), it just is not
 * stored in the file.  Verified against elf2hunk before adopting; the chip RAM
 * REQUIREMENT is unchanged, so the WHDLoad slave's CHIPMEMSIZE needs no retune.
 *
 * ⚠ THE TRAP THIS CREATES, AND WHY THERE IS A GUARD: a static initialiser on a `__chip`
 *   variable is now silently DISCARDED -- the linker drops the contents and the variable
 *   comes up zero at runtime.  wave_pure used to be `= { 0x7F, 0x81 }` and would have gone
 *   silently silent.  So amiga/Makefile runs ../tools/check_chip_bss.py over the objects
 *   before the link and fails the build if any object contributes a nonzero byte here.
 *
 * INSERT AFTER .data supplements the default linker script rather than replacing it, so
 * -Ttext=0 and every other default placement still applies.
 */
SECTIONS
{
  .MEMF_CHIP (NOLOAD) : { *(.MEMF_CHIP) }
} INSERT AFTER .data;
