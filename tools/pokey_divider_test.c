/* pokey_divider_test — host proof for PlatformAmiga.cpp's pokey_divider().
 *
 * WHAT IT PROVES.  pokey_divider() resolves a POKEY channel's frequency divider from
 * (channel, AUDF, AUDCTL), including AUDCTL's two 16-bit chains.  Everything audible
 * downstream is a function of the master-clock tick count it implies — the Paula period
 * (pokey_period_compute) and the poly4/poly5/poly9 distortion strides all take
 * divider * base_div — so this proof compares exactly that product against an oracle.
 *
 * THE ORACLE IS INDEPENDENT, not the old code.  It is transcribed from atari800's accurate
 * POKEY model, tmp/atari800/src/mzpokeysnd.c (Update_c0divstart .. Update_c3divstart), whose
 * rules are:
 *   unchained, 1.79 MHz  : AUDF + 4                     (ticks; the base divider is bypassed)
 *   unchained, divided   : (AUDF + 1) * mdivk           (mdivk = 114 if AUDCTL bit0 else 28)
 *   chained, HIGH half   : AUDF_lo + 256*AUDF_hi + 7    at 1.79 MHz
 *                          (AUDF_lo + 256*AUDF_hi + 1) * mdivk  on a divided clock
 *   chained, LOW half    : 256 at 1.79 MHz, else 256 * mdivk  — the low counter no longer
 *                          reloads from AUDF; it free-runs the full byte wrap
 * The clock select and the join bit belong to the PAIR: CH1_179 $40 / CH1_CH2 $10 govern
 * ch0+ch1, CH3_179 $20 / CH3_CH4 $08 govern ch2+ch3.
 *
 * (mzpokeysnd also reloads the low half with AUDF_lo on the single cycle the 16-bit pair
 * itself reloads — c0divstart_p.  That once-per-16-bit-period jitter is not representable in
 * a fixed-period wavetable and is deliberately outside both the model and this proof.  The
 * older, cruder pokeysnd.c "rf" model does not model the low half's byte wrap at all.)
 *
 * Exhaustive: all 256 AUDCTL values x 4 channels x all 65536 (AUDF_lo, AUDF_hi) pairs.
 *
 * !! SNAPSHOT !!  `subject_divider` below is a verbatim copy of PlatformAmiga.cpp's
 * pokey_divider().  Green proves the resolver is right, NOT that the shipping source still
 * matches this copy — re-read the two side by side before leaning on an old green.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

/* ---- SNAPSHOT of PlatformAmiga.cpp: pokey_divider() + its two AUDCTL macros ---------- */
static uint8_t pokey[16];   /* the register shadow it reads AUDF_lo out of */

#define POKEY_PAIR_179(ch)    ((ch) & 2u ? 0x20u : 0x40u)
#define POKEY_PAIR_CHAIN(ch)  ((ch) & 2u ? 0x08u : 0x10u)

static uint32_t subject_divider(uint8_t ch, uint8_t audf, uint8_t audctl,
                                uint16_t* out_bd, bool* out_179)
{
    const uint8_t lo_ch  = (uint8_t)(ch & 2u);
    const bool    hi_half = (ch & 1u) != 0u;
    const bool    chained = (audctl & POKEY_PAIR_CHAIN(ch)) != 0u;
    bool     use_179;
    uint32_t divider;

    if (chained) {
        use_179 = (audctl & POKEY_PAIR_179(ch)) != 0u;
        divider = hi_half ? ((uint32_t)pokey[lo_ch * 2] + 256u * (uint32_t)audf + (use_179 ? 7u : 1u))
                          : 256u;
    } else {
        use_179 = !hi_half && (audctl & POKEY_PAIR_179(ch)) != 0u;
        divider = (uint32_t)audf + (use_179 ? 4u : 1u);
    }
    if (out_179) *out_179 = use_179;
    *out_bd  = use_179 ? 1u : ((audctl & 0x01u) ? 114u : 28u);
    return divider;
}

/* SNAPSHOT of PlatformAmiga.cpp: poly_stride_mod() — the poly-stride residue, whose fast path
 * relies on the unchained divider never exceeding 574. */
