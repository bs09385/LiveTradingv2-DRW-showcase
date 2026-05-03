namespace PolymarketUI.Models;

public class ConfigInfo
{
    public string FilePath { get; set; } = "";

    /// <summary>Relative path from repo root (e.g. "config/default.json")</summary>
    public string RelativePath { get; set; } = "";

    public string DisplayName => System.IO.Path.GetFileNameWithoutExtension(FilePath);
}
