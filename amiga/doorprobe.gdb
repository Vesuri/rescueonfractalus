# Which bitmap actually holds the door field when the hangar doors start parting?
#   cd amiga && make clean && make -j4 PROBES=1
#   . ./env.sh && GDBSCRIPT=doorprobe.gdb ./diag_run.sh 90
#
# The PROBES auto-launch presses START at the initial standby, so the headless run reproduces the
# exact sequence by itself -- no interactive test needed.  At the earliest (smallest-g2) doors frame
# the probe samples the top door band in BOTH candidate sources and reports whether each is blank:
#   doorScrollBitmap = what updateDoorsCopper reads now (decodeDoorScrollField's target)
#   viewportBitmap   = what it read before 2026-08-10, left behind when the level-select elevator
#                      scroll moved the door decode to doorScrollBitmap
# Measured with the fix in: doorScrollBitmap blank=0, viewportBitmap blank=1 -- the door content
# only ever existed in the former, and the doors used to open onto flat green because the copper
# was pointed at the latter.  A blank=1 on doorScrollBitmap means the field was never decoded
# (check the decode count on the second line) and the doors will be green again.
set pagination off
continue
echo \n==== SIGINT ====\n
printf "earliest doors frame (g2=%u):  doorScrollBitmap blank=%u   viewportBitmap blank=%u\n", g_doorTopG2, g_doorTopBlack, g_doorTopBlackVp
printf "doorScrollBitmap decodes=%u  (seen=%u)\n", g_dsDecodes, g_doorTopSeen
