// Dump the full call graph as an edge list plus a function summary
// (entry addr, size in bytes, callers, callees, indirect jumps).
// Arg0 = output path
//@category Atari
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import java.io.*;
import java.util.*;

public class DumpCallGraph extends GhidraScript {

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        String out = args.length > 0 ? args[0] : "callgraph.txt";

        Listing listing = currentProgram.getListing();
        PrintWriter w = new PrintWriter(new BufferedWriter(new FileWriter(out)));

        // Pass 1: per-function stats.
        w.println("# Function summary");
        w.println("# addr  name  size_bytes  num_callers  num_callees  indirect_jumps");
        w.println();

        // Collect all JSR targets (callees) and indirect branches.
        Map<Long, Set<Long>> callees = new HashMap<>();
        Map<Long, Set<Long>> callers = new HashMap<>();
        Map<Long, Integer>   indirect = new HashMap<>();

        InstructionIterator it = listing.getInstructions(true);
        while (it.hasNext()) {
            Instruction ins = it.next();
            String mn = ins.getMnemonicString().toUpperCase();
            Function caller = getFunctionContaining(ins.getAddress());
            long callerAddr = caller != null ? caller.getEntryPoint().getOffset() : -1;

            if (mn.equals("JSR")) {
                Object[] ops = ins.getOpObjects(0);
                if (ops.length > 0 && ops[0] instanceof Address) {
                    long target = ((Address) ops[0]).getOffset();
                    callees.computeIfAbsent(callerAddr, k -> new TreeSet<>()).add(target);
                    callers.computeIfAbsent(target, k -> new TreeSet<>()).add(callerAddr);
                }
            }
            // Indirect: JMP (abs) — operand has no resolved address (deref at runtime).
            if (mn.equals("JMP") && ins.toString().contains("(")) {
                indirect.merge(callerAddr, 1, Integer::sum);
            }
            // RTS used as jump table: heuristic — we log functions that contain
            // an unusual number of pushed constants before RTS.
        }

        FunctionIterator fns = listing.getFunctions(true);
        while (fns.hasNext()) {
            Function f = fns.next();
            long fa = f.getEntryPoint().getOffset();
            long size = f.getBody().getNumAddresses();
            int nc = callers.getOrDefault(fa, Collections.emptySet()).size();
            int nee = callees.getOrDefault(fa, Collections.emptySet()).size();
            int ni = indirect.getOrDefault(fa, 0);
            w.printf("%04X  %-28s  %5d  %3d callers  %3d callees  %d indirect%n",
                     fa, f.getName(), size, nc, nee, ni);
        }

        // Pass 2: edge list.
        w.println();
        w.println("# Call edges (caller_addr -> callee_addr)");
        for (Map.Entry<Long, Set<Long>> e : new TreeMap<>(callees).entrySet()) {
            for (long callee : e.getValue()) {
                w.printf("%04X -> %04X%n", e.getKey(), callee);
            }
        }
        w.close();
        println("wrote " + out);
    }
}
