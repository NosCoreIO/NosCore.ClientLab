// Creates functions at the hex addresses listed one per line (skips addresses already inside a function).
// @category NosCore.ClientLab
import ghidra.app.cmd.disassemble.DisassembleCommand;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressSet;

import java.io.File;
import java.nio.file.Files;

public class CreateFunctions extends GhidraScript {
    @Override
    protected void run() throws Exception {
        File file = askFile("Seeds file", "Create");
        int created = 0;
        int failed = 0;
        for (String line : Files.readAllLines(file.toPath())) {
            line = line.strip();
            if (line.isEmpty() || line.startsWith("#")) {
                continue;
            }
            Address addr = toAddr(Long.parseLong(line.split("\\s+")[0], 16));
            if (getFunctionContaining(addr) != null && getFunctionAt(addr) == null) {
                continue;
            }
            if (getFunctionAt(addr) != null) {
                continue;
            }
            if (getInstructionAt(addr) == null) {
                new DisassembleCommand(new AddressSet(addr), null, true).applyTo(currentProgram, monitor);
            }
            if (createFunction(addr, null) != null) {
                created++;
            } else {
                failed++;
            }
        }
        println("Created " + created + " functions, " + failed + " failed");
    }
}
