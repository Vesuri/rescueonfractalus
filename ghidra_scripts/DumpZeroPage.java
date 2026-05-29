// Enumerate every zero-page address ($00-$FF) and low-RAM address ($0400-$07FF)
// that is referenced by instructions, with read/write counts and a sample of
// the functions that use it. Output is a CSV for symbols.csv seeding.
// Arg0 = output path
//@category Atari
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import java.io.*;
import java.util.*;

public class DumpZeroPage extends GhidraScript {

    static class Entry {
        int reads, writes;
        Set<String> fns = new LinkedHashSet<>();
    }

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        String out = args.length > 0 ? args[0] : "zeropage.csv";

        Listing listing = currentProgram.getListing();
        Map<Long, Entry> map = new TreeMap<>();

        InstructionIterator it = listing.getInstructions(true);
        while (it.hasNext()) {
            Instruction ins = it.next();
            Function f = getFunctionContaining(ins.getAddress());
            String fname = f != null ? f.getName() : "?";
            for (int op = 0; op < ins.getNumOperands(); op++) {
                for (Object o : ins.getOpObjects(op)) {
                    if (o instanceof Address) {
                        long a = ((Address) o).getOffset();
                        // zero page + page-1 (stack) + page-4..7 (game RAM workspace)
                        if (!((a >= 0x0000 && a < 0x0200) || (a >= 0x0400 && a < 0x0800))) continue;
                        // exclude hardware / OS shadow (handled by DumpHwAccesses)
                        Entry e = map.computeIfAbsent(a, k -> new Entry());
                        if (isWrite(ins)) e.writes++; else e.reads++;
                        if (e.fns.size() < 8) e.fns.add(fname);
                    }
                }
            }
        }

        PrintWriter w = new PrintWriter(new BufferedWriter(new FileWriter(out)));
        w.println("addr,reads,writes,functions");
        for (Map.Entry<Long, Entry> me : map.entrySet()) {
            long a = me.getKey();
            Entry e = me.getValue();
            w.printf("$%04X,%d,%d,\"%s\"%n", a, e.reads, e.writes,
                     String.join(" | ", e.fns));
        }
        w.close();
        println("wrote " + out + " — " + map.size() + " zero/low-RAM addresses referenced");
    }

    boolean isWrite(Instruction ins) {
        String mn = ins.getMnemonicString().toUpperCase();
        return mn.equals("STA") || mn.equals("STX") || mn.equals("STY")
            || mn.equals("STZ") || mn.equals("INC") || mn.equals("DEC")
            || mn.equals("ASL") || mn.equals("LSR") || mn.equals("ROL")
            || mn.equals("ROR");
    }
}
