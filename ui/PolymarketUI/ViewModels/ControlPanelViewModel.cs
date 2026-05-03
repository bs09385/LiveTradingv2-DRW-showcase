using System;
using System.Globalization;
using System.Windows.Input;
using PolymarketUI.Services;

namespace PolymarketUI.ViewModels;

public class ControlPanelViewModel : ViewModelBase
{
    private EngineConnectionService _connection;
    private bool _updatingFromSnapshot;

    private bool _strategyEnabled;
    public bool StrategyEnabled
    {
        get => _strategyEnabled;
        set
        {
            if (!SetField(ref _strategyEnabled, value)) return;
            OnPropertyChanged(nameof(IsProbeAllowed));
            OnPropertyChanged(nameof(StrategyToggleText));
        }
    }

    public string StrategyToggleText => _strategyEnabled ? "Off" : "On";

    private string _executionMode = "DRY_RUN";
    public string ExecutionMode
    {
        get => _executionMode;
        set
        {
            if (!SetField(ref _executionMode, value)) return;
            OnPropertyChanged(nameof(IsDryRun));
            OnPropertyChanged(nameof(ModeToggleText));
        }
    }

    public bool IsDryRun => _executionMode == "DRY_RUN";
    public string ModeToggleText => IsDryRun ? "On" : "Off";

    // Trading session state
    private string _sessionState = "IDLE";
    public string SessionState
    {
        get => _sessionState;
        set
        {
            if (!SetField(ref _sessionState, value)) return;
            OnPropertyChanged(nameof(IsSessionIdle));
            OnPropertyChanged(nameof(IsSessionActive));
            OnPropertyChanged(nameof(SessionStatusText));
        }
    }

    private bool _isIndefinite = true;
    public bool IsIndefinite
    {
        get => _isIndefinite;
        set
        {
            if (!SetField(ref _isIndefinite, value)) return;
            OnPropertyChanged(nameof(IsNotIndefinite));
        }
    }
    public bool IsNotIndefinite => !_isIndefinite;

    private string _endTimeText = "";
    public string EndTimeText { get => _endTimeText; set => SetField(ref _endTimeText, value); }

    private int _sessionMarketsEntered;
    public int SessionMarketsEntered { get => _sessionMarketsEntered; set => SetField(ref _sessionMarketsEntered, value); }

    public bool IsSessionIdle => _sessionState == "IDLE";
    public bool IsSessionActive => _sessionState != "IDLE";

    public string SessionStatusText => _sessionState switch
    {
        "IDLE" => "Idle",
        "PENDING" => "Waiting for next market...",
        "ACTIVE" => "LIVE — trading",
        _ => _sessionState
    };

    private int _spreadTicks = 1;
    public int SpreadTicks { get => _spreadTicks; set => SetField(ref _spreadTicks, value); }

    private int _quoteSize = 5;
    public int QuoteSize { get => _quoteSize; set => SetField(ref _quoteSize, value); }

    public ICommand EnableStrategyCommand { get; }
    public ICommand DisableStrategyCommand { get; }
    public ICommand ToggleStrategyCommand { get; }
    public ICommand ToggleModeCommand { get; }
    public ICommand CancelAllCommand { get; }
    public ICommand StartSessionCommand { get; }
    public ICommand StopSessionCommand { get; }
    public ICommand ProbeLatencyCommand { get; }
    public ICommand SetDryRunCommand { get; }
    public ICommand SetLiveCommand { get; }
    public ICommand MarketSellAllCommand { get; }
    public ICommand MarketSellDownCommand { get; }

    public bool IsProbeAllowed => !_strategyEnabled;

    public ControlPanelViewModel(EngineConnectionService connection)
    {
        _connection = connection;

        EnableStrategyCommand = new RelayCommand(async () =>
            await _connection.SendCommandAsync(CommandSerializer.EnableStrategy()));

        DisableStrategyCommand = new RelayCommand(async () =>
            await _connection.SendCommandAsync(CommandSerializer.DisableStrategy()));

        ToggleStrategyCommand = new RelayCommand(async () =>
        {
            if (_strategyEnabled)
                await _connection.SendCommandAsync(CommandSerializer.DisableStrategy());
            else
                await _connection.SendCommandAsync(CommandSerializer.EnableStrategy());
        });

        ToggleModeCommand = new RelayCommand(async () =>
        {
            if (IsDryRun)
                await _connection.SendCommandAsync(CommandSerializer.SetMode("LIVE"));
            else
                await _connection.SendCommandAsync(CommandSerializer.SetMode("DRY_RUN"));
        });

        CancelAllCommand = new RelayCommand(async () =>
            await _connection.SendCommandAsync(CommandSerializer.CancelAll()));

        StartSessionCommand = new RelayCommand(async () =>
        {
            if (_isIndefinite)
            {
                await _connection.SendCommandAsync(CommandSerializer.StartSessionIndefinite());
            }
            else
            {
                long endTimeS = ParseEndTime(_endTimeText);
                if (endTimeS > 0)
                    await _connection.SendCommandAsync(CommandSerializer.StartSession(endTimeS));
            }
        });

        StopSessionCommand = new RelayCommand(async () =>
            await _connection.SendCommandAsync(CommandSerializer.StopSession()));

        ProbeLatencyCommand = new RelayCommand(async () =>
            await _connection.SendCommandAsync(CommandSerializer.LatencyProbe()));

        SetDryRunCommand = new RelayCommand(async () =>
            await _connection.SendCommandAsync(CommandSerializer.SetMode("DRY_RUN")));

        SetLiveCommand = new RelayCommand(async () =>
            await _connection.SendCommandAsync(CommandSerializer.SetMode("LIVE")));

        MarketSellAllCommand = new RelayCommand(async () =>
            await _connection.SendCommandAsync(CommandSerializer.MarketSellAll()));

        MarketSellDownCommand = new RelayCommand(async () =>
            await _connection.SendCommandAsync(CommandSerializer.MarketSellDown()));
    }

    public void UpdateConnection(EngineConnectionService connection) => _connection = connection;

    public void UpdateFromState(Models.StateSnapshot? state)
    {
        if (state == null) return;

        _updatingFromSnapshot = true;

        StrategyEnabled = state.StrategyEnabled;
        SpreadTicks = state.SpreadTicks;
        QuoteSize = (int)state.QuoteSize;
        ExecutionMode = state.ExecutionMode;
        SessionState = state.SessionState;
        SessionMarketsEntered = state.SessionMarketsEntered;

        _updatingFromSnapshot = false;
    }

    /// <summary>
    /// Parse "HH:mm" end time string to UTC epoch seconds (today or tomorrow).
    /// Returns 0 on invalid input.
    /// </summary>
    private static long ParseEndTime(string text)
    {
        if (string.IsNullOrWhiteSpace(text)) return 0;
        if (!TimeSpan.TryParseExact(text.Trim(), @"hh\:mm", CultureInfo.InvariantCulture, out var time))
            return 0;

        var now = DateTimeOffset.UtcNow;
        var target = now.Date + time;
        var targetUtc = new DateTimeOffset(target, TimeSpan.Zero);
        if (targetUtc <= now) targetUtc = targetUtc.AddDays(1);  // next occurrence
        return targetUtc.ToUnixTimeSeconds();
    }
}
