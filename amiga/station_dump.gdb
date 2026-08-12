# station_dump.gdb — dump the decoded GTIA-9 field bitmap + the live window row, so the frame
# the copper is showing can be reconstructed offline (tools/station_bmp.py) and compared with the
# SDL/atari800 render.  Cheaper and far more precise than eyeballing an emulator window.
#
# Run:  . ./env.sh && GDBSCRIPT=station_dump.gdb ./diag_run.sh <secs>
# Writes .run/station_field.bin (320x340, 4bp interleaved) and .run/station_meta.txt.
continue
echo \n==== SIGINT ====\n
set $sc = 'PlatformAmiga.cpp'::s_scene
set $p  = (unsigned char *)$sc->bootFieldBitmap->data
printf "scene=%u winRow=%u starRows=%u phase=%02x\n", \
  $sc->bootFieldScene, $sc->stationWindowRow, $sc->stationStarRows, mem[0x008B]
set logging file .run/station_meta.txt
set logging overwrite on
set logging redirect on
set logging enabled on
printf "winRow %u\nstarRows %u\nphase %u\n", \
  $sc->stationWindowRow, $sc->stationStarRows, mem[0x008B]
set logging enabled off
dump binary memory .run/station_field.bin $p ($p + 340*160)
echo dumped\n
