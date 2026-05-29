// Dump every instruction that reads or writes Atari 8-bit hardware registers
// ($D000-$D7FF), key page-2 OS shadow registers, and zero-page RTCLOK/vectors.
// Output is a markdown table suitable for docs/hw-access.md.
// Arg0 = output path
//@category Atari
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.mem.*;
import ghidra.program.model.symbol.*;
import java.io.*;
import java.util.*;

public class DumpHwAccesses extends GhidraScript {

    // Named hardware and shadow registers.
    static final Map<Integer, String> NAMES = new LinkedHashMap<>();
    static {
        // GTIA $D000-$D01F
        NAMES.put(0xD000, "HPOSP0"); NAMES.put(0xD001, "HPOSP1");
        NAMES.put(0xD002, "HPOSP2"); NAMES.put(0xD003, "HPOSP3");
        NAMES.put(0xD004, "HPOSM0"); NAMES.put(0xD005, "HPOSM1");
        NAMES.put(0xD006, "HPOSM2"); NAMES.put(0xD007, "HPOSM3");
        NAMES.put(0xD008, "SIZEP0"); NAMES.put(0xD009, "SIZEP1");
        NAMES.put(0xD00A, "SIZEP2"); NAMES.put(0xD00B, "SIZEP3");
        NAMES.put(0xD00C, "SIZEM");
        NAMES.put(0xD00D, "GRAFP0"); NAMES.put(0xD00E, "GRAFP1");
        NAMES.put(0xD00F, "GRAFP2"); NAMES.put(0xD010, "GRAFP3");
        NAMES.put(0xD011, "GRAFM");
        NAMES.put(0xD012, "COLPM0"); NAMES.put(0xD013, "COLPM1");
        NAMES.put(0xD014, "COLPM2"); NAMES.put(0xD015, "COLPM3");
        NAMES.put(0xD016, "COLPF0"); NAMES.put(0xD017, "COLPF1");
        NAMES.put(0xD018, "COLPF2"); NAMES.put(0xD019, "COLPF3");
        NAMES.put(0xD01A, "COLBK");
        NAMES.put(0xD01B, "PRIOR");  NAMES.put(0xD01C, "VDELAY");
        NAMES.put(0xD01D, "GRACTL"); NAMES.put(0xD01E, "HITCLR");
        NAMES.put(0xD01F, "CONSOL");
        // POKEY $D200-$D21F
        NAMES.put(0xD200, "AUDF1/POT0"); NAMES.put(0xD201, "AUDC1/POT1");
        NAMES.put(0xD202, "AUDF2/POT2"); NAMES.put(0xD203, "AUDC2/POT3");
        NAMES.put(0xD204, "AUDF3/POT4"); NAMES.put(0xD205, "AUDC3/POT5");
        NAMES.put(0xD206, "AUDF4/POT6"); NAMES.put(0xD207, "AUDC4/POT7");
        NAMES.put(0xD208, "AUDCTL/ALLPOT"); NAMES.put(0xD209, "STIMER/KBCODE");
        NAMES.put(0xD20A, "SKREST/RANDOM"); NAMES.put(0xD20B, "POTGO");
        NAMES.put(0xD20D, "SEROUT/SERIN"); NAMES.put(0xD20E, "IRQEN/IRQST");
        NAMES.put(0xD20F, "SKCTL/SKSTAT");
        // PIA $D300-$D31F
        NAMES.put(0xD300, "PORTA");  NAMES.put(0xD301, "PORTB");
        NAMES.put(0xD302, "PACTL");  NAMES.put(0xD303, "PBCTL");
        // ANTIC $D400-$D41F
        NAMES.put(0xD400, "DMACTL"); NAMES.put(0xD401, "CHACTL");
        NAMES.put(0xD402, "DLISTL"); NAMES.put(0xD403, "DLISTH");
        NAMES.put(0xD404, "HSCROL"); NAMES.put(0xD405, "VSCROL");
        NAMES.put(0xD407, "PMBASE"); NAMES.put(0xD409, "CHBASE");
        NAMES.put(0xD40A, "WSYNC");  NAMES.put(0xD40B, "VCOUNT");
        NAMES.put(0xD40C, "PENH");   NAMES.put(0xD40D, "PENV");
        NAMES.put(0xD40E, "NMIEN");  NAMES.put(0xD40F, "NMIST/NMIRES");
        // OS page-2 shadows and vectors
        NAMES.put(0x0200, "VDSLST_L"); NAMES.put(0x0201, "VDSLST_H");
        NAMES.put(0x0202, "VPRCED_L"); NAMES.put(0x0203, "VPRCED_H");
        NAMES.put(0x0204, "VINTER_L"); NAMES.put(0x0205, "VINTER_H");
        NAMES.put(0x0206, "VBREAK_L"); NAMES.put(0x0207, "VBREAK_H");
        NAMES.put(0x0208, "VKEYBD_L"); NAMES.put(0x0209, "VKEYBD_H");
        NAMES.put(0x020A, "VSERIN_L"); NAMES.put(0x020B, "VSERIN_H");
        NAMES.put(0x020C, "VSEROR_L"); NAMES.put(0x020D, "VSEROR_H");
        NAMES.put(0x020E, "VSEROC_L"); NAMES.put(0x020F, "VSEROC_H");
        NAMES.put(0x0210, "VTIMR1_L"); NAMES.put(0x0211, "VTIMR1_H");
        NAMES.put(0x0212, "VTIMR2_L"); NAMES.put(0x0213, "VTIMR2_H");
        NAMES.put(0x0214, "VTIMR4_L"); NAMES.put(0x0215, "VTIMR4_H");
        NAMES.put(0x0216, "VIMIRQ_L"); NAMES.put(0x0217, "VIMIRQ_H");
        NAMES.put(0x021A, "CDTMV1_L"); NAMES.put(0x021B, "CDTMV1_H");
        NAMES.put(0x021C, "CDTMV2_L"); NAMES.put(0x021D, "CDTMV2_H");
        NAMES.put(0x021E, "CDTMV3_L"); NAMES.put(0x021F, "CDTMV3_H");
        NAMES.put(0x0220, "CDTMV4_L"); NAMES.put(0x0221, "CDTMV4_H");
        NAMES.put(0x0222, "VVBLKI_L"); NAMES.put(0x0223, "VVBLKI_H");
        NAMES.put(0x0224, "VVBLKD_L"); NAMES.put(0x0225, "VVBLKD_H");
        NAMES.put(0x022F, "SDMCTL");
        NAMES.put(0x0230, "SDLSTL"); NAMES.put(0x0231, "SDLSTH");
        NAMES.put(0x0240, "SAVMSC_L"); NAMES.put(0x0241, "SAVMSC_H");
        NAMES.put(0x0244, "COLDST");
        NAMES.put(0x026F, "GPRIOR");
        NAMES.put(0x0278, "STICK0");  NAMES.put(0x0279, "STICK1");
        NAMES.put(0x027A, "STICK2");  NAMES.put(0x027B, "STICK3");
        NAMES.put(0x0284, "STRIG0");  NAMES.put(0x0285, "STRIG1");
        NAMES.put(0x0286, "STRIG2");  NAMES.put(0x0287, "STRIG3");
        NAMES.put(0x02BE, "LMARGN");  NAMES.put(0x02BF, "RMARGN");
        NAMES.put(0x02C0, "PCOLR0");  NAMES.put(0x02C1, "PCOLR1");
        NAMES.put(0x02C2, "PCOLR2");  NAMES.put(0x02C3, "PCOLR3");
        NAMES.put(0x02C4, "COLOR0");  NAMES.put(0x02C5, "COLOR1");
        NAMES.put(0x02C6, "COLOR2");  NAMES.put(0x02C7, "COLOR3");
        NAMES.put(0x02C8, "COLOR4");
        NAMES.put(0x02F3, "CHACT");   NAMES.put(0x02F4, "CHBAS");
        NAMES.put(0x02FC, "CH");      NAMES.put(0x02FD, "ATACNT");
        NAMES.put(0x02E0, "RUNAD_L"); NAMES.put(0x02E1, "RUNAD_H");
        NAMES.put(0x02E2, "INITAD_L"); NAMES.put(0x02E3, "INITAD_H");
    }

