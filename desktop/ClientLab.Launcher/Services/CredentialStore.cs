using System.ComponentModel;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;

namespace NosCore.ClientLab.Launcher.Services;

public sealed record SavedCredentials(string Email, string Password);

/// <summary>A Gameforge session token and the game account it last launched, kept like the official client keeps its
/// sign-in so a restart does not ask for the password again.</summary>
public sealed record SavedSession(string Token, string AccountId);

/// <summary>Stores credentials in the user's OS vault, never in launcher settings or release files.</summary>
public sealed class CredentialStore(string dataDirectory, string kind = "credentials")
{
    private readonly string target = "NosCore.ClientLab/" + Convert.ToHexStringLower(SHA256.HashData(Encoding.UTF8.GetBytes(Path.GetFullPath(dataDirectory))))[..16]
        + (kind == "credentials" ? "" : "/" + kind);

    public Task<SavedCredentials?> ReadAsync() => ReadAsync<SavedCredentials>();

    public Task SaveAsync(string email, string password) => SaveAsync(email, new SavedCredentials(email, password));

    public async Task<T?> ReadAsync<T>() where T : class
    {
        string? value;
        if (OperatingSystem.IsWindows())
        {
            if (!CredRead(target, 1, 0, out var pointer))
            {
                var error = Marshal.GetLastWin32Error();
                if (error == 1168) return null;
                throw new Win32Exception(error);
            }
            try
            {
                var credential = Marshal.PtrToStructure<Credential>(pointer);
                var bytes = new byte[credential.BlobSize];
                Marshal.Copy(credential.Blob, bytes, 0, bytes.Length);
                try { value = Encoding.UTF8.GetString(bytes); }
                finally { CryptographicOperations.ZeroMemory(bytes); }
            }
            finally { CredFree(pointer); }
        }
        else if (OperatingSystem.IsMacOS()) value = MacRead();
        else value = await SecretToolAsync(["lookup", "service", target]);
        return string.IsNullOrWhiteSpace(value) ? null : JsonSerializer.Deserialize<T>(value);
    }

    public async Task SaveAsync<T>(string userName, T value)
    {
        var bytes = JsonSerializer.SerializeToUtf8Bytes(value);
        try
        {
            if (OperatingSystem.IsWindows())
            {
                var blob = Marshal.AllocCoTaskMem(bytes.Length);
                try
                {
                    Marshal.Copy(bytes, 0, blob, bytes.Length);
                    var credential = new Credential { Type = 1, TargetName = target, UserName = userName, BlobSize = (uint)bytes.Length, Blob = blob, Persist = 2 };
                    if (!CredWrite(ref credential, 0)) throw new Win32Exception(Marshal.GetLastWin32Error());
                }
                finally { Marshal.Copy(new byte[bytes.Length], 0, blob, bytes.Length); Marshal.FreeCoTaskMem(blob); }
            }
            else if (OperatingSystem.IsMacOS()) MacSave(bytes);
            else await SecretToolAsync(["store", "--label=ClientLab Gameforge account", "service", target], Encoding.UTF8.GetString(bytes));
        }
        finally { CryptographicOperations.ZeroMemory(bytes); }
    }

    public async Task DeleteAsync()
    {
        if (OperatingSystem.IsWindows())
        {
            if (!CredDelete(target, 1, 0) && Marshal.GetLastWin32Error() != 1168) throw new Win32Exception(Marshal.GetLastWin32Error());
        }
        else if (OperatingSystem.IsMacOS())
        {
            var status = MacFind(out _, out var data, out var item);
            if (status == -25300) return;
            MacCheck(status);
            try { SecKeychainItemFreeContent(IntPtr.Zero, data); MacCheck(SecKeychainItemDelete(item)); }
            finally { CFRelease(item); }
        }
        else await SecretToolAsync(["clear", "service", target]);
    }

