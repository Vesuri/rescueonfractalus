# Stage-1 verifier: dump the flight terrainBitmap chip RAM so the decoder can
# reconstruct/diff the displayed bitplanes headlessly.  Requires out/RoF.exe built
# with PROBES=1 (g_terrainBmpAddr only exists there).  Driven by dump_terrain.sh,
# which SIGINTs the `continue` once flight has been reached.
continue
echo \n==== SIGINT ====\n
printf "vbi=%u VVBLKI=%02x%02x bmp=%08x\n", g_vbiCount, mem[0x0223], mem[0x0222], g_terrainBmpAddr
# terrainBitmap: 3 interleaved bitplanes, 320px => 40 bytes/plane/scanline = 120/scanline.
# The convert writes 47 mode-D rows (one interleaved scanline each) = 47*120 = 5640 bytes.
dump binary memory .run/terrain.bin g_terrainBmpAddr (g_terrainBmpAddr + 47*120)
# Also dump the source GTIA field mem[$1070] (47 rows x 96 stride = 4512) + $260E heights.
dump binary memory .run/field1070.bin &mem[0x1070] (&mem[0x1070] + 47*96)
dump binary memory .run/heights260e.bin &mem[0x260E] (&mem[0x260E] + 0xD5)
echo dumped .run/terrain.bin field1070.bin heights260e.bin\n
detach
quit
