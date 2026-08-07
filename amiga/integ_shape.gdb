# flight_control_integrate SHAPE — where its 15.3 t/firing actually goes.
#
#   make clean && make -j4 COMBAT=1 PROBES=1 PROFILE_NORING=1 NO_TDRAW_PROF=1 FIXED_RNG=1 INTEG_SHAPE=1
#   . ./env.sh && GDBSCRIPT=integ_shape.gdb ./diag_run.sh 200
#
# integ is 17% of the flight VBI and the VBI is ~33% of ALL wall clock, so this bucket is ~4.8%
# of the frame — the biggest never-examined block left inside the ISR.
#
# ⚠ Read these as SHARES, not absolutes.  Two reasons: (1) each region is timed by LAPPING one
# running beam stamp, so every bucket carries one rof_beam_line() of floor — g_inNop is an empty
# lap sampled once per firing and IS that floor; (2) the lap points block GCC's reordering, so
# the probed integ total runs above the 15.3 t/firing an unprobed build measures.
set pagination off
set confirm off

tbreak RescueOnFractalus::renderFrame if g_vbiCount >= 2600
continue
printf "=== snapshot in flight at vbi %u (level %u, $0623=%02x $0621=%02x) ===\n", \
  g_vbiCount, mem[0x6D], mem[0x623], mem[0x621]
set $s_n    = g_inCalls
set $s_isrn = g_flightProf.isrCalls
set $s_isrl = g_flightProf.isrLines
set $s_integ= g_pInteg
set $s_vbi  = g_vbiCount
set $s_head = g_inHead
set $s_disp = g_inDisp
set $s_levl = g_inLevel
set $s_thr  = g_inThr
set $s_attc = g_inAttc
set $s_ang  = g_inAng
set $s_pos  = g_inPos
set $s_hud  = g_inHud
set $s_lock = g_inLock
set $s_objv = g_inObjv
set $s_obj  = g_inObj
set $s_slot = g_inSlot
set $s_tail = g_inTail
set $s_nop  = g_inNop
set $s_cblip= g_inBlipCalls
set $s_cap  = g_inAutoP
set $s_car  = g_inAutoR
set $s_ctk  = g_inThrKick
set $s_ctc  = g_inThrClamp
set $s_cos  = g_inObjStep
set $s_col  = g_inObjLoad
set $s_cop  = g_inObjPos
set $s_cob  = g_inObjBox
set $s_cj   = g_inJitter
set $s_csi  = g_inSlotIdle
set $s_oscalls = g_osCalls
set $s_osacc = g_osAcc
set $s_oshit = g_osHit
set $s_oscell= g_osCell
set $s_oslerp= g_osLerp
set $s_ostail= g_osTail
set $s_osnop = g_osNop
set $s_oshc  = g_osHitCalls
set $s_oslc  = g_osLerpCalls
set $s_osex  = g_osExplode
set $s_oser  = g_osEarlyRet
set $s_blcalls = g_blCalls
set $s_blf   = g_blFetch
set $s_blb1  = g_blB1
set $s_blb2  = g_blB2
set $s_blb3  = g_blB3
set $s_blt   = g_blTail
set $s_blnop = g_blNop

tbreak RescueOnFractalus::renderFrame if g_vbiCount >= 5400
continue

set $n  = g_inCalls - $s_n
set $il = g_flightProf.isrLines - $s_isrl
set $ic = g_flightProf.isrCalls - $s_isrn
set $tot = g_pInteg - $s_integ
set $floor = g_inNop - $s_nop
printf "\n=== window vbi %u -> %u : %lu integ firings (%lu ISR firings) ===\n", \
  $s_vbi, g_vbiCount, $n, $ic
printf "  VVBLKI=$%02x%02x $3D=%02x  (must be $4ff5 / 00)\n", mem[0x223], mem[0x222], mem[0x3D]
printf "  handler = %lu.%02lu t/firing   integ (PROBED) = %lu.%02lu t/firing\n", \
  ($ic ? $il/$ic : 0), ($ic ? ((100*$il)/$ic)%100 : 0), \
  ($n ? $tot/$n : 0), ($n ? ((100*$tot)/$n)%100 : 0)
printf "  per-lap FLOOR (empty lap) = 0.%03lu t   x13 laps = %lu.%02lu t/firing of the total\n\n", \
  ($n ? (1000*$floor)/$n : 0), ($n ? (13*$floor)/$n : 0), ($n ? ((1300*$floor)/$n)%100 : 0)

# r <acc-delta> "label"  — raw t/firing, floor-corrected t/firing, and share of the probed total
define r
  printf "  %-34s %6lu.%02lu t/firing  net %6lu.%03lu  %3lu%%\n", $arg1, \
    ($n ? $arg0/$n : 0), ($n ? ((100*$arg0)/$n)%100 : 0), \
    ($n ? ($arg0 > $floor ? ($arg0-$floor)/$n : 0) : 0), \
    ($n ? ($arg0 > $floor ? ((1000*($arg0-$floor))/$n)%1000 : 0) : 0), \
    ($tot ? (100*$arg0)/$tot : 0)
