using System.Globalization;
using System.Text.Json.Serialization;

namespace PolymarketUI.Models;

public static class QtyConvert
{
    public const long Scale = 1_000_000L;
    public static string Format(long scaled) =>
        (scaled / (double)Scale).ToString("F2", CultureInfo.InvariantCulture);
}

public class EngineSnapshot
{
    [JsonPropertyName("type")]
    public string? Type { get; set; }

    [JsonPropertyName("timestamp_ns")]
    public long TimestampNs { get; set; }

    [JsonPropertyName("markets")]
    public List<MarketSnapshot> Markets { get; set; } = new();

    [JsonPropertyName("state")]
    public StateSnapshot? State { get; set; }

    [JsonPropertyName("metrics")]
    public MetricsSnapshot? Metrics { get; set; }

    [JsonPropertyName("gateway")]
    public GatewayHealth? Gateway { get; set; }

    [JsonPropertyName("account")]
    public AccountIdentity? Account { get; set; }

    [JsonPropertyName("rotation")]
    public RotationInfo? Rotation { get; set; }

    [JsonPropertyName("balance")]
    public AccountBalance? Balance { get; set; }

    [JsonPropertyName("latency")]
    public LatencySnapshot? Latency { get; set; }
}

public class AccountIdentity
{
    [JsonPropertyName("name")]
    public string Name { get; set; } = "";

    [JsonPropertyName("address")]
    public string Address { get; set; } = "";
}

public class MarketSnapshot
{
    [JsonPropertyName("condition_id")]
    public string ConditionId { get; set; } = "";

    [JsonPropertyName("token_id_up")]
    public string TokenIdUp { get; set; } = "";

    [JsonPropertyName("token_id_down")]
    public string TokenIdDown { get; set; } = "";

    [JsonPropertyName("series_label")]
    public string SeriesLabel { get; set; } = "";

    [JsonPropertyName("book_up")]
    public BookSnapshot? BookUp { get; set; }

    [JsonPropertyName("book_down")]
    public BookSnapshot? BookDown { get; set; }

    [JsonPropertyName("position_up")]
    public long PositionUp { get; set; }

    [JsonPropertyName("position_down")]
    public long PositionDown { get; set; }
}

public class BookSnapshot
{
    [JsonPropertyName("bbo")]
    public BboData? Bbo { get; set; }

    [JsonPropertyName("bids")]
    public List<BookLevel> Bids { get; set; } = new();

    [JsonPropertyName("asks")]
    public List<BookLevel> Asks { get; set; } = new();
}

public class BboData
{
    [JsonPropertyName("best_bid")]
    public int BestBid { get; set; }

    [JsonPropertyName("best_ask")]
    public int BestAsk { get; set; }

    [JsonPropertyName("bid_size")]
    public long BidSize { get; set; }

    [JsonPropertyName("ask_size")]
    public long AskSize { get; set; }
}

public class BookLevel
{
    [JsonPropertyName("price")]
    public int Price { get; set; }

    [JsonPropertyName("size")]
    public long Size { get; set; }

    public string PriceStr => $"{Price / 10000.0:F4}";
}

public class StateSnapshot
{
    [JsonPropertyName("working_orders")]
    public List<WorkingOrderModel> WorkingOrders { get; set; } = new();

    [JsonPropertyName("closed_orders")]
    public List<WorkingOrderModel> ClosedOrders { get; set; } = new();

    [JsonPropertyName("trades")]
    public List<TradeHistoryModel> Trades { get; set; } = new();

    [JsonPropertyName("strategy_enabled")]
    public bool StrategyEnabled { get; set; }

    [JsonPropertyName("spread_ticks")]
    public int SpreadTicks { get; set; }

    [JsonPropertyName("quote_size")]
    public long QuoteSize { get; set; }
    public string QuoteSizeStr => QtyConvert.Format(QuoteSize);

    [JsonPropertyName("execution_mode")]
    public string ExecutionMode { get; set; } = "DRY_RUN";

    [JsonPropertyName("risk_checks")]
    public long RiskChecks { get; set; }

    [JsonPropertyName("risk_allows")]
    public long RiskAllows { get; set; }

    [JsonPropertyName("risk_denies")]
    public long RiskDenies { get; set; }

    [JsonPropertyName("session_state")]
    public string SessionState { get; set; } = "IDLE";

    [JsonPropertyName("session_end_time_s")]
    public long SessionEndTimeS { get; set; }

    [JsonPropertyName("session_markets_entered")]
    public int SessionMarketsEntered { get; set; }

    [JsonPropertyName("positions")]
    public List<TokenPositionModel> Positions { get; set; } = new();
}

public class TokenPositionModel
{
    [JsonPropertyName("token_id")]
    public string TokenId { get; set; } = "";

