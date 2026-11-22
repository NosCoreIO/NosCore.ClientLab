using NosCore.ClientLab.Recompiler;

internal static class WebPageChecks
{
    // The start of the client's web page window "shown" handler (build 6104, 0x563240) and, later in it,
    // "Trim(FUrl)" on the field at +0x100.
    private const string Shown = "558BEC6A0053568BD833C055687A33560064FF30648920A12C747900803801751" +
        "6E806311D003C01750D33D28B83E4000000";
    private const string UrlRead = "8D55FC8B8300010000E82C8DEAFF";

    public static void Run()
    {
        var code = Convert.FromHexString("90" + Shown + "9090" + UrlRead + "C3");
        var image = new ExecutableImage(code);
        if (!WebPageHooks.Find(image).TryGetValue(0x1001, out var offset) || offset != 0x100)
            throw new Exception("The web page window's shown handler did not resolve with its address field.");
        if (WebPageHooks.Find(new ExecutableImage(Convert.FromHexString("90" + Shown + UrlRead + Shown + UrlRead))).Count != 0)
            throw new Exception("An ambiguous web page hook was accepted.");
        if (WebPageHooks.Find(new ExecutableImage(Convert.FromHexString("90" + Shown + "C3"))).Count != 0)
            throw new Exception("A web page hook without its address field was accepted.");
        var function = new FunctionDefinition(0x1001, "shown", [new AddressRange(0x1001, 0x1000 + (uint)code.Length)]);
        var generated = new CEmitter(image, function).Generate();
        if (!generated.Contains("port_open_web_page(c, 0x100u);"))
            throw new Exception("The recompiler did not emit the web page hook at the handler's entry.");
        if (Environment.GetEnvironmentVariable("CLIENTLAB_TEST_EXE") is { Length: > 0 } executable)
        {
            var sites = WebPageHooks.Find(new ExecutableImage(executable));
            foreach (var site in sites) Console.WriteLine($"Resolved web page hook at {site.Key:x8}, address field +0x{site.Value:x}");
            if (sites.Count != 1) throw new Exception("The selected client does not uniquely match the web page window signature.");
        }
        Console.WriteLine("PASS: the in-game web page window hook resolves uniquely with its address field and hands the window's address to the page.");
    }
}
