// Recover method entry points from Delphi 7 VMT metadata, including dynamic tables.
// @category NosCore.ClientLab
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.reloc.Relocation;
import ghidra.app.cmd.disassemble.DisassembleCommand;
import ghidra.program.model.address.AddressSet;
import java.io.PrintWriter;
import java.util.TreeSet;

public class ExportDelphiMethods extends GhidraScript {
    private Address pointer(Address slot) throws Exception {
        return toAddr(Integer.toUnsignedLong(currentProgram.getMemory().getInt(slot)));
    }
    private boolean code(Address address) {
        var block = currentProgram.getMemory().getBlock(address);
        return block != null && block.isExecute();
    }
    @Override protected void run() throws Exception {
        var entries = new TreeSet<Long>();
        int classes = 0;
        var relocations = currentProgram.getRelocationTable().getRelocations();
        while (relocations.hasNext()) {
            monitor.checkCancelled();
            Relocation relocation = relocations.next();
            Address slot = relocation.getAddress();
            Address vmt;
            try { vmt = pointer(slot); } catch (Exception ignored) { continue; }
            // Delphi also stores free callbacks in data tables. A relocated code
            // pointer with the standard stack-frame prologue is a function entry,
            // even when auto-analysis has not followed a direct call to it.
            if (code(vmt)) {
                try {
                    var memory = currentProgram.getMemory();
                    int first = Byte.toUnsignedInt(memory.getByte(vmt));
                    int second = Byte.toUnsignedInt(memory.getByte(vmt.add(1)));
                    boolean frame = first == 0x55 && second == 0x8b && Byte.toUnsignedInt(memory.getByte(vmt.add(2))) == 0xec;
                    boolean savedRegisters = (first == 0x53 || first == 0x56 || first == 0x57) &&
                        (second == 0x53 || second == 0x55 || second == 0x56 || second == 0x57 || second == 0x8b || second == 0x89);
                    if (frame || savedRegisters)
                        entries.add(vmt.getOffset());
                } catch (Exception ignored) { }
            }
            if (!vmt.equals(slot.add(0x4c))) continue;
            try {
                Address name = pointer(vmt.subtract(0x2c));
                int length = Byte.toUnsignedInt(currentProgram.getMemory().getByte(name));
                if (length == 0 || length > 100) continue;
                Address dynamic = pointer(vmt.subtract(0x30));
                if (dynamic.getOffset() != 0) {
                    int count = Short.toUnsignedInt(currentProgram.getMemory().getShort(dynamic));
                    if (count > 4096) continue;
                    for (int index = 0; index < count; index++) {
                        Address method = pointer(dynamic.add(2L + 2L * count + 4L * index));
                        if (code(method)) entries.add(method.getOffset());
                    }
                }
                for (int offset = -0x20; offset < 2048; offset += 4) {
                    Address method = pointer(vmt.add(offset));
                    if (!code(method)) { if (offset >= 0) break; else continue; }
                    entries.add(method.getOffset());
                }
                classes++;
            } catch (Exception ignored) { /* not a complete Delphi VMT */ }
        }
        int created = 0, contained = 0;
        for (long entry : entries) {
            monitor.checkCancelled();
            Address address = toAddr(entry);
            if (getFunctionAt(address) != null) continue;
            var owner = getFunctionContaining(address);
            if (owner != null) {
                // Auto-analysis sometimes extends the preceding routine into a
                // separately referenced Delphi callback. Retain the preceding body
                // and give the callback its own entry, as the enriched analysis does.
                var before = new AddressSet(owner.getBody());
                before.delete(address, owner.getBody().getMaxAddress());
                if (before.isEmpty() || !before.contains(owner.getEntryPoint())) { contained++; continue; }
                owner.setBody(before);
            }
            if (getInstructionAt(address) == null) {
                clearListing(address, address);
                new DisassembleCommand(new AddressSet(address), null, true).applyTo(currentProgram, monitor);
            }
            if (createFunction(address, null) != null) created++;
            else printerr("Could not create recovered callback at " + address);
        }
        try (var output = new PrintWriter(getScriptArgs()[0])) {
            output.println("# Delphi 7 VMT and dynamic-method entry metadata; no executable bytes.");
            for (long entry : entries) output.printf("%08x%n", entry);
        }
        println("Recovered " + entries.size() + " method entries from " + classes + " Delphi VMTs; created " + created + " functions; " + contained + " already inside another function.");
    }
}
