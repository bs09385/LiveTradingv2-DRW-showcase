namespace PolymarketUI.Services;

public static class CommandSerializer
{
    public static string EnableStrategy() => "{\"cmd\":\"enable_strategy\"}";
    public static string DisableStrategy() => "{\"cmd\":\"disable_strategy\"}";
    public static string CancelAll() => "{\"cmd\":\"cancel_all\"}";
    public static string Shutdown() => "{\"cmd\":\"shutdown\"}";

    public static string SetMode(string mode) =>
        $"{{\"cmd\":\"set_mode\",\"mode\":\"{mode}\"}}";

    // Session commands
    public static string StartSession(long endTimeS) =>
        $"{{\"cmd\":\"start_session\",\"end_time\":{endTimeS}}}";

    public static string StartSessionIndefinite() =>
        "{\"cmd\":\"start_session\",\"end_time\":0}";

    public static string StopSession() =>
        "{\"cmd\":\"stop_session\"}";

    // Watcher commands
    public static string WatchSubscribe(string seriesKey) =>
        $"{{\"cmd\":\"watch_subscribe\",\"series_key\":\"{seriesKey}\"}}";

    public static string WatchUnsubscribe(string seriesKey) =>
        $"{{\"cmd\":\"watch_unsubscribe\",\"series_key\":\"{seriesKey}\"}}";

    public static string RequestSeriesList() =>
        "{\"cmd\":\"request_series_list\"}";

    public static string LatencyProbe() =>
        "{\"cmd\":\"latency_probe\"}";

    public static string MarketSellAll() =>
        "{\"cmd\":\"market_sell_all\"}";

    public static string MarketSellDown() =>
        "{\"cmd\":\"market_sell_down\"}";
}
