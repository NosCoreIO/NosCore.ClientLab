using NosCore.ClientLab.Launcher.Services;

internal static class CompilationCacheChecks
{
    public static async Task RunAsync()
    {
        var root = Path.Combine(Path.GetTempPath(), "ClientLab-compilation-cache-check-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(root);
        try
        {
            var first = Path.Combine(root, "attempt-one"); var second = Path.Combine(root, "attempt-two");
            Directory.CreateDirectory(first); Directory.CreateDirectory(second);
            var sourceOne = Path.Combine(first, "module.c"); var sourceTwo = Path.Combine(second, "module.c");
            await File.WriteAllTextAsync(sourceOne, "original source"); await File.WriteAllTextAsync(sourceTwo, "original source");
            var cacheRoot = Path.Combine(root, "cache"); var compiles = 0;
            Task Compile(string path, CancellationToken token) { compiles++; return File.WriteAllTextAsync(path, "complete object", token); }
            var original = await new CompilationCache(cacheRoot, "compiler-flags-and-headers-A").GetOrCompileAsync(sourceOne, Compile, default);
            var reused = await new CompilationCache(cacheRoot, "compiler-flags-and-headers-A").GetOrCompileAsync(sourceTwo, Compile, default);
            if (original.Reused || !reused.Reused || original.Path != reused.Path || compiles != 1)
                throw new Exception("A new build attempt did not reuse identical completed compilation output.");
            await File.WriteAllTextAsync(sourceTwo, "changed source");
            var changed = await new CompilationCache(cacheRoot, "compiler-flags-and-headers-A").GetOrCompileAsync(sourceTwo, Compile, default);
            var headersChanged = await new CompilationCache(cacheRoot, "compiler-flags-and-headers-B").GetOrCompileAsync(sourceTwo, Compile, default);
            if (changed.Reused || headersChanged.Reused || compiles != 3 || original.Path == changed.Path || headersChanged.Path == changed.Path)
                throw new Exception("Changed compilation inputs incorrectly reused a cached object.");
            var cancelledCache = new CompilationCache(cacheRoot, "cancelled-build");
            try
            {
                await cancelledCache.GetOrCompileAsync(sourceOne, async (path, token) =>
                { await File.WriteAllTextAsync(path, "partial output", token); throw new OperationCanceledException(); }, default);
                throw new Exception("Cancelled output was accepted.");
            }
            catch (OperationCanceledException) { }
            var retried = await cancelledCache.GetOrCompileAsync(sourceOne, Compile, default);
            if (retried.Reused || await File.ReadAllTextAsync(retried.Path) != "complete object" || Directory.EnumerateFiles(cacheRoot, "*.partial.o", SearchOption.AllDirectories).Any())
                throw new Exception("Cancelled compilation left a reusable partial object.");
            try
            {
                await new CompilationCache(cacheRoot, "empty-output").GetOrCompileAsync(sourceOne, (path, token) => File.WriteAllTextAsync(path, "", token), default);
                throw new Exception("Empty compiler output was cached.");
            }
            catch (InvalidDataException) { }
            Console.WriteLine("PASS: compiled objects survive new attempts, changed inputs invalidate them, and cancelled/empty output is never cached.");
        }
        finally { Directory.Delete(root, recursive: true); }
    }
}
