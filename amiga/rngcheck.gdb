# Level-determinism check for `make FIXED_RNG=1` (amiga/Makefile).
#
# The level IS the two maps the fresh-start seeding builds from RANDOM ($D20A):
#   $0900 = marker / height-map grid (256 cells)   <- intro_random_setup + intro_unmark_random_cells
#   $0A00 = object map (256 cells, $64 = pilot)    <- intro_seed_object_map
# Our $D20A is a READ-CLOCKED LFSR from a fixed power-on seed, so which level you get is decided
# purely by how many RANDOM reads happened before that seeding.  Any code change shifts the
# free-running main loop against the real-time 50Hz VBI, shifts that read count, and silently
# changes the level — which is what made cross-build beam-tick numbers incomparable.
#
# Run the SAME command on two builds that differ in timing (e.g. plain vs RASTER_C=1):
#   GDBSCRIPT=rngcheck.gdb ./diag_run.sh 40
# WITHOUT FIXED_RNG the checksums differ; WITH it they must match exactly.
set pagination off
set confirm off
break intro_random_setup
continue
printf "SEED-POINT HIT: lfsr=0x%05x fresh$0627=%d stage$006D=%d vbi=%u\n", \
  rof_lfsr_state, mem[0x0627], mem[0x006D], g_vbiCount
delete
continue
echo \n==== SIGINT ====\n
set $g900 = 0
set $g0a00 = 0
set $n900 = 0
set $nobj = 0
set $i = 0
while $i < 256
  set $b9 = mem[0x0900 + $i]
  set $ba = mem[0x0A00 + $i]
  # order-sensitive rolling checksum (position matters, not just the multiset)
  set $g900  = (($g900  * 33) + $b9 + $i) & 0xFFFFFFFF
  set $g0a00 = (($g0a00 * 33) + $ba + $i) & 0xFFFFFFFF
  if $b9 & 0x80
    set $n900 = $n900 + 1
  end
  if $ba != 0
    set $nobj = $nobj + 1
  end
  set $i = $i + 1
end
printf "LEVEL  grid$0900 sum=0x%08x marked=%d   objmap$0A00 sum=0x%08x placed=%d\n", \
  $g900, $n900, $g0a00, $nobj
printf "  lfsr=0x%05x  stage$006D=%d  fresh$0627=%d  vbi=%u\n", \
  rof_lfsr_state, mem[0x006D], mem[0x0627], g_vbiCount
detach
quit
