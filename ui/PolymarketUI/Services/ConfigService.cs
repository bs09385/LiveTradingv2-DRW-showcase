using System.IO;
using PolymarketUI.Models;

namespace PolymarketUI.Services;

public class ConfigService
{
    public List<ConfigInfo> LoadConfigs()
    {
        var configs = new List<ConfigInfo>();
        var repoRoot = EngineProcessService.FindRepoRoot();
        if (repoRoot == null) return configs;

        var configDir = Path.Combine(repoRoot, "config");
        if (!Directory.Exists(configDir)) return configs;

        foreach (var file in Directory.GetFiles(configDir, "*.json").OrderBy(f => f))
        {
            var relativePath = Path.GetRelativePath(repoRoot, file).Replace('\\', '/');
            configs.Add(new ConfigInfo
            {
                FilePath = file,
                RelativePath = relativePath
            });
        }

        return configs;
    }
}