    private static async Task<string?> SecretToolAsync(string[] arguments, string? secret = null)
    {
        var info = new ProcessStartInfo("secret-tool") { UseShellExecute = false, CreateNoWindow = true, RedirectStandardInput = true, RedirectStandardOutput = true, RedirectStandardError = true };
        foreach (var argument in arguments) info.ArgumentList.Add(argument);
        Process process;
        try { process = Process.Start(info)!; }
        catch (Win32Exception) { throw new InvalidOperationException("To remember your account on Linux, install libsecret-tools and enable a desktop keyring."); }
        using (process)
        {
            var output = process.StandardOutput.ReadToEndAsync();
            var error = process.StandardError.ReadToEndAsync();
            if (secret is not null) await process.StandardInput.WriteAsync(secret);
            process.StandardInput.Close();
            await process.WaitForExitAsync();
            await error; // Drain diagnostics without logging credential-store output.
            if (process.ExitCode != 0 && !(secret is null && process.ExitCode == 1))
                throw new InvalidOperationException("The desktop keyring could not save your account.");
            return await output;
        }
    }

    private int MacFind(out uint length, out IntPtr data, out IntPtr item)
    {
        var service = Encoding.UTF8.GetBytes(target); var account = Encoding.UTF8.GetBytes(kind);
        return SecKeychainFindGenericPassword(IntPtr.Zero, (uint)service.Length, service, (uint)account.Length, account, out length, out data, out item);
    }
    private string? MacRead()
    {
        var status = MacFind(out var length, out var data, out var item);
        if (status == -25300) return null;
        MacCheck(status);
        try { return Marshal.PtrToStringUTF8(data, (int)length); }
        finally { SecKeychainItemFreeContent(IntPtr.Zero, data); CFRelease(item); }
    }
    private void MacSave(byte[] bytes)
    {
        var status = MacFind(out _, out var data, out var item);
        if (status == -25300)
        {
            var service = Encoding.UTF8.GetBytes(target); var account = Encoding.UTF8.GetBytes(kind);
            MacCheck(SecKeychainAddGenericPassword(IntPtr.Zero, (uint)service.Length, service, (uint)account.Length, account, (uint)bytes.Length, bytes, IntPtr.Zero));
            return;
        }
        MacCheck(status);
        try { SecKeychainItemFreeContent(IntPtr.Zero, data); MacCheck(SecKeychainItemModifyAttributesAndData(item, IntPtr.Zero, (uint)bytes.Length, bytes)); }
        finally { CFRelease(item); }
    }
    private static void MacCheck(int status) { if (status != 0) throw new InvalidOperationException($"The macOS Keychain could not access your account (status {status})."); }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct Credential
    {
        public uint Flags, Type;
        public string TargetName;
        public string? Comment;
        public long LastWritten;
        public uint BlobSize;
        public IntPtr Blob;
        public uint Persist, AttributeCount;
        public IntPtr Attributes;
        public string? TargetAlias, UserName;
    }
    [DllImport("advapi32.dll", EntryPoint = "CredWriteW", CharSet = CharSet.Unicode, SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)] private static extern bool CredWrite(ref Credential credential, uint flags);
    [DllImport("advapi32.dll", EntryPoint = "CredReadW", CharSet = CharSet.Unicode, SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)] private static extern bool CredRead(string target, uint type, uint flags, out IntPtr credential);
    [DllImport("advapi32.dll", EntryPoint = "CredDeleteW", CharSet = CharSet.Unicode, SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)] private static extern bool CredDelete(string target, uint type, uint flags);
    [DllImport("advapi32.dll")] private static extern void CredFree(IntPtr pointer);
    private const string Security = "/System/Library/Frameworks/Security.framework/Security";
    [DllImport(Security)] private static extern int SecKeychainFindGenericPassword(IntPtr keychain, uint serviceLength, byte[] service, uint accountLength, byte[] account, out uint length, out IntPtr data, out IntPtr item);
    [DllImport(Security)] private static extern int SecKeychainAddGenericPassword(IntPtr keychain, uint serviceLength, byte[] service, uint accountLength, byte[] account, uint length, byte[] data, IntPtr item);
    [DllImport(Security)] private static extern int SecKeychainItemModifyAttributesAndData(IntPtr item, IntPtr attributes, uint length, byte[] data);
    [DllImport(Security)] private static extern int SecKeychainItemDelete(IntPtr item);
    [DllImport(Security)] private static extern int SecKeychainItemFreeContent(IntPtr attributes, IntPtr data);
    [DllImport("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation")] private static extern void CFRelease(IntPtr item);
}
