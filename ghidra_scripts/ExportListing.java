// Export the full disassembly listing (address, bytes, mnemonic, comments)
// plus a function summary, to a text file. Arg0 = output path.
//@category Atari
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import java.io.*;

public class ExportListing extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        String out = args.length > 0 ? args[0] : "listing.txt";
        Listing listing = currentProgram.getListing();
        PrintWriter w = new PrintWriter(new BufferedWriter(new FileWriter(out)));

        // Function summary first.
        FunctionIterator fns = listing.getFunctions(true);
        int fcount = 0;
        StringBuilder hdr = new StringBuilder();
        while (fns.hasNext()) {
            Function f = fns.next();
            hdr.append(String.format("; FUNC %-24s %s - %s%n",
                f.getName(), f.getEntryPoint(), f.getBody().getMaxAddress()));
            fcount++;
        }
        w.printf("; %d functions, %d instructions defined%n",
                 fcount, listing.getNumInstructions());
        w.print(hdr);
        w.println(";----------------------------------------------------------");

        InstructionIterator it = listing.getInstructions(true);
        while (it.hasNext()) {
            Instruction ins = it.next();
            Address a = ins.getAddress();
            // label?
            Symbol s = getSymbolAt(a);
            if (s != null && s.getSource() != SourceType.DEFAULT)
                w.printf("%s:%n", s.getName());
            StringBuilder hex = new StringBuilder();
            for (byte b : ins.getBytes()) hex.append(String.format("%02X ", b));
            String cmt = ins.getComment(CodeUnit.EOL_COMMENT);
            w.printf("%s  %-9s %-18s%s%n", a, hex.toString().trim(),
                     ins.toString(), cmt != null ? "  ; " + cmt : "");
        }
        w.close();
        println("wrote " + out + " (" + fcount + " functions, "
                + listing.getNumInstructions() + " instructions)");
    }
}
