// Writes one line per function: "entry name start-end[,start-end...]" (hex, end exclusive).
// @category NosCore.ClientLab
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.AddressRange;
import ghidra.program.model.listing.Function;

import java.io.PrintWriter;

public class ExportFunctions extends GhidraScript {
    @Override
    protected void run() throws Exception {
        String out = getScriptArgs()[0];
        try (PrintWriter w = new PrintWriter(out)) {
            for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
                if (f.isExternal() || f.isThunk() && f.getBody().getNumAddresses() == 0) {
                    continue;
                }
                StringBuilder sb = new StringBuilder();
                for (AddressRange r : f.getBody().getAddressRanges()) {
                    if (sb.length() > 0) {
                        sb.append(',');
                    }
                    sb.append(String.format("%x-%x", r.getMinAddress().getOffset(), r.getMaxAddress().getOffset() + 1));
                }
                w.printf("%x %s %s%n", f.getEntryPoint().getOffset(), f.getName(true).replace(' ', '_'), sb);
            }
        }
    }
}
