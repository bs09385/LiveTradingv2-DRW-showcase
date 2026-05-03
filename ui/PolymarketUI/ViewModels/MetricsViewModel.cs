using System;
using PolymarketUI.Models;

namespace PolymarketUI.ViewModels;

public class MetricsViewModel : ViewModelBase
{
    private string _utcClock = "";
    public string UtcClock { get => _utcClock; set => SetField(ref _utcClock, value); }

    private long _wsFrames;
    public long WsFrames { get => _wsFrames; set => SetField(ref _wsFrames, value); }

    private long _parseOk;
    public long ParseOk { get => _parseOk; set => SetField(ref _parseOk, value); }

    private long _schedCycles;
    public long SchedCycles { get => _schedCycles; set => SetField(ref _schedCycles, value); }

    private long _schedEvents;
    public long SchedEvents { get => _schedEvents; set => SetField(ref _schedEvents, value); }

    private long _restRequests;
    public long RestRequests { get => _restRequests; set => SetField(ref _restRequests, value); }

    private long _restErrors;
    public long RestErrors { get => _restErrors; set => SetField(ref _restErrors, value); }

    private long _uiSnapshotsDropped;
    public long UiSnapshotsDropped { get => _uiSnapshotsDropped; set => SetField(ref _uiSnapshotsDropped, value); }

    private long _uiBookDrops;
    public long UiBookDrops { get => _uiBookDrops; set => SetField(ref _uiBookDrops, value); }

    private long _uiStateDrops;
    public long UiStateDrops { get => _uiStateDrops; set => SetField(ref _uiStateDrops, value); }

    private bool _gatewayDegraded;
    public bool GatewayDegraded { get => _gatewayDegraded; set => SetField(ref _gatewayDegraded, value); }

    private long _heartbeatOk;
    public long HeartbeatOk { get => _heartbeatOk; set => SetField(ref _heartbeatOk, value); }

    private long _heartbeatFail;
    public long HeartbeatFail { get => _heartbeatFail; set => SetField(ref _heartbeatFail, value); }

    private long _riskChecks;
    public long RiskChecks { get => _riskChecks; set => SetField(ref _riskChecks, value); }

    private long _riskAllows;
    public long RiskAllows { get => _riskAllows; set => SetField(ref _riskAllows, value); }

    private long _riskDenies;
    public long RiskDenies { get => _riskDenies; set => SetField(ref _riskDenies, value); }

    // Latency display properties
    private string _orderRttDisplay = "-";
    public string OrderRttDisplay { get => _orderRttDisplay; set => SetField(ref _orderRttDisplay, value); }

    private string _cancelRttDisplay = "-";
    public string CancelRttDisplay { get => _cancelRttDisplay; set => SetField(ref _cancelRttDisplay, value); }

    private string _engineSpeedDisplay = "-";
    public string EngineSpeedDisplay { get => _engineSpeedDisplay; set => SetField(ref _engineSpeedDisplay, value); }

    private string _wsBookDisplay = "-";
    public string WsBookDisplay { get => _wsBookDisplay; set => SetField(ref _wsBookDisplay, value); }

    private string _exchRecvDisplay = "-";
    public string ExchRecvDisplay { get => _exchRecvDisplay; set => SetField(ref _exchRecvDisplay, value); }

    private string _binanceMdDisplay = "-";
    public string BinanceMdDisplay { get => _binanceMdDisplay; set => SetField(ref _binanceMdDisplay, value); }

    private string _probeStatusDisplay = "Ready";
    public string ProbeStatusDisplay { get => _probeStatusDisplay; set => SetField(ref _probeStatusDisplay, value); }

    public void Update(MetricsSnapshot? metrics, GatewayHealth? gateway, StateSnapshot? state, LatencySnapshot? latency = null)
    {
        UtcClock = DateTimeOffset.UtcNow.ToString("HH:mm:ss") + " UTC";

        if (metrics != null)
        {
            WsFrames = metrics.WsFrames;
            ParseOk = metrics.ParseOk;
            SchedCycles = metrics.SchedCycles;
            SchedEvents = metrics.SchedEvents;
            RestRequests = metrics.RestRequests;
            RestErrors = metrics.RestErrors;
            UiSnapshotsDropped = metrics.UiSnapshotsDropped;
            UiBookDrops = metrics.UiBookDrops;
            UiStateDrops = metrics.UiStateDrops;
        }

        if (gateway != null)
        {
            GatewayDegraded = gateway.Degraded;
            HeartbeatOk = gateway.HeartbeatOk;
            HeartbeatFail = gateway.HeartbeatFail;
        }

        if (state != null)
        {
            RiskChecks = state.RiskChecks;
            RiskAllows = state.RiskAllows;
            RiskDenies = state.RiskDenies;
        }

        if (latency != null)
        {
            OrderRttDisplay = latency.OrderRttAvgNs > 0
                ? $"{latency.OrderRttAvgMs} / {latency.OrderRttP95Ms} ms"
                : "-";
            CancelRttDisplay = latency.CancelRttAvgNs > 0
                ? $"{latency.CancelRttAvgMs} / {latency.CancelRttP95Ms} ms"
                : "-";
            EngineSpeedDisplay = latency.EngineAvgNs > 0
                ? $"{latency.EngineAvgUs} / {latency.EngineP95Us} us"
                : "-";
            WsBookDisplay = latency.WsBookAvgNs > 0
                ? $"{latency.WsBookAvgUs} / {latency.WsBookP95Us} us"
                : "-";
            ExchRecvDisplay = latency.ExchangeToRecvAvgNs > 0
                ? $"{latency.ExchRecvAvgMs} / {latency.ExchRecvP95Ms} ms"
                : "-";
            BinanceMdDisplay = latency.BinanceMdAvgNs > 0
                ? $"{latency.BinanceMdAvgMs} / {latency.BinanceMdP95Ms} ms"
                : "-";
            ProbeStatusDisplay = latency.ProbeStatusText;
        }
    }
}
