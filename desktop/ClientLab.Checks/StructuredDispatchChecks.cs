using System.Diagnostics;
using NosCore.ClientLab.Recompiler;

internal static class StructuredDispatchChecks
{
    public static async Task RunAsync()
    {
        var root = Path.Combine(Path.GetTempPath(), "ClientLab-dispatch-check-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(root);
        try
        {
            var image = new ExecutableImage(Convert.FromHexString("B96400000031C001C84975FBC3"));
            var definition = new FunctionDefinition(0x1000, "control-flow fixture", [new(0x1000, image.CodeEnd)]);
            CEmitter Emitter(int threshold) => new(image, definition) { StructuredDispatchThreshold = threshold, ExtraLabels = [0x1005, 0x1009] };
            var reference = Emitter(int.MaxValue).Generate().Replace("f_00001000", "reference");
            var structured = Emitter(1).Generate().Replace("f_00001000", "structured");
            // xor/add/sub, shl eax,cl, jb, add, adc, sete bl, test, setl bh, and, shr, sbb, inc, dec ecx, jne back, shl edx,cl
            // (cl is 0 there, so dec's flags must survive to ret), ret.
            var flagImage = new ExecutableImage(Convert.FromHexString("31C001C829D0D3E0720383C00511D80F94C385C90F9CC721D2D1E819C8404975DFD3E2C3"));
            var flagDefinition = new FunctionDefinition(0x1000, "flag fixture", [new(0x1000, flagImage.CodeEnd)]);
            var flagsKept = new CEmitter(flagImage, flagDefinition) { FlagLiveness = false }.Generate().Replace("f_00001000", "flags_kept");
            var flagsDropped = new CEmitter(flagImage, flagDefinition).Generate().Replace("f_00001000", "flags_dropped");
            if (!flagsDropped.Contains("xCF =") || flagsKept.Contains("xCF =")) throw new Exception("Flag liveness did not drop the overwritten add's flags.");
            var comparisonImage = new ExecutableImage(Convert.FromHexString("D8D3DFE0C3")); // fcom st(3); fnstsw ax; ret
            var comparison = new CEmitter(comparisonImage, new(0x1000, "x87 comparison", [new(0x1000, comparisonImage.CodeEnd)]))
                .Generate().Replace("f_00001000", "comparison");
            File.WriteAllText(Path.Combine(root, "rt.h"), """
                #include <stdint.h>
                typedef struct CPU {
                  union { struct { uint32_t eax,ecx,edx,ebx,esp,ebp,esi,edi; }; uint32_t regs[8]; };
                  uint32_t cf,zf,sf,of,pf,pend,pend_target;
                  double st[8]; uint16_t fsw;
                } CPU;
                static uint32_t memory[1024];
                #define RT_RESTRICT __restrict
                #define RT_TRACE(c,e,r) ((void)0)
                #define R32(i) c->regs[i]
                #define R16(i) (*(uint16_t*)&c->regs[i])
                #define R8L(i) (*(uint8_t*)&c->regs[i])
                #define R8H(i) (((uint8_t*)&c->regs[i])[1])
                #define ST(i) c->st[i]
                static void rt_fcom(CPU* c,double a,double b) { c->fsw=a<b?0x100:a==b?0x4000:0; }
                static uint16_t rt_fsw(CPU* c) { return c->fsw; }
                #define M32(a) memory[(a)/4]
                #define CF c->cf
                #define ZF c->zf
                #define SF c->sf
                #define OF c->of
                #define PF c->pf
                static int parity(uint8_t v) { v ^= v>>4; v ^= v>>2; v ^= v>>1; return !(v&1); }
                #define PARITY(v) parity(v)
                static void rt_jump_ind(CPU* c,uint32_t target,uint32_t ret,int ind) { c->pend=ind; c->pend_target=target; }
                static void rt_jump(CPU* c,uint32_t target,uint32_t ret) { c->pend=1; c->pend_target=target; }
                """);
            File.WriteAllText(Path.Combine(root, "test.c"), "#include \"rt.h\"\n#include <string.h>\n" + reference + structured + comparison + flagsKept + flagsDropped + """

                int main(void) {
                  uint32_t flagSeed=777;
                  for(int i=0;i<500;i++) {
                    CPU a={0},b;
                    for(int r=0;r<8;r++) { flagSeed=flagSeed*1664525u+1013904223u; a.regs[r]=flagSeed; }
                    a.ecx=(flagSeed%3)==0?256:flagSeed%8+1; a.esp=256;
                    a.cf=flagSeed>>27&1; a.zf=flagSeed>>28&1; a.sf=flagSeed>>29&1; a.of=flagSeed>>30&1; a.pf=flagSeed>>31; b=a;
                    memory[64]=0xc0de;
                    flags_kept(&a,0x1000,0xc0de);
                    flags_dropped(&b,0x1000,0xc0de);
                    if(memcmp(&a,&b,sizeof(CPU))) return 5;
                  }
                  CPU fp={0}; fp.esp=256; fp.st[0]=0.5; fp.st[3]=1.0;
                  memory[64]=0xc0de; comparison(&fp,0x1000,0xc0de);
                  if((fp.eax&0x4500)!=0x100 || fp.esp!=260) return 3;
                  fp.esp=256; fp.st[0]=2.0; comparison(&fp,0x1000,0xc0de);
                  if((fp.eax&0x4500)!=0) return 4;
                  uint32_t entries[] = {0x1000,0x1005,0x1007,0x1009,0x100c,0xdead};
                  uint32_t seed=12345;
                  for(int i=0;i<500;i++) {
                    CPU a={0},b;
                    seed=seed*1664525u+1013904223u;
                    a.eax=seed; a.ecx=(seed%64)+1; a.esp=256;
                    a.cf=seed&1; a.zf=(seed>>1)&1; a.pf=(seed>>2)&1;
                    a.sf=(seed>>3)&1; a.of=(seed>>4)&1; b=a;
                    memory[64]=(i%7)==0?0xdead:0xc0de;
                    reference(&a,entries[i%6],0xc0de);
                    structured(&b,entries[i%6],0xc0de);
                    if(memcmp(&a,&b,sizeof(CPU))) return 1;
                  }
                  CPU known={0}; known.esp=256; memory[64]=0xc0de;
                  structured(&known,0x1000,0xc0de);
                  return known.eax==5050 && known.ecx==0 && known.esp==260 && known.zf==1 && known.pend==0 ? 0 : 2;
                }
                """);
            var exe = Path.Combine(root, OperatingSystem.IsWindows() ? "test.exe" : "test");
            var vcvars = @"C:\Program Files\Microsoft Visual Studio\18\Professional\VC\Auxiliary\Build\vcvars32.bat";
            if (OperatingSystem.IsWindows() && File.Exists(vcvars))
            {
                var batch = Path.Combine(root, "build.cmd");
                File.WriteAllText(batch, $"@call \"{vcvars}\" >nul 2>nul\r\n@cl /nologo /O2 /W0 /std:c17 /TC /Fe\"{exe}\" /Fo\"{Path.Combine(root, "test.obj")}\" \"{Path.Combine(root, "test.c")}\"\r\n");
                await RunAsync("cmd.exe", ["/d", "/c", batch], root);
            }
            else
            {
                var compiler = (Environment.GetEnvironmentVariable("PATH") ?? "").Split(Path.PathSeparator)
                    .Select(path => Path.Combine(path, OperatingSystem.IsWindows() ? "clang.exe" : "cc")).FirstOrDefault(File.Exists);
                if (compiler is null) { Console.WriteLine("SKIP: full-function native dispatch check requires a C compiler."); return; }
                await RunAsync(compiler, ["-O2", Path.Combine(root, "test.c"), "-o", exe], root);
            }
            await RunAsync(exe, [], root);
            Console.WriteLine("PASS: structured dispatch matches the original generated C for 500 branch/loop/mid-entry/return cases; x87 compares distinct stack registers correctly; dropping dead flags leaves registers and flags identical over 500 runs.");
        }
        finally { Directory.Delete(root, recursive: true); }
    }

    private static async Task RunAsync(string executable, string[] arguments, string directory)
    {
        var info = new ProcessStartInfo(executable) { WorkingDirectory = directory, UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true, RedirectStandardError = true };
        foreach (var argument in arguments) info.ArgumentList.Add(argument);
        using var child = Process.Start(info)!;
        var output = child.StandardOutput.ReadToEndAsync(); var error = child.StandardError.ReadToEndAsync();
        await child.WaitForExitAsync();
        if (child.ExitCode != 0) throw new Exception("Native dispatch check failed: " + await output + await error);
    }
}
