set pagination off
set confirm off
tbreak rof_native.c:10409 if g_iterCount > 12
commands
  set variable g_forcePilotR1 = 1
  set variable mem[0x003D] = 3
  continue
end
continue
printf "\n==== SIGINT ====\n"
printf "iter=%u vbi=%u ev01_loads=%u\n", g_iterCount, g_vbiCount, g_bc01N
printf "DRAIN events total=%u  out-of-range(id>33)=%u  event-$01($81)=%u  pushes(v&7f==1)=%u\n", g_drainN, g_drainOOR, g_drain81, g_bcPush01N
echo --- distinct drained EVENT entries (bit7 set) seen, last 40: ---\n
set $i = 0
while $i < 40
  set $k = (g_drainIdx + 88 + $i) % 128
  if g_drainVbi[$k] != 0
    printf "%02x@%u[t%02x] ", g_drainEvt[$k], g_drainVbi[$k], g_drainTail[$k]
  end
  set $i = $i + 1
end
echo \n
