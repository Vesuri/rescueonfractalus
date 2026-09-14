#ifndef ROF_FLIGHT_TERRAIN_GEOMETRY_H
#define ROF_FLIGHT_TERRAIN_GEOMETRY_H

/* Flight terrain geometry.
 *
 * The Atari-compatible renderer produces 160 x 47 mode-D cells.  The Amiga
 * display window is 320 x 94 lores pixels.  Keeping both domains explicit is
 * essential: the former remains the gameplay/validation contract, while the
 * latter is the native renderer's output domain.
 *
 * This header is deliberately C-compatible; rof_native.c and the C++ display
 * backend both include it.  The hand-written assembler imports the tables
 * built from these constants rather than duplicating the values again.
 */
#define ROF_FLIGHT_SOURCE_WIDTH          160
#define ROF_FLIGHT_SOURCE_TERRAIN_ROWS    43
#define ROF_FLIGHT_SOURCE_BAND_ROWS         4
#define ROF_FLIGHT_SOURCE_ROWS             47

#define ROF_FLIGHT_PHYSICAL_WIDTH         320
#define ROF_FLIGHT_PHYSICAL_TERRAIN_ROWS   86
#define ROF_FLIGHT_PHYSICAL_BAND_ROWS        8
#define ROF_FLIGHT_PHYSICAL_ROWS            94

#define ROF_FLIGHT_PLANE_ROW_BYTES          40
#define ROF_FLIGHT_BITPLANES                  3
#define ROF_FLIGHT_ROW_STRIDE              120

#define ROF_FLIGHT_SOURCE_TO_PHYSICAL_X       2
#define ROF_FLIGHT_SOURCE_TO_PHYSICAL_Y       2

#if ROF_FLIGHT_SOURCE_WIDTH * ROF_FLIGHT_SOURCE_TO_PHYSICAL_X != ROF_FLIGHT_PHYSICAL_WIDTH
#error flight terrain horizontal geometry is inconsistent
#endif
#if ROF_FLIGHT_SOURCE_ROWS * ROF_FLIGHT_SOURCE_TO_PHYSICAL_Y != ROF_FLIGHT_PHYSICAL_ROWS
#error flight terrain vertical geometry is inconsistent
#endif
#if ROF_FLIGHT_PLANE_ROW_BYTES * ROF_FLIGHT_BITPLANES != ROF_FLIGHT_ROW_STRIDE
#error flight terrain bitplane geometry is inconsistent
#endif

#ifdef __cplusplus
extern "C" {
#endif
extern int g_flightEnhancedTerrain;
extern int g_flightTerrainYScale;
extern int g_flightPhysicalTerrainRows;
extern int g_flightPhysicalRows;
#ifdef __cplusplus
}
#endif

#endif
