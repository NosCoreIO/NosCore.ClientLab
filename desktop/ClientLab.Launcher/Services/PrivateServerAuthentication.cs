using System.Net.Http.Headers;
using System.Net.Http.Json;
using System.Text.Json.Nodes;

namespace NosCore.ClientLab.Launcher.Services;

/// <summary>The same NosCore session/code protocol used by the established browser runner.</summary>
public static class PrivateServerAuthentication
{
    public static async Task<string> SignInAsync(string address, string username, string password, HttpMessageHandler? handler = null)
    {
        if (!Uri.TryCreate(address.TrimEnd('/') + "/", UriKind.Absolute, out var endpoint) || endpoint.Scheme is not ("http" or "https"))
            throw new InvalidOperationException("Enter the private server's authentication URL.");
        if (string.IsNullOrWhiteSpace(username) || string.IsNullOrEmpty(password))
            throw new InvalidOperationException("Enter your private-server account and password.");
        // Local NosCore development uses a self-signed HTTPS certificate. Remote servers
        // retain normal certificate validation.
        handler ??= new HttpClientHandler { ServerCertificateCustomValidationCallback = (request, certificate, chain, errors) =>
            errors == System.Net.Security.SslPolicyErrors.None || request.RequestUri?.IsLoopback == true };
        using var http = new HttpClient(handler) { BaseAddress = endpoint, Timeout = TimeSpan.FromSeconds(15) };
        async Task<JsonObject> Post(string path, JsonObject body)
        {
            using var response = await http.PostAsJsonAsync(path, body);
            if (!response.IsSuccessStatusCode)
                throw new InvalidOperationException($"Private server rejected the sign-in request (HTTP {(int)response.StatusCode}).");
            return (await response.Content.ReadFromJsonAsync<JsonObject>()) ?? throw new InvalidDataException("The private server returned an empty response.");
        }
        var session = await Post("api/v1/auth/thin/sessions", new JsonObject {
            ["identity"] = username.Trim(), ["password"] = password, ["gfLang"] = "EN", ["locale"] = "en-GB", ["mfa"] = null });
        http.DefaultRequestHeaders.Authorization = new AuthenticationHeaderValue("Bearer", session["token"]?.GetValue<string>()
            ?? throw new InvalidDataException("The private server did not return a session token."));
        var code = await Post("api/v1/auth/thin/codes", new JsonObject { ["platformGameAccountId"] = session["platformGameAccountId"]?.DeepClone() });
        return code["code"]?.GetValue<string>() ?? throw new InvalidDataException("The private server did not return a launch code.");
    }
}
