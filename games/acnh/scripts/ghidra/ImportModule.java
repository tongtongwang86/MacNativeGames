// Sets up a raw import of build/ghidra/<module>.flat (scripts/ghidra_prep.py): segments with their permissions, names
// of imported functions, and every function start from the module's eh_frame_hdr (disassembled and created; no
// auto-analysis, which takes hours on ACNH's 60 MB of code). Argument: the .tsv path.
import ghidra.app.script.GhidraScript;
import ghidra.app.cmd.disassemble.DisassembleCommand;
import ghidra.app.cmd.function.CreateFunctionCmd;
import ghidra.program.model.address.Address;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.SourceType;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.List;

public class ImportModule extends GhidraScript {
    @Override
    public void run() throws Exception {
        List<String> lines = Files.readAllLines(Path.of(getScriptArgs()[0]));
        Memory mem = currentProgram.getMemory();
        for (String l : lines) {
            String[] f = l.split(" ");
            if (!f[0].equals("seg")) continue;
            Address start = toAddr(Long.decode(f[2])), end = toAddr(Long.decode(f[3]));
            MemoryBlock b = mem.getBlock(start);
            if (b == null) continue;
            if (!b.getStart().equals(start)) { mem.split(b, start); b = mem.getBlock(start); }
            if (end.compareTo(b.getEnd()) <= 0 && !end.equals(b.getEnd().add(1))) { mem.split(b, end); b = mem.getBlock(start); }
            b.setName(f[1]);
            b.setRead(true); b.setWrite(f[4].contains("w")); b.setExecute(f[4].contains("x"));
        }
        int syms = 0, fns = 0;
        for (String l : lines) {
            String[] f = l.split(" ", 3);
            if (f[0].equals("sym")) {
                createLabel(toAddr(Long.decode(f[1])), f[2], true, SourceType.IMPORTED); syms++;
                if (mem.getBlock(toAddr(Long.decode(f[1]))).getName().equals("imports")) createFunction(toAddr(Long.decode(f[1])), f[2]);
            }
        }
        monitor.setMessage("disassembling");
        for (String l : lines) {
            String[] f = l.split(" ");
            if (!f[0].equals("fn")) continue;
            Address a = toAddr(Long.decode(f[1]));
            new DisassembleCommand(a, null, true).applyTo(currentProgram, monitor);
        }
        monitor.setMessage("creating functions");
        for (String l : lines) {
            String[] f = l.split(" ");
            if (!f[0].equals("fn")) continue;
            Address a = toAddr(Long.decode(f[1]));
            if (getFunctionAt(a) == null && new CreateFunctionCmd(a).applyTo(currentProgram, monitor)) fns++;
            if (fns % 20000 == 0) println("functions: " + fns);
        }
        println("ImportModule: " + syms + " names, " + fns + " functions");
    }
}
