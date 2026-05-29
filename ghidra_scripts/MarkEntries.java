// Mark the loader/init entry points discovered from the XEX INITAD vectors
// and disassemble from each so auto-analysis can follow control flow.
//@category Atari
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;

public class MarkEntries extends GhidraScript {
    @Override
    public void run() throws Exception {
        // addr, name  (game_entry is the final INITAD = $3CDE)
        long[] addrs   = { 0x3CDEL, 0x3C00L, 0x5000L, 0x1B30L, 0x1A97L, 0xB800L };
        String[] names = { "game_entry", "loader_3C00", "stage_5000",
                           "code_1B30", "init_1A97", "init_B800" };
        for (int i = 0; i < addrs.length; i++) {
            Address a = toAddr(addrs[i]);
            addEntryPoint(a);
            disassemble(a);
            createFunction(a, names[i]);
            println("entry: " + names[i] + " @ " + a);
        }
    }
}