    static boolean isHwOrShadow(long a) {
        return (a >= 0xD000 && a < 0xD800) || (a >= 0x0200 && a < 0x0300);
    }

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        String out = args.length > 0 ? args[0] : "hw-access.txt";

        Listing listing = currentProgram.getListing();
        // addr -> {reads:[funcAddr,...], writes:[funcAddr,...]}
        Map<Long, List<String>> reads  = new TreeMap<>();
        Map<Long, List<String>> writes = new TreeMap<>();

        InstructionIterator it = listing.getInstructions(true);
        while (it.hasNext()) {
            Instruction ins = it.next();
            long iAddr = ins.getAddress().getOffset();
            String fn = "?";
            Function f = getFunctionContaining(ins.getAddress());
            if (f != null) fn = String.format("%s(%04X)", f.getName(), iAddr);
            else fn = String.format("?(%04X)", iAddr);

            // Walk all operand objects looking for addresses.
            for (int op = 0; op < ins.getNumOperands(); op++) {
                for (Object o : ins.getOpObjects(op)) {
                    if (o instanceof Address) {
                        long a = ((Address) o).getOffset();
                        if (!isHwOrShadow(a)) continue;
                        boolean isWrite = isWrite(ins);
                        if (isWrite) writes.computeIfAbsent(a, k -> new ArrayList<>()).add(fn);
                        else         reads.computeIfAbsent(a,  k -> new ArrayList<>()).add(fn);
                    }
                }
            }
        }

