// Apply names and comments from disasm/symbols.csv to the Ghidra project.
// Arg0 = path to symbols.csv
//@category Atari
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import java.io.*;
import java.util.*;

public class ApplyNames extends GhidraScript {

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        String csvPath = args.length > 0 ? args[0] : "disasm/symbols.csv";

        int applied = 0, skipped = 0;
        BufferedReader r = new BufferedReader(new FileReader(csvPath));
        String line;
        while ((line = r.readLine()) != null) {
            line = line.trim();
            if (line.isEmpty() || line.startsWith("#")) continue;
            String[] parts = line.split(",", 5);
            if (parts.length < 3) continue;

            String addrStr = parts[0].trim().replaceAll("^\\$", "");
            String name    = parts[1].trim();
            String type    = parts[2].trim();
            // parts[3] = is_hw, parts[4] = note (used as comment if present)
            String note    = parts.length >= 5 ? parts[4].trim() : "";

            long offset;
            try { offset = Long.parseLong(addrStr, 16); }
            catch (NumberFormatException e) { skipped++; continue; }

            Address a = toAddr(offset);
            try {
                // Set primary label (user-source so it survives re-analysis).
                if (!name.isEmpty()) {
                    Symbol existing = getSymbolAt(a);
                    if (existing == null || existing.getSource() != SourceType.USER_DEFINED) {
                        createLabel(a, name, true, SourceType.USER_DEFINED);
                    }
                }
                // Set plate comment from note.
                if (!note.isEmpty() && note.length() > 1) {
                    CodeUnit cu = currentProgram.getListing().getCodeUnitAt(a);
                    if (cu != null) {
                        String cur = cu.getComment(CodeUnit.PLATE_COMMENT);
                        if (cur == null || cur.isEmpty())
                            cu.setComment(CodeUnit.PLATE_COMMENT, note);
                    }
                }
                applied++;
            } catch (Exception e) {
                println("SKIP " + addrStr + " (" + name + "): " + e.getMessage());
                skipped++;
            }
        }
        r.close();
        println("ApplyNames: applied=" + applied + " skipped=" + skipped);
    }
}
