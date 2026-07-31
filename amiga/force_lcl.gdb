set pagination off
set confirm off
# One-shot: once flight is well underway, force the level-clear chain (level_or_state!=0 +
# level_cleared_flag=1) so setup_level_clear_state fires -> player_lives=2 -> flight-loop
# break -> display_setup re-invoke -> event $01 reload.  Confirms the causal model headlessly.
tbreak rof_native.c:10375 if g_iterCount > 15
commands
  printf "\n>>> FORCING level-clear at iter=%u vbi=%u (dsN=%u bc01N=%u lclN=%u)\n", g_iterCount, g_vbiCount, g_dsEntryN, g_bc01N, g_lclN
  set variable mem[0x0004] = 1
  set variable mem[0x2849] = 1
  continue
end
continue
printf "\n==== SIGINT ====\n"
printf "g_iterCount=%u g_dsEntryN=%u g_bc01N=%u g_lclN=%u g_vbiCount=%u\n", g_iterCount, g_dsEntryN, g_bc01N, g_lclN, g_vbiCount
set $i = 0
while $i < g_lclN && $i < 16
  printf "  lcl[%u] @vbi=%u lvl_or_state=%02x range=%02x clearedFlag=%02x lives_before=%02x 003A=%02x\n", $i, g_lclVbi[$i], g_lclCtx[$i][0], g_lclCtx[$i][1], g_lclCtx[$i][2], g_lclCtx[$i][3], g_lclCtx[$i][4]
  set $i = $i + 1
end
set $i = 0
while $i < g_bc01N && $i < 16
  printf "  ev01[%u] @vbi=%u path=%u range=%02x lvl_or_state=%02x\n", $i, g_bc01Vbi[$i], g_bc01Ctx[$i][7], g_bc01Ctx[$i][4], g_bc01Ctx[$i][0]
  set $i = $i + 1
end