        Set<Long> all = new TreeSet<>();
        all.addAll(reads.keySet());
        all.addAll(writes.keySet());

        PrintWriter w = new PrintWriter(new BufferedWriter(new FileWriter(out)));
        w.println("# Hardware & OS-shadow register accesses");
        w.println();
        w.println("Generated by DumpHwAccesses.java from rof_mem.bin via Ghidra headless.");
        w.println("Sorted by register address. `R`=read, `W`=write.");
        w.println();
        w.println("| Addr | Name | RW | Chip/Group | Access sites |");
        w.println("|---|---|---|---|---|");

        for (long a : all) {
            String name = NAMES.getOrDefault((int)a, String.format("$%04X", a));
            String group = chipGroup(a);
            List<String> rs = reads.getOrDefault(a, Collections.emptyList());
            List<String> ws = writes.getOrDefault(a, Collections.emptyList());
            String rw = (!rs.isEmpty() && !ws.isEmpty()) ? "R+W" :
                        (!rs.isEmpty() ? "R" : "W");
            // Summarize sites: up to 6, then "..."
            List<String> all_sites = new ArrayList<>();
            for (String s : ws) all_sites.add("W:"+s);
            for (String s : rs) all_sites.add("R:"+s);
            String sites = String.join(", ", all_sites.subList(0, Math.min(6, all_sites.size())));
            if (all_sites.size() > 6) sites += " … +" + (all_sites.size()-6) + " more";
            w.printf("| `$%04X` | %-22s | %3s | %-12s | %s |%n",
                     a, name, rw, group, sites);
        }
        w.close();
        println("wrote " + out + " — " + all.size() + " distinct HW/shadow addresses accessed");
    }

    boolean isWrite(Instruction ins) {
        String mn = ins.getMnemonicString().toUpperCase();
        // Store instructions.
        if (mn.equals("STA") || mn.equals("STX") || mn.equals("STY")
         || mn.equals("STZ")) return true;
        // RMW instructions count as both; report as write for our purposes.
        if (mn.equals("INC") || mn.equals("DEC") || mn.equals("ASL")
         || mn.equals("LSR") || mn.equals("ROL") || mn.equals("ROR")) return true;
        return false;
    }

    String chipGroup(long a) {
        if (a >= 0xD000 && a < 0xD020) return "GTIA";
        if (a >= 0xD200 && a < 0xD220) return "POKEY";
        if (a >= 0xD300 && a < 0xD320) return "PIA";
        if (a >= 0xD400 && a < 0xD420) return "ANTIC";
        if (a >= 0x0200 && a < 0x0230) return "OS vectors";
        if (a >= 0x0230 && a < 0x0250) return "ANTIC shadow";
        if (a >= 0x026F && a < 0x0270) return "GTIA shadow";
        if (a >= 0x0278 && a < 0x0290) return "Input shadow";
        if (a >= 0x02C0 && a < 0x02D0) return "Color shadow";
        if (a >= 0x02E0 && a < 0x02F0) return "Run/Init";
        if (a >= 0x02F0 && a < 0x0300) return "Char/KB";
        return "OS page-2";
    }
}
