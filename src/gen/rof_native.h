/* rof_native.h — typed C cores of the natively-reimplemented 6502 routines.
 *
 * These are the idiomatic-C halves of the VALIDATE_FUNCS routines: real
 * parameters and return values, no dependence on mem[]/cpu for argument
 * passing.  NAMING: each core is `<canonical>_core`, where <canonical> is the
 * matching `void <canonical>(void)` 6502-ABI shim (in rof_native.c) that the
 * transpiler/validate/register-convention boundary binds to and that just
 * marshals mem[]/cpu into the core.
 *
 * Both the C transpile (rof_native.c) and the hand-written C++ Amiga ports
 * include this header, so it carries C linkage for C++.
 *
 * Type source: includers must already provide uint8_t/uint16_t (rof_native.c
 * via <stdint.h>; the Amiga ports via the framework headers).  This header does
 * NOT pull <stdint.h> itself — the Amiga framework's SASCCompat.h defines the
 * fixed-width types incompatibly (int8_t == char), so a second definition clashes.
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* divide_16x16 @ $9D6F core — unsigned restoring 16-bit divide.
 * Domain: divisor in [1, 0x7FFF] and dividend < divisor (the in-game caller
 * scales the divisor above the dividend first).  See rof_native.c for the
 * derivation of the normalization-shifted remainder/divisor outputs. */
typedef struct {
    uint8_t  quotient;         /* (dividend * 256) / divisor, always < 256   */
    uint16_t remainder;        /* ((dividend * 256) % divisor) << k          */
    uint16_t shifted_divisor;  /* divisor << k (k = normalization shift count) */
} DivResult;
DivResult divide_16x16_core(uint16_t dividend, uint16_t divisor);

/* clear_terrain_column @ $AD5F core — clear one terrain column band + its
 * scattered object-table cells, starting at column offset startCol. */
void clear_terrain_column_core(uint8_t startCol);

/* bin_to_bcd @ $4E84 core — convert a binary byte to packed BCD by repeated
 * subtraction (faithful to the 6502: 8-bit (tens<<4) truncation when a >= 100).
 * Returns the packed BCD byte; writes the ones digit to *units and the tens to
 * *tens (the shim maps units->$00C1 and tens->cpu.Y). */
uint8_t bin_to_bcd_core(uint8_t a, uint8_t *units, uint8_t *tens);

/* copy_title_text_block_to_screen @ $782A core — Standby per-frame altitude
 * banner update.  entryY is the 6502 entry Y (stored to $0091 as the new gate).
 * Gated on $0091 (>= $C0, with an $00E2 sign check at exactly $C0); when it fires
 * it copies a 20-byte graphic from $5A9F+X (X=$27 if $0091 >= $E0 else $13, the
 * former also setting $00D8=$44) into the title line $32B7..$32CA. */
void copy_title_text_block_to_screen_core(uint8_t entryY);

#ifdef __cplusplus
}
#endif
