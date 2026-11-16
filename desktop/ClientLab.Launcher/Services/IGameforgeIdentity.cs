using System.Text.Json.Nodes;

namespace NosCore.ClientLab.Launcher.Services;

public interface IGameforgeIdentity
{
    string InstallationId { get; }
    string UserAgent { get; }
    Task<string> CreateAsync(HttpClient http, JsonObject? request = null);
}

public interface IGameforgeVerification
{
    Task CompleteProofOfWorkAsync(string challengeId);
}
