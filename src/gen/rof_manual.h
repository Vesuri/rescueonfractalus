#ifndef ROF_MANUAL_H
#define ROF_MANUAL_H
/* Forward declarations for hand-written functions in rof_manual.c */

/* DLI dispatchers */
void dli_handler_game(void);
void dli_handler_game2(void);
void dli_handler_cockpit(void);

/* DLI sub-handlers */
void dli_sub_4a0c(void);
void dli_sub_4a11(void);
void dli_sub_4a1f(void);
void dli_sub_4a40(void);
void dli_sub_4a78(void);
void dli_sub_4acd(void);
void dli_sub_6cd7(void);
void dli_sub_6cf1(void);
void dli_sub_6d0e(void);
void dli_sub_6d28(void);
void dli_sub_6d42(void);
void dli_sub_6d4f(void);
void dli_sub_6d67(void);
void dli_sub_6d7c(void);
void dli_sub_6d99(void);
void dli_sub_6da1(void);
void screen_page_swap(void);

/* ---- station-image dirty rectangles (scene 2) -------------------------------
 * The station cinematic's per-frame animation rewrites under 200 bytes of the 122-row
 * mode-F image at $0600 (stride 40): station_sub_1EB4's 102-byte column stripe and
 * station_chan_step's six small blocks.  A platform that mirrors mem[] into bitplanes
 * (the Amiga) decodes the field ONCE and then only needs those bytes back, so the two
 * twins record what they wrote here.  A full re-decode is ~14k table lookups = far past
 * one frame on a 7 MHz 68000, and a shadow compare of the whole field is barely cheaper,
 * so the exact rectangles are what makes the scene affordable.
 *
 * Rectangles, not a row range: the stripe is 102 rows x 1 byte and the blocks are up to
 * 7 x 12, so their UNION would be ~2.6 KB — thirteen times the real dirty area.
 * Overflowing the list (or a caller that cannot describe its write) sets `full`, and the
 * renderer then re-decodes everything.  Inert on SDL, which renders mem[] directly.    */
#define ROF_STATION_DIRTY_MAX 12
typedef struct {
    unsigned short addr;    /* first byte written (absolute Atari address)  */
    unsigned char  cols;    /* bytes per row                                */
    unsigned char  rows;    /* number of rows, each `addr` + 40 further down */
} RofStationDirty;
extern RofStationDirty g_stationDirty[ROF_STATION_DIRTY_MAX];
extern unsigned char   g_stationDirtyCount;
extern unsigned char   g_stationDirtyFull;
void rof_station_dirty(unsigned short addr, unsigned char cols, unsigned char rows);

#endif /* ROF_MANUAL_H */
