// Writes Ghidra's C for the functions containing the given addresses: Decompile.java <out.c> <hex address>...
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.listing.Function;
import java.io.PrintWriter;

public class Decompile extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        DecompInterface d = new DecompInterface();
        d.openProgram(currentProgram);
        try (PrintWriter out = new PrintWriter(args[0])) {
            for (int i = 1; i < args.length; i++) {
                Function fn = getFunctionContaining(toAddr(Long.decode(args[i])));
                if (fn == null) { out.println("// no function at " + args[i]); continue; }
                DecompileResults r = d.decompileFunction(fn, 120, monitor);
                out.println("// ===== " + fn.getName() + " @ " + fn.getEntryPoint() + " (asked: " + args[i] + ")");
                out.println(r.decompileCompleted() ? r.getDecompiledFunction().getC() : "// failed: " + r.getErrorMessage());
            }
        }
        d.dispose();
    }
}
