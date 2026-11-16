using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;

namespace NosCore.ClientLab.Launcher.Services;

public static class GameforgeLaunchFeatures
{
    // Official 3.0.4.2238: 00533cb0 encodes a boolean in the parity of the
    // decimal digit sum, not in the integer's low bit. Report the real condition.
    public static long Collect()
    {
        Span<byte> bytes = stackalloc byte[4]; uint nonce;
        do { RandomNumberGenerator.Fill(bytes); nonce = BitConverter.ToUInt32(bytes); }
        while (nonce is 0 or uint.MaxValue);
        return Encode(nonce, HasMatchingWindow());
    }

    public static uint Encode(uint nonce, bool present) => Decode(nonce) == present ? nonce : nonce ^ 1u;
    public static bool Decode(uint value)
    {
        uint sum = 0;
        for (; value != 0; value /= 10) sum += value % 10;
        return (sum & 1) != 0;
    }

    private static bool HasMatchingWindow()
    {
        if (!OperatingSystem.IsWindows()) return false;
        var found = false;
        EnumWindows((window, _) =>
        {
            var title = new StringBuilder(1024);
            GetWindowText(window, title, title.Capacity);
            // The native predicate decodes this title prefix using "gfclient.exe".
            if (!title.ToString().StartsWith("M2Bob", StringComparison.OrdinalIgnoreCase)) return true;
            found = true; return false;
        }, IntPtr.Zero);
        return found;
    }

    private delegate bool WindowCallback(IntPtr window, IntPtr parameter);
    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool EnumWindows(WindowCallback callback, IntPtr parameter);
    [DllImport("user32.dll", EntryPoint = "GetWindowTextW", CharSet = CharSet.Unicode)]
    private static extern int GetWindowText(IntPtr window, StringBuilder title, int length);
}
