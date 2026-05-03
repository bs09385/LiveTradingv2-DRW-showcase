using System.Text.Json.Serialization;

namespace PolymarketUI.Models;

public class SeriesListMessage
{
    [JsonPropertyName("type")]
    public string Type { get; set; } = "series_list";

    [JsonPropertyName("series")]
    public List<SeriesInfo> Series { get; set; } = new();
}

public class SeriesInfo
{
    [JsonPropertyName("series_key")]
    public string SeriesKey { get; set; } = "";

    [JsonPropertyName("condition_id")]
    public string ConditionId { get; set; } = "";

    [JsonPropertyName("status")]
    public string Status { get; set; } = "DISCONNECTED";

    [JsonPropertyName("has_next")]
    public bool HasNext { get; set; }
}

public class WatcherBooksMessage
{
    [JsonPropertyName("type")]
    public string Type { get; set; } = "watcher_books";

    [JsonPropertyName("series_key")]
    public string SeriesKey { get; set; } = "";

    [JsonPropertyName("condition_id")]
    public string ConditionId { get; set; } = "";

    [JsonPropertyName("buy_levels")]
    public List<WatcherBookLevel> BuyLevels { get; set; } = new();

    [JsonPropertyName("sell_levels")]
    public List<WatcherBookLevel> SellLevels { get; set; } = new();

    [JsonPropertyName("trades")]
    public List<WatcherBookLevel> Trades { get; set; } = new();

    [JsonPropertyName("tick_size")]
    public int TickSize { get; set; } = 100;
}

public class WatcherBookLevel
{
    [JsonPropertyName("price")]
    public int Price { get; set; }

    [JsonPropertyName("size")]
    public long Size { get; set; }

    public string PriceStr => $"{Price / 10000.0:F4}";
}

public class WatcherStatusMessage
{
    [JsonPropertyName("type")]
    public string Type { get; set; } = "watcher_status";

    [JsonPropertyName("series_key")]
    public string SeriesKey { get; set; } = "";

    [JsonPropertyName("status")]
    public string Status { get; set; } = "DISCONNECTED";
}
