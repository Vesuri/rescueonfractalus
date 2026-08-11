# What sprite/playfield priority does the Standby actually run at?
# Build: make clean && make PROBES=1 FORCE_RETURN=1 ATTRACT_NOW=2 TITLE_START=1
# Run:   GDBSCRIPT=b2_probe.gdb ./diag_run.sh 300
#
# BPLCON2 ($DFF104) is write-only, and a CPU read of it returns the floating bus — only gdb's read
# returns the stored value (cross-check: BPLCON0 $DFF100 reads back exactly what the live list set).
# rof_b2_mark_pre/_post bracket the priority write at every Standby copper install:
#   pre  = what the Standby INHERITED from the previous scene's copper list
#   post = what setSpritePriority left
# 0024 = all sprites in front of the playfield (the energy gauge draws over the cockpit — the bug),
# 0009 = sprite pair 0 (canopy posts) in front, pairs 1+ (the gauge, sprite 2) BEHIND.
#
# Measured 2026-08-11: first-boot standby inherits 0009 (initialize just wrote it), the POST-MOTHER-
# SHIP standby inherits 0024 from the boost TunnelCopperList — that is where the bug enters — and
# every later Standby (incl. the one after the Title) then keeps whatever that one left.
set pagination off
break rof_b2_mark_pre
commands
  silent
  printf "standby install vbi=%-6u INHERITED BPLCON2=%04x   (live BPLCON0=%04x)\n", \
    g_vbiCount, *(unsigned short*)0xDFF104, *(unsigned short*)0xDFF100
  continue
end
break rof_b2_mark_post
commands
  silent
  printf "                          after write  =%04x\n", *(unsigned short*)0xDFF104
  continue
end
continue
echo \n==== SIGINT ====\n
printf "NOW: vbi=%u VV=%04x cop=%u BPLCON2=%04x  installs=%lu\n", \
  g_vbiCount, (mem[0x0222] | (mem[0x0223] << 8)), g_liveCopper, \
  *(unsigned short*)0xDFF104, g_b2MarkPreN
