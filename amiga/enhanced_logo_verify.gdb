# enhanced_logo_verify.gdb — exercise the built-in logo hook without WHDLoad.
#
# Build and run with:
#   make clean && make ENHANCED_LOGO=1
#   GDBSCRIPT=enhanced_logo_verify.gdb ./diag_run.sh 12
#
# The two dumps are compared with the generated assets after the emulator exits.
set pagination off
set breakpoint pending on
break rofBuiltInEnhancedLogo
continue

printf "enhanced logo hook: initial phase=%u\n", context->phase
set $bitmap = context->bitmap
finish
dump binary memory .run/enhanced_logo_initial.bin $bitmap ($bitmap + 16000)

continue
printf "enhanced logo hook: games phase=%u\n", context->phase
set $bitmap = context->bitmap
finish
dump binary memory .run/enhanced_logo_games.bin $bitmap ($bitmap + 16000)

detach
quit