static uint16_t subject_stride_mod(uint32_t divider, uint16_t bd, uint16_t m)
{
    if (divider <= 574u)
        return (uint16_t)(((uint32_t)(uint16_t)divider * bd) % m);
    return (uint16_t)((((uint32_t)(divider % m)) * (bd % m)) % m);
}

/* ---- ORACLE: atari800 mzpokeysnd.c, transcribed --------------------------------------- */
static uint32_t oracle_ticks(int ch, uint8_t audf_lo, uint8_t audf_hi, uint8_t audctl)
{
    const uint32_t mdivk = (audctl & 0x01u) ? 114u : 28u;
    const int      pair  = ch >> 1;                       /* 0 = ch0+ch1, 1 = ch2+ch3 */
    const bool     hf    = (audctl & (pair ? 0x20u : 0x40u)) != 0u;   /* CH3_179 : CH1_179 */
    const bool     join  = (audctl & (pair ? 0x08u : 0x10u)) != 0u;   /* CH3_CH4 : CH1_CH2 */
    const bool     high  = (ch & 1) != 0;
    const uint8_t  own   = high ? audf_hi : audf_lo;      /* this channel's own AUDF */

    if (join) {
        if (high)
            return hf ? ((uint32_t)audf_lo + 256u * audf_hi + 7u)
                      : ((uint32_t)audf_lo + 256u * audf_hi + 1u) * mdivk;
        return hf ? 256u : 256u * mdivk;
    }
    if (hf && !high) return (uint32_t)own + 4u;           /* only ch0/ch2 can select 1.79 MHz */
    return ((uint32_t)own + 1u) * mdivk;
}

int main(void)
{
    long long cases = 0;
    for (int audctl = 0; audctl < 256; audctl++) {
        for (int lo = 0; lo < 256; lo++) {
            for (int hi = 0; hi < 256; hi++) {
                for (int pair = 0; pair < 2; pair++) {
                    /* AUDF1/AUDF2 for the ch0+ch1 pair live at pokey[0]/pokey[2];
                       AUDF3/AUDF4 for ch2+ch3 at pokey[4]/pokey[6]. */
                    pokey[pair * 4 + 0] = (uint8_t)lo;
                    pokey[pair * 4 + 2] = (uint8_t)hi;
                    for (int half = 0; half < 2; half++) {
                        int ch = pair * 2 + half;
                        uint16_t bd; bool u179;
                        uint32_t div = subject_divider((uint8_t)ch,
                                                       (uint8_t)(half ? hi : lo),
                                                       (uint8_t)audctl, &bd, &u179);
                        uint32_t got  = div * (uint32_t)bd;
                        uint32_t want = oracle_ticks(ch, (uint8_t)lo, (uint8_t)hi, (uint8_t)audctl);
                        if (got != want) {
                            printf("MISMATCH audctl=$%02X ch=%d AUDFlo=$%02X AUDFhi=$%02X: "
                                   "got %u ticks (div=%u bd=%u), want %u\n",
                                   audctl, ch, lo, hi, got, div, bd, want);
                            return 1;
                        }
                        /* bd must be 1 exactly when the channel runs off the 1.79 MHz clock */
                        if ((bd == 1u) != u179) {
                            printf("BD/179 DISAGREE audctl=$%02X ch=%d: bd=%u u179=%d\n",
                                   audctl, ch, bd, (int)u179);
                            return 1;
                        }
                        /* the poly4/poly5/poly9 stride residues must equal the exact product's,
                           on both sides of poly_stride_mod's 574 fast-path cutoff */
                        static const uint16_t mods[3] = { 15u, 31u, 511u };
                        for (int k = 0; k < 3; k++) {
                            uint16_t sg = subject_stride_mod(div, bd, mods[k]);
                            uint16_t sw = (uint16_t)(((uint64_t)div * bd) % mods[k]);
                            if (sg != sw) {
                                printf("STRIDE MISMATCH audctl=$%02X ch=%d div=%u bd=%u mod %u: "
                                       "got %u want %u\n", audctl, ch, div, bd, mods[k], sg, sw);
                                return 1;
                            }
                        }
                        cases++;
                    }
                }
            }
        }
    }
    printf("%lld cases, 0 mismatches\n", cases);
    return 0;
}
