# Diagnostic: how far does the genuine station_init->game_entry chain get,
# and do frames pump?  Each `continue` is bounded by a breakpoint so the
# script terminates even if the attract later freezes.
set pagination off
set confirm off
set remotetimeout 90

break station_init
break game_entry
break RescueOnFractalus::pumpFrame

echo \n>>> running to station_init...\n
continue
echo \n=== at station_init ===\n
bt

echo \n>>> letting the attract pump frames (expect pumpFrame hits)...\n
# If station_init spins without pumping, this continue hangs (timeout catches it).
ignore 3 30
continue
echo \n=== reached 30 pumpFrame calls — frames ARE pumping in attract ===\n
printf "g_vbiCount=%u frameCounter=%u launchPhase=%d\n", g_vbiCount, 0, 0
bt

echo \n>>> now run toward game_entry (press START not available headless; checking if attract self-advances)...\n
ignore 3 600
continue
echo \n=== 600 more pumpFrame calls done ===\n
quit
