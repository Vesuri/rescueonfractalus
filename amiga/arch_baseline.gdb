set pagination off
set confirm off
continue
printf "\n==== SIGINT ====\n"
printf "=== ARCHITECTURE BASELINE ===\n"
printf "g_vbiCount        = %u\n", g_vbiCount
printf "g_dsEntryN        = %u (display_setup invocations)\n", g_dsEntryN
printf "g_dsEntryVbi      = %u (vbi of most-recent ds entry)\n", g_dsEntryVbi
printf "g_iterCount       = %u (game_main_loop_body flight-loop iterations)\n", g_iterCount
printf "g_fadeLoopVbi     = %u\n", g_fadeLoopVbi
printf "g_bc01N           = %u (event-01 loads)\n", g_bc01N
printf "g_bcResetN        = %u (sfx_engine_reset calls)\n", g_bcResetN
printf "g_l634fPath       = %u (last L_634f path 1/2/3)\n", g_l634fPath
set $i = 0
while $i < g_bc01N
  printf "  ev01[%u] vbi=%u 0004=%u 006d=%u 006c=0x%02x 0644=0x%02x range=%u D01F=0x%02x fresh=%u path=%u\n", $i, g_bc01Vbi[$i], g_bc01Ctx[$i][0], g_bc01Ctx[$i][1], g_bc01Ctx[$i][2], g_bc01Ctx[$i][3], g_bc01Ctx[$i][4], g_bc01Ctx[$i][5], g_bc01Ctx[$i][6], g_bc01Ctx[$i][7]
  set $i = $i + 1
end
