# NosCore.ClientLab

Decompiling the NosTale client so it can eventually run in a browser and drive automated NosCore tests.

**No client content lives here.** Bring your own client. Everything it produces (patched exe, IDR/Ghidra databases, decompiled C) stays in a local work folder outside git.

## Decompile your client

Requirements: Windows, Ghidra 12 (it has a built-in Delphi calling convention), Java 21, Python 3, 7-Zip and `gh`.

```powershell
./tools/decompile.ps1 -ClientExe "C:\Program Files (x86)\Nostale\NosCore.exe"
```

1. Downloads IDR plus its Delphi 7 knowledge base, and the Dhrake Ghidra scripts, into `C:\tools\re`.
2. Drives IDR to recover Delphi units, classes, VMTs and method names, then exports them as `.map` and `.idc`.
3. Imports into Ghidra with the `borlanddelphi` compiler spec, applies the IDR symbols through Dhrake, and writes decompiled C to `C:\tools\re\work\decomp`.

The client is Delphi 7. It uses Direct3D 9 (with D3DX), Miles Sound System, Winsock with Indy, and CEF for the in-game browser.
