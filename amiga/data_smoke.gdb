# Standalone-data startup smoke test. Run after `make standalone ROM=../rof.rom`:
#   GDBSCRIPT=data_smoke.gdb ./diag_run.sh 14
# Reaching scene.initialize proves the descriptor/CRC gate passed and all four boot stages
# were reconstructed into mem[] before any renderer consumed them.
set pagination off
break RescueOnFractalus::initialize()
commands
  silent
  printf "DATA_SMOKE PASS ready=%08lx size=%lu\n", rof_data_descriptor.ready, rof_data_descriptor.size
  detach
  quit
end
continue
