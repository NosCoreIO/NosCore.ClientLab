// Applies "address name [comment...]" lines from a symbols file, creating functions where only a label exists.
// @category NosCore.ClientLab
import ghidra.app.cmd.disassemble.DisassembleCommand;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressSet;
import ghidra.program.model.listing.CodeUnit;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.SourceType;

import java.io.File;
import java.nio.file.Files;

public class ApplyNames extends GhidraScript {
    @Override
    protected void run() throws Exception {
        File file = askFile("Symbols file", "Apply");
        int named = 0;
        int created = 0;
        for (String raw : Files.readAllLines(file.toPath())) {
            String line = raw.strip();
            if (line.isEmpty() || line.startsWith("#")) {
                continue;
            }
            String[] parts = line.split("\\s+", 3);
            Address addr = toAddr(Long.parseLong(parts[0], 16));
            Function f = getFunctionAt(addr);
            if (f == null) {
                if (getInstructionAt(addr) == null) {
                    new DisassembleCommand(new AddressSet(addr), null, true).applyTo(currentProgram, monitor);
                }
                f = createFunction(addr, null);
                if (f == null) {
                    printerr("Could not create function at " + addr);
                    continue;
                }
                created++;
            }
            f.setName(parts[1], SourceType.USER_DEFINED);
            if (parts.length > 2) {
                currentProgram.getListing().setComment(addr, CodeUnit.PLATE_COMMENT, parts[2]);
            }
            named++;
        }
        println("Named " + named + " functions (" + created + " created)");
    }
}
