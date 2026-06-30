# Render-diff verifier for the hand-asm terrain rasterizer (asm-migration-plan Phase 2).
# Breaks at a DETERMINISTIC flight moment (the g_fdCalls'th renderFlightDirect, well into
# steady flight) and dumps the rasterizer's outputs: $260E (per-column max-height map, the
# rasterizer's direct output) + the displayed terrainBitmap (the final composite incl. the
# plane2 dots).  The asm build and the RASTER_C=1 build differ ONLY in the rasterizer's
# machine code, so byte-identical dumps at the same break prove equivalence.
# Requires PROBES=1 (g_fdCalls / g_terrainBmpAddr).
set pagination off
set confirm off
tbreak RescueOnFractalus::renderFlightDirect if g_fdCalls >= 150
continue
printf "BREAK fdCalls=%u vbi=%u bmp=%08x dot=%08x\n", g_fdCalls, g_vbiCount, g_terrainBmpAddr, g_flightDotPlane
dump binary memory .run/diff_heights.bin &mem[0x260E] (&mem[0x260E] + 0xD5)
dump binary memory .run/diff_terrain.bin g_terrainBmpAddr (g_terrainBmpAddr + 47*120)
echo dumped .run/diff_heights.bin .run/diff_terrain.bin\n
detach
quit
