/* FlightProf.h — flight per-frame profiler.
 *
 * Two clocks:
 *  - RTCLOK (mem[$0013:$0014], 50 Hz, bumped by the real VBI) for the main-loop
 *    phases — at ~2 FPS each spans many VBIs so 20 ms resolution is ample.
 *  - the raster beam line (VHPOSR/VPOSR, ~63.56 us/line) for the VBI ISR body,
 *    which RTCLOK cannot time (it ticks the same clock the ISR drives).
 *
 * Accumulated across all flight frames into g_flightProf; per-frame value =
 * field / frames.  isrLines is in raster lines (×63.56 us = time); isr per frame
 * = isrLines * 63.56us / frames.  Never zeroed at run time: take the delta over a
 * window that is entirely in flight.  Read from the gdb stub: `x/9wu &g_flightProf`.
 *
 * No <stdint.h> (clashes with the framework's SASCCompat.h); C linkage for C++.
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* All fields 32-bit so isrLines can't overflow; read as `x/9wu &g_flightProf`. */
struct FlightProf {
    unsigned long terrain;     /* RTCLOK: native terrain pass (frame_setup+clear+draw+collision) */
    unsigned long stateEnemy;  /* RTCLOK: TRANSPILED game_state_update + enemy_check              */
    unsigned long render;      /* RTCLOK: renderViewportModeD (bitplane conversion) only          */
    unsigned long copper;      /* RTCLOK: per-frame copper-list build/update                      */
    unsigned long frames;      /* main-loop flight frames accumulated                             */
    unsigned long updateTot;   /* RTCLOK: whole scene.update()                                    */
    unsigned long renderTot;   /* RTCLOK: whole scene.render()                                    */
    unsigned long isrLines;    /* BEAM: raster lines elapsed across flight_vbi_native invocations */
    unsigned long isrCalls;    /* number of flight_vbi_native invocations (VBIs while flying)     */
    /* terrain sub-phase breakdown (RTCLOK, accumulated over frames) — locates the
       dominant cost within the ~218 ms terrain bucket.  terrain == sum of these.    */
    unsigned long tFrameSetup; /* RTCLOK: terrain_frame_setup $9E54                               */
    unsigned long tClear;      /* RTCLOK: clear_terrain_column_core $AD5F                          */
    unsigned long tDraw;       /* RTCLOK: terrain_draw_frame $A31E (fractal/projection/object loop)*/
    unsigned long tCollision;  /* RTCLOK: terrain_collision_and_silhouette $AE53                                  */
};
extern volatile struct FlightProf g_flightProf;

unsigned short flight_vbi_tick(void);  /* current 16-bit VBI count (RTCLOK $0013:$0014) */

#ifdef __cplusplus
}
#endif
