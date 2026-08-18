# station_audio.gdb — the Station scene's POKEY stream, and the v5.0 fixes in it.
# Needs the boot scenes IN: `make clean && make PROBES=1 SKIPBOOT=0` (PROBES alone implies
# SKIPBOOT).  Run: . ./env.sh && GDBSCRIPT=station_audio.gdb ./diag_run.sh 300
#
# Logs every AUDCTL / AUDF3 / AUDC1 / AUDC3 write from boot to well past Standby, which is
# exactly what v5.0 changes (docs/rom-v50-diff.md §4.8).  ~200 stops; the Logo's own sweep
# writes all four too, so read the Station by its RTCLOK, which station_init resets to 0:0.
# What a good run looks like:
#   AUDCTL=29 then 01, AUDF3=28 then 0a   <- station_init, then the $19AB hook overriding it
#   AUDC3 = a7 a6 a5 .. a0 with mask=a7   <- the missile blip, ~3x per scene, now audible
#   AUDC1 = a7 a6 .. a0 on 8 consecutive frames, then g_stationAudc3Mask still a7  <- the fade
# ⚠ Do NOT `break station_audio_ch1_init` to sample the init: GCC gives it 2 locations and
# gdb picks the one the Logo reaches, printing a pre-Station snapshot that looks like a
# missing hook.  Watch the register writes instead.
set width 0
break rof_pokey_write if reg == 1 || reg == 3 || reg == 4 || reg == 5 || reg == 8
commands 1
silent
printf "  D2%02x = %02x  RTCLOK=%02x:%02x mask=%02x vbi=%u\n", reg, val, mem[0x0013], mem[0x0014], g_stationAudc3Mask, 'PlatformAmiga.cpp'::g_vbiCount
continue
end

# screen_page_swap: hit #1 is initad_1A97's, hit #2 is the Station exit (after the fade).
break screen_page_swap
commands 2
silent
printf "SWAP: mask=%02x AUDCTL=%02x AUDC1=%02x AUDC2=%02x AUDC3=%02x AUDC4=%02x vbi=%u\n", g_stationAudc3Mask, 'PlatformAmiga.cpp'::pokey[8], 'PlatformAmiga.cpp'::pokey[1], 'PlatformAmiga.cpp'::pokey[3], 'PlatformAmiga.cpp'::pokey[5], 'PlatformAmiga.cpp'::pokey[7], 'PlatformAmiga.cpp'::g_vbiCount
continue
end
continue
