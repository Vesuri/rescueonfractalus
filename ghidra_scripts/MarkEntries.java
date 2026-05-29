// Mark the loader/init entry points discovered from the XEX INITAD vectors
// and disassemble from each so auto-analysis can follow control flow.
//@category Atari
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;

public class MarkEntries extends GhidraScript {
    @Override
    public void run() throws Exception {
        // addr, name  (game_entry is the final INITAD = $3CDE)
        // Includes interrupt handlers (VBI/DLI/IRQ) not reached by normal JSR flow.
        long[] addrs   = {
            0x3CDEL, 0x3C00L, 0x5000L, 0x1B30L, 0x1A97L, 0xB800L,
            /* VBI handlers (installed via VVBLKI $0222/3 OS shadow) */
            0x53CCL, 0x4FF5L, 0x52D7L,
            /* DLI handlers (installed via VDSLST $0200/1) */
            0x49EEL, 0x6CC2L,
            /* IRQ handler (installed via VIMIRQ $0216/7) */
            0x462AL
        };
        String[] names = {
            "game_entry", "loader_3C00", "stage_5000",
            "vbi_handler_attract", "init_1A97", "init_B800",
            "vbi_handler_1", "vbi_handler_2", "vbi_handler_game",
            "dli_handler_game", "dli_handler_game2",
            "irq_handler"
        };
        for (int i = 0; i < addrs.length; i++) {
            Address a = toAddr(addrs[i]);
            addEntryPoint(a);
            disassemble(a);
            createFunction(a, names[i]);
            println("entry: " + names[i] + " @ " + a);
        }
    }
}