    [JsonPropertyName("position")]
    public long Position { get; set; }
}

public class WorkingOrderModel
{
    [JsonPropertyName("client_order_id")]
    public string ClientOrderId { get; set; } = "";

    [JsonPropertyName("exchange_order_id")]
    public string ExchangeOrderId { get; set; } = "";

    [JsonPropertyName("asset_id")]
    public string AssetId { get; set; } = "";

    [JsonPropertyName("market_id")]
    public string MarketId { get; set; } = "";

    [JsonPropertyName("market_label")]
    public string MarketLabel { get; set; } = "";

    [JsonPropertyName("side")]
    public string Side { get; set; } = "";

    [JsonPropertyName("price")]
    public int Price { get; set; }

    [JsonPropertyName("original_size")]
    public long OriginalSize { get; set; }

    [JsonPropertyName("filled_size")]
    public long FilledSize { get; set; }

    [JsonPropertyName("is_live")]
    public bool IsLive { get; set; }

    [JsonPropertyName("is_pending")]
    public bool IsPending { get; set; }

    [JsonPropertyName("lifecycle_state")]
    public string LifecycleState { get; set; } = "WORKING";

    [JsonPropertyName("last_update_ts")]
    public long LastUpdateTs { get; set; }

    public string TimeStr => LastUpdateTs > 0
        ? DateTimeOffset.FromUnixTimeMilliseconds(LastUpdateTs / 1_000_000).ToLocalTime().ToString("HH:mm:ss.fff")
        : "";

    public string PriceStr => $"{Price / 10000.0:F4}";
    public long Remaining => OriginalSize - FilledSize;
    private static string FormatQty(long qty) => QtyConvert.Format(qty);
    public string OriginalSizeStr => FormatQty(OriginalSize);
    public string FilledSizeStr => FormatQty(FilledSize);
    public string RemainingStr => FormatQty(Remaining);
    public string Status
    {
        get
        {
            if (!string.IsNullOrWhiteSpace(LifecycleState))
                return LifecycleState;

            if (IsPending) return "WORKING";
            return IsLive ? "WORKING" : "UNKNOWN";
        }
    }
}

public class TradeHistoryModel
{
    [JsonPropertyName("trade_id")]
    public string TradeId { get; set; } = "";

    [JsonPropertyName("order_id")]
    public string OrderId { get; set; } = "";

    [JsonPropertyName("asset_id")]
    public string AssetId { get; set; } = "";

    [JsonPropertyName("market_id")]
    public string MarketId { get; set; } = "";

    [JsonPropertyName("market_label")]
    public string MarketLabel { get; set; } = "";

    [JsonPropertyName("side")]
    public string Side { get; set; } = "";

    [JsonPropertyName("price")]
    public int Price { get; set; }

    [JsonPropertyName("size")]
    public long Size { get; set; }

    [JsonPropertyName("status")]
    public string Status { get; set; } = "UNKNOWN";

    [JsonPropertyName("last_update_ts")]
    public long LastUpdateTs { get; set; }

    public string TimeStr => LastUpdateTs > 0
        ? DateTimeOffset.FromUnixTimeMilliseconds(LastUpdateTs / 1_000_000).ToLocalTime().ToString("HH:mm:ss.fff")
        : "";

    public string PriceStr => $"{Price / 10000.0:F4}";
    public string SizeStr => QtyConvert.Format(Size);
    public string BuySell => Side switch
    {
        "BID" => "BUY",
        "ASK" => "SELL",
        _ => Side
    };
    public bool IsFillRelatedState =>
        Status.Equals("MATCHED", StringComparison.OrdinalIgnoreCase) ||
        Status.Equals("MINED", StringComparison.OrdinalIgnoreCase) ||
        Status.Equals("CONFIRMED", StringComparison.OrdinalIgnoreCase);
}

public class MetricsSnapshot
{
    [JsonPropertyName("ws_frames")]
    public long WsFrames { get; set; }

    [JsonPropertyName("parse_ok")]
    public long ParseOk { get; set; }

    [JsonPropertyName("sched_cycles")]
    public long SchedCycles { get; set; }

    [JsonPropertyName("sched_events")]
    public long SchedEvents { get; set; }

    [JsonPropertyName("rest_requests")]
    public long RestRequests { get; set; }

    [JsonPropertyName("rest_errors")]
    public long RestErrors { get; set; }

    [JsonPropertyName("ui_snapshots_dropped")]
    public long UiSnapshotsDropped { get; set; }

    [JsonPropertyName("ui_book_drops")]
    public long UiBookDrops { get; set; }

    [JsonPropertyName("ui_state_drops")]
    public long UiStateDrops { get; set; }
}

public class GatewayHealth
{
    [JsonPropertyName("degraded")]
    public bool Degraded { get; set; }