end
r (g_inHead-$s_head) "head: steering + blip call"
r (g_inDisp-$s_disp) "disp: per-state dispatch"
r (g_inLevel-$s_levl) "level: pitch+roll auto-level"
r (g_inThr-$s_thr)  "thr: throttle kick + clamp"
r (g_inAttc-$s_attc) "attc: pitch/roll integ+clamp+mul"
r (g_inAng-$s_ang)  "ang: heading + rel_angle_scale"
r (g_inPos-$s_pos)  "pos: world pos/depth/canopy/snd"
r (g_inHud-$s_hud)  "hud: refresh_hud_field_0d (2x55FC)"
r (g_inLock-$s_lock) "lock: $5D + voice slot + 55FC"
r (g_inObjv-$s_objv) "objv: ring velocity + 2 accums"
r (g_inObj-$s_obj)  "obj: object step + collide"
r (g_inSlot-$s_slot) "slot: slot dispatch"
r (g_inTail-$s_tail) "tail: 7-frame history ring roll"
r (g_inNop-$s_nop)  "nop: EMPTY LAP (the floor)"

printf "\n  PATH COUNTS (per 100 firings)\n"
define c
  printf "    %-34s %5lu / 100 firings   (%lu)\n", $arg1, \
    ($n ? (100*$arg0)/$n : 0), $arg0
end
c (g_inBlipCalls-$s_cblip) "compute_target_blip_position"
c (g_inAutoP-$s_cap)  "pitch auto-level"
c (g_inAutoR-$s_car)  "roll auto-level"
c (g_inThrKick-$s_ctk)  "throttle kick (dial != 0)"
c (g_inThrClamp-$s_ctc) "throttle clamp ($5D != 0)"
c (g_inObjStep-$s_cos)  "object_step_and_collide"
c (g_inObjLoad-$s_col)  "load_velocity_from_param_block"
c (g_inObjPos-$s_cop)   "object_integrate_position"
c (g_inObjBox-$s_cob)   "check_object_in_target_box"
c (g_inJitter-$s_cj)    "terrain_jitter_column"
c (g_inSlotIdle-$s_csi) "slot idle (index already < 0)"

# ---- level 2: inside object_step_and_collide (the `obj` bucket) ----
set $n2 = g_osCalls - $s_oscalls
set $floor2 = g_osNop - $s_osnop
printf "\n  === object_step_and_collide: %lu calls, floor 0.%03lu t/lap ===\n", \
  $n2, ($n2 ? (1000*$floor2)/$n2 : 0)
define r2
  printf "    %-32s %6lu.%02lu t/call   net %6lu.%03lu\n", $arg1, \
    ($n2 ? $arg0/$n2 : 0), ($n2 ? ((100*$arg0)/$n2)%100 : 0), \
    ($n2 ? ($arg0 > $floor2 ? ($arg0-$floor2)/$n2 : 0) : 0), \
    ($n2 ? ($arg0 > $floor2 ? ((1000*($arg0-$floor2))/$n2)%1000 : 0) : 0)
end
r2 (g_osAcc-$s_osacc)  "acc: 3 position accumulators"
r2 (g_osHit-$s_oshit)  "hit: check_player_proximity_hit"
r2 (g_osCell-$s_oscell) "cell: index + explode test"
r2 (g_osLerp-$s_oslerp) "lerp: sample_terrain_height_bilerp"
r2 (g_osTail-$s_ostail) "tail: collision resolve (rare)"
r2 (g_osNop-$s_osnop)  "nop: EMPTY LAP (the floor)"
printf "    hitCalls=%lu lerpCalls=%lu explode=%lu earlyRet=%lu\n", \
  (g_osHitCalls-$s_oshc), (g_osLerpCalls-$s_oslc), (g_osExplode-$s_osex), (g_osEarlyRet-$s_oser)

# ---- level 2: inside sample_terrain_height_bilerp (ALL callers: obj + proj) ----
set $n3 = g_blCalls - $s_blcalls
set $floor3 = g_blNop - $s_blnop
printf "\n  === sample_terrain_height_bilerp: %lu calls (all callers), floor 0.%03lu t/lap ===\n", \
  $n3, ($n3 ? (1000*$floor3)/$n3 : 0)
define r3
  printf "    %-32s %6lu.%02lu t/call   net %6lu.%03lu\n", $arg1, \
    ($n3 ? $arg0/$n3 : 0), ($n3 ? ((100*$arg0)/$n3)%100 : 0), \
    ($n3 ? ($arg0 > $floor3 ? ($arg0-$floor3)/$n3 : 0) : 0), \
    ($n3 ? ($arg0 > $floor3 ? ((1000*($arg0-$floor3))/$n3)%1000 : 0) : 0)
end
r3 (g_blFetch-$s_blf) "fetch: 4 corner heights"
r3 (g_blB1-$s_blb1)   "blend 1 (row 0, horizontal)"
r3 (g_blB2-$s_blb2)   "blend 2 (row 1, horizontal)"
r3 (g_blB3-$s_blb3)   "blend 3 (vertical)"
r3 (g_blTail-$s_blt)  "tail: R += R>>4 + R>>5"
r3 (g_blNop-$s_blnop) "nop: EMPTY LAP (the floor)"
detach
quit
