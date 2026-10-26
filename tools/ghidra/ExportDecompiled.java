// Decompiles every function of the current program into one C file per 0x10000 address block.
// @category NosCore.ClientLab
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileOptions;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;

import java.io.File;
import java.io.FileWriter;
import java.io.PrintWriter;
import java.util.Map;
import java.util.TreeMap;

public class ExportDecompiled extends GhidraScript {
    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        File outDir = new File(args.length > 0 ? args[0] : "decomp");
        outDir.mkdirs();

        DecompInterface decompiler = new DecompInterface();
        decompiler.setOptions(new DecompileOptions());
        decompiler.openProgram(currentProgram);

        Map<Long, StringBuilder> blocks = new TreeMap<>();
        int total = currentProgram.getFunctionManager().getFunctionCount();
        int done = 0;
        int failed = 0;
        for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
            if (monitor.isCancelled()) {
                break;
            }
            long addr = f.getEntryPoint().getOffset();
            StringBuilder sb = blocks.computeIfAbsent(addr & ~0xFFFFL, k -> new StringBuilder());
            DecompileResults res = decompiler.decompileFunction(f, 60, monitor);
            if (res != null && res.decompileCompleted()) {
                sb.append("// ").append(f.getName()).append(" @ ").append(f.getEntryPoint()).append('\n');
                sb.append(res.getDecompiledFunction().getC()).append('\n');
            } else {
                failed++;
                sb.append("// FAILED ").append(f.getName()).append(" @ ").append(f.getEntryPoint()).append('\n');
            }
            if (++done % 1000 == 0) {
                println(done + "/" + total + " decompiled");
            }
        }
        decompiler.dispose();

        for (Map.Entry<Long, StringBuilder> e : blocks.entrySet()) {
            File out = new File(outDir, String.format("block_%08x.c", e.getKey()));
            try (PrintWriter w = new PrintWriter(new FileWriter(out))) {
                w.print(e.getValue());
            }
        }
        println("Decompiled " + done + " functions (" + failed + " failed) into " + blocks.size() + " files in " + outDir.getAbsolutePath());
    }
}
