# boot_gap.gdb — the Logo -> Station handover window.  Answers ONE question: what is on screen
# while scene 2's entry rebuilds the SHARED Gtia9CopperList and re-decodes the SHARED field bitmap?
#
# Plain `make` build only (PROBES=1/FPSCOUNT=1 imply SKIPBOOT=1 and skip the scenes under test).
#   GDBSCRIPT=boot_gap.gdb ./diag_run.sh 75
#
# Breakpoint-driven, not delay-driven: the window is a handful of frames inside a multi-second
# boot whose startup jitter is larger than the window (logo-station-port memory, §Traps).
# ⚠ Do NOT break on AmigaHardware::setCopperList itself — its args are optimised out (no symbol
# "copperList"), and buildStationSprites is inlined onto its address.  Break on the CALL SITES.
set width 0
set height 0

# 1 — the shared Gtia9CopperList being (re)built.  prevScene != bootScene => this is a scene entry,
#     and bootInst=1 means the list being rewritten is the one the copper is CURRENTLY executing.
break Gtia9CopperList::buildLayout
commands
silent
printf "buildLayout       vbi=%5u bootScene=%u prevScene=%u bootInst=%u emptyInst=%u top=%u rows=%u\n", g_vbiCount, g_bootScene, 'PlatformAmiga.cpp'::s_scene->bootFieldScene, 'PlatformAmiga.cpp'::s_scene->bootFieldCopperInstalled, 'PlatformAmiga.cpp'::s_scene->emptyCopperInstalled, topLines, rows
continue
end

# 2
break RescueOnFractalus::decodeLogoField
commands
silent
printf "decodeLogoField   vbi=%5u\n", g_vbiCount
continue
end

# 3
break RescueOnFractalus::decodeStationField
commands
silent
printf "decodeStation IN  vbi=%5u\n", g_vbiCount
continue
end

# 4 — renderBootScene's own install (the scene-entry tail).  vbi here minus "decodeStation IN"
#     = the number of displayed frames the entry spent working on the LIVE list + LIVE bitmap.
break RescueOnFractalus.cpp:5578
commands
silent
printf "install bootField vbi=%5u\n", g_vbiCount
continue
end

# 5 — the scene-entry BLANK step (blank the shared list/bitmap before rebuilding them)
break RescueOnFractalus.cpp:5540
commands
silent
printf "install empty(pre) vbi=%5u bootScene=%u prevScene=%u\n", g_vbiCount, g_bootScene, 'PlatformAmiga.cpp'::s_scene->bootFieldScene
continue
end

# 6 — the first post-entry station frame: the field is on screen from here
break RescueOnFractalus::decodeStationStars
commands
silent
printf "station live      vbi=%5u\n", g_vbiCount
disable 6
continue
end

# 7 — the black-until-ready hold's install (renderFrame, past the boot scenes)
break RescueOnFractalus.cpp:4261
commands
silent
printf "install empty     vbi=%5u bootScene=%u\n", g_vbiCount, g_bootScene
disable 7
continue
end

continue
echo \n==== SIGINT ====\n
printf "bootScene=%u vbi=%u\n", g_bootScene, g_vbiCount
detach
quit
