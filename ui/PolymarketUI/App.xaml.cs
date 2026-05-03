using System.IO;
using System.Text.Json;
using System.Windows;

namespace PolymarketUI;

public partial class App : Application
{
    public static string EngineUri { get; private set; } = "ws://localhost:9090";
    public static string AuthToken { get; private set; } = "";

    protected override void OnStartup(StartupEventArgs e)
    {
        base.OnStartup(e);
        LoadSettings();
    }

    private void LoadSettings()
    {
        try
        {
            var settingsPath = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "appsettings.json");
            if (File.Exists(settingsPath))
            {
                var json = File.ReadAllText(settingsPath);
                using var doc = JsonDocument.Parse(json);
                var root = doc.RootElement;

                if (root.TryGetProperty("engine_uri", out var uri))
                    EngineUri = uri.GetString() ?? EngineUri;
                if (root.TryGetProperty("auth_token", out var token))
                    AuthToken = token.GetString() ?? AuthToken;
            }
        }
        catch
        {
            // Use defaults if settings file is missing or invalid
        }
    }
}