    [JsonPropertyName("heartbeat_ok")]
    public long HeartbeatOk { get; set; }

    [JsonPropertyName("heartbeat_fail")]
    public long HeartbeatFail { get; set; }
}

public class RotationInfo
{
    [JsonPropertyName("market")]
    public string Market { get; set; } = "";

    [JsonPropertyName("window_start")]
    public long WindowStart { get; set; }

    [JsonPropertyName("window_end")]
    public long WindowEnd { get; set; }

    [JsonPropertyName("rotations")]
    public int Rotations { get; set; }

    [JsonPropertyName("no_trade")]
    public bool InNoTrade { get; set; }
}

public class AccountBalance
{
    [JsonPropertyName("position_value")]
    public double PositionValue { get; set; }

    [JsonPropertyName("usdc_balance")]
    public double UsdcBalance { get; set; }

    [JsonPropertyName("realized_pnl")]
    public double RealizedPnl { get; set; }

    [JsonPropertyName("pol_balance")]
    public double PolBalance { get; set; }

    [JsonPropertyName("available")]
    public bool Available { get; set; }

    [JsonPropertyName("error")]
    public string Error { get; set; } = "";
}

public class LatencySnapshot
{
    [JsonPropertyName("order_rtt_avg_ns")] public long OrderRttAvgNs { get; set; }
    [JsonPropertyName("order_rtt_p95_ns")] public long OrderRttP95Ns { get; set; }
    [JsonPropertyName("cancel_rtt_avg_ns")] public long CancelRttAvgNs { get; set; }
    [JsonPropertyName("cancel_rtt_p95_ns")] public long CancelRttP95Ns { get; set; }
    [JsonPropertyName("engine_avg_ns")] public long EngineAvgNs { get; set; }
    [JsonPropertyName("engine_p95_ns")] public long EngineP95Ns { get; set; }
    [JsonPropertyName("pipeline_avg_ns")] public long PipelineAvgNs { get; set; }
    [JsonPropertyName("pipeline_p95_ns")] public long PipelineP95Ns { get; set; }
    [JsonPropertyName("ws_book_avg_ns")] public long WsBookAvgNs { get; set; }
    [JsonPropertyName("ws_book_p95_ns")] public long WsBookP95Ns { get; set; }
    [JsonPropertyName("exchange_to_recv_avg_ns")] public long ExchangeToRecvAvgNs { get; set; }
    [JsonPropertyName("exchange_to_recv_p95_ns")] public long ExchangeToRecvP95Ns { get; set; }
    [JsonPropertyName("binance_md_avg_ns")] public long BinanceMdAvgNs { get; set; }
    [JsonPropertyName("binance_md_p95_ns")] public long BinanceMdP95Ns { get; set; }
    [JsonPropertyName("probe_order_rtt_ns")] public long ProbeOrderRttNs { get; set; }
    [JsonPropertyName("probe_cancel_rtt_ns")] public long ProbeCancelRttNs { get; set; }
    [JsonPropertyName("probe_roundtrip_ns")] public long ProbeRoundtripNs { get; set; }
    [JsonPropertyName("probe_status")] public int ProbeStatus { get; set; }

    public string OrderRttAvgMs => FormatMs(OrderRttAvgNs);
    public string OrderRttP95Ms => FormatMs(OrderRttP95Ns);
    public string CancelRttAvgMs => FormatMs(CancelRttAvgNs);
    public string CancelRttP95Ms => FormatMs(CancelRttP95Ns);
    public string EngineAvgUs => FormatUs(EngineAvgNs);
    public string EngineP95Us => FormatUs(EngineP95Ns);
    public string WsBookAvgUs => FormatUs(WsBookAvgNs);
    public string WsBookP95Us => FormatUs(WsBookP95Ns);
    public string ExchRecvAvgMs => FormatMs(ExchangeToRecvAvgNs);
    public string ExchRecvP95Ms => FormatMs(ExchangeToRecvP95Ns);
    // Binance MD measures Binance exchange-timestamp -> local-recv wall-clock
    // delta — i.e. network/exchange latency. Real values are tens of ms.
    public string BinanceMdAvgMs => FormatMs(BinanceMdAvgNs);
    public string BinanceMdP95Ms => FormatMs(BinanceMdP95Ns);

    public string ProbeStatusText => ProbeStatus switch
    {
        0 => "Ready",
        1 => "Running...",
        2 => $"Order {FormatMs(ProbeOrderRttNs)} / Cancel {FormatMs(ProbeCancelRttNs)} ms",
        3 => "Failed",
        _ => "Unknown"
    };

    private static string FormatMs(long ns) => $"{ns / 1_000_000.0:F1}";
    private static string FormatUs(long ns) => $"{ns / 1_000.0:F1}";
}
