using System.Collections.ObjectModel;
using System.Threading;
using System.Windows;
using System.Windows.Input;
using System.Windows.Threading;
using PolymarketUI.Models;
using PolymarketUI.Services;
using PolymarketUI.Views;

namespace PolymarketUI.ViewModels;

public class MainViewModel : ViewModelBase, IDisposable
{
    private EngineConnectionService _connection;
    private readonly AccountService _accountService = new();
    private readonly ConfigService _configService = new();
    private readonly EngineProcessService _engineProcess = new();
    private readonly object _snapshotGate = new();
    private readonly object _statusGate = new();
    private EngineSnapshot? _pendingSnapshot;
    private string? _pendingStatusText;
    private int _snapshotApplyQueued;
    private int _statusApplyQueued;

    private bool _isConnected;
    public bool IsConnected { get => _isConnected; set => SetField(ref _isConnected, value); }

    private string _statusText = "Disconnected";
    public string StatusText { get => _statusText; set => SetField(ref _statusText, value); }

    private string _connectionUri = "ws://localhost:9090";
    public string ConnectionUri { get => _connectionUri; set => SetField(ref _connectionUri, value); }

    // Account selection
    public ObservableCollection<AccountInfo> Accounts { get; } = new();

    private AccountInfo? _selectedAccount;
    public AccountInfo? SelectedAccount { get => _selectedAccount; set => SetField(ref _selectedAccount, value); }

    // Config selection
    public ObservableCollection<ConfigInfo> Configs { get; } = new();

    private ConfigInfo? _selectedConfig;
    public ConfigInfo? SelectedConfig { get => _selectedConfig; set => SetField(ref _selectedConfig, value); }

    public string ConfigPath => SelectedConfig?.RelativePath ?? "config/default.json";

    private bool _isEngineRunning;
    public bool IsEngineRunning { get => _isEngineRunning; set => SetField(ref _isEngineRunning, value); }

    private string _activeAccountName = "";
    public string ActiveAccountName { get => _activeAccountName; set => SetField(ref _activeAccountName, value); }

    private string _activeAccountAddress = "";
    public string ActiveAccountAddress { get => _activeAccountAddress; set => SetField(ref _activeAccountAddress, value); }

    private string _positionValue = "--";
    public string PositionValue { get => _positionValue; set => SetField(ref _positionValue, value); }

    private string _usdcBalance = "--";
    public string UsdcBalance { get => _usdcBalance; set => SetField(ref _usdcBalance, value); }

    private string _realizedPnl = "$0.00";
    public string RealizedPnl { get => _realizedPnl; set => SetField(ref _realizedPnl, value); }

    private string _polBalance = "--";
    public string PolBalance { get => _polBalance; set => SetField(ref _polBalance, value); }

    private string _pnlColor = "Black";
    public string PnlColor { get => _pnlColor; set => SetField(ref _pnlColor, value); }

    // Rotation display
    private string _rotationMarket = "";
    public string RotationMarket { get => _rotationMarket; set => SetField(ref _rotationMarket, value); }

    private string _rotationTimeRemaining = "";
    public string RotationTimeRemaining { get => _rotationTimeRemaining; set => SetField(ref _rotationTimeRemaining, value); }

    private int _rotationCount;
    public int RotationCount { get => _rotationCount; set => SetField(ref _rotationCount, value); }

    private bool _isInNoTradeZone;
    public bool IsInNoTradeZone { get => _isInNoTradeZone; set => SetField(ref _isInNoTradeZone, value); }

    private bool _rotationActive;
    public bool RotationActive { get => _rotationActive; set => SetField(ref _rotationActive, value); }

    // Child ViewModels
    public PositionsViewModel Positions { get; }
    public WorkingOrdersViewModel WorkingOrders { get; }
    public MetricsViewModel Metrics { get; }
    public ControlPanelViewModel ControlPanel { get; }
    public MarketPickerViewModel MarketPicker { get; }

    // Ladder windows (one per series key)
    private readonly Dictionary<string, LadderWindow> _ladderWindows = new();
    private readonly Dictionary<string, LadderViewModel> _ladderViewModels = new();

    // Series keys to auto-open
    private static readonly string[] AutoLadderKeys = { "BTC_5m", "BTC_15m" };

    public ICommand ConnectCommand { get; }
    public ICommand DisconnectCommand { get; }
    public ICommand StartEngineCommand { get; }
    public ICommand StopEngineCommand { get; }
    public ICommand RefreshAccountsCommand { get; }

    public MainViewModel()
    {
        _connection = new EngineConnectionService(App.EngineUri, App.AuthToken);
        ConnectionUri = App.EngineUri;
        Positions = new PositionsViewModel();
        WorkingOrders = new WorkingOrdersViewModel();
        Metrics = new MetricsViewModel();
        ControlPanel = new ControlPanelViewModel(_connection);
        MarketPicker = new MarketPickerViewModel(ToggleLadder);

        ConnectCommand = new RelayCommand(async () =>
        {
            UnwireEvents(_connection);
            _connection.Dispose();
            _connection = new EngineConnectionService(ConnectionUri, App.AuthToken);
            ControlPanel.UpdateConnection(_connection);
            WireEvents(_connection);
            await _connection.ConnectAsync();
        });

        DisconnectCommand = new RelayCommand(async () =>
        {
            await _connection.DisconnectAsync();
        });

        StartEngineCommand = new RelayCommand(OnStartEngine);
        StopEngineCommand = new RelayCommand(OnStopEngine);
        RefreshAccountsCommand = new RelayCommand(RefreshAccounts);

        // Wire engine process events
        _engineProcess.OutputReceived += msg =>
            QueueStatusText(msg);
        _engineProcess.ErrorOccurred += msg =>
            QueueStatusText($"Engine error: {msg}");
        _engineProcess.EngineExited += () =>
            Application.Current?.Dispatcher.BeginInvoke(DispatcherPriority.Background, new Action(() =>
            {
                IsEngineRunning = false;
                StatusText = "Engine exited";
            }));

        WireEvents(_connection);

        // Load accounts and configs on startup
        RefreshAccounts();
        RefreshConfigs();
    }

    private void RefreshAccounts()
    {
        var accounts = _accountService.LoadAccounts();
        Accounts.Clear();
        foreach (var acct in accounts)
            Accounts.Add(acct);

        if (Accounts.Count > 0 && SelectedAccount == null)
            SelectedAccount = Accounts[0];
    }

    private void RefreshConfigs()
    {
        var configs = _configService.LoadConfigs();
        Configs.Clear();
        foreach (var cfg in configs)
            Configs.Add(cfg);

        // Default to "default" config if available
        SelectedConfig = Configs.FirstOrDefault(c => c.DisplayName == "default")
                         ?? Configs.FirstOrDefault();
    }

    private async void OnStartEngine()
    {
        if (_engineProcess.IsRunning)
        {
            StatusText = "Engine is already running";
            return;
        }

        if (SelectedAccount == null)
        {
            StatusText = "Select an account before starting the engine";
            return;
        }

        StatusText = $"Starting engine: {ConfigPath} {SelectedAccount.DisplayName}...";
        _engineProcess.Start(ConfigPath, SelectedAccount.FilePath);
        IsEngineRunning = _engineProcess.IsRunning;

        if (IsEngineRunning)
        {
            // Auto-connect WS after a short delay for engine startup
            await Task.Delay(1500);

            if (_engineProcess.IsRunning)
            {
                UnwireEvents(_connection);
                _connection.Dispose();
                _connection = new EngineConnectionService(ConnectionUri, App.AuthToken);
                ControlPanel.UpdateConnection(_connection);
                WireEvents(_connection);
                await _connection.ConnectAsync();
            }
        }
    }

    private async void OnStopEngine()
    {
        // Send graceful shutdown command via WS first
        if (_connection.IsConnected)
        {
            await _connection.SendCommandAsync(CommandSerializer.Shutdown());
        }

        _engineProcess.Stop();
        IsEngineRunning = false;
        StatusText = "Engine stopped";
    }

    private void WireEvents(EngineConnectionService conn)
    {
        conn.SnapshotReceived += OnSnapshot;
        conn.ConnectionChanged += OnConnectionChanged;
        conn.ErrorOccurred += OnError;
        conn.WatcherRouter.SeriesListReceived += OnSeriesListReceived;
        conn.WatcherRouter.WatcherBooksReceived += OnWatcherBooksReceived;
        conn.WatcherRouter.WatcherStatusReceived += OnWatcherStatusReceived;
    }

    private void UnwireEvents(EngineConnectionService conn)
    {
        conn.SnapshotReceived -= OnSnapshot;
        conn.ConnectionChanged -= OnConnectionChanged;
        conn.ErrorOccurred -= OnError;
        conn.WatcherRouter.SeriesListReceived -= OnSeriesListReceived;
        conn.WatcherRouter.WatcherBooksReceived -= OnWatcherBooksReceived;
        conn.WatcherRouter.WatcherStatusReceived -= OnWatcherStatusReceived;
    }

    private void OnConnectionChanged(bool connected)
    {
        Application.Current?.Dispatcher.BeginInvoke(DispatcherPriority.Background, new Action(() =>
        {
            IsConnected = connected;
            StatusText = connected ? "Connected" : "Disconnected";

            if (connected)
            {
                AutoSubscribeAndOpenLadders();
            }
        }));
    }

    private async void AutoSubscribeAndOpenLadders()
    {
        // Subscribe watchers for both timeframes
        foreach (var key in AutoLadderKeys)
        {
            await _connection.SendCommandAsync(CommandSerializer.WatchSubscribe(key));
        }
    }

    private void ToggleLadder(string seriesKey)
    {
        if (_ladderWindows.ContainsKey(seriesKey))
            CloseLadder(seriesKey);
        else
            OpenLadder(seriesKey);
    }

    private void OpenLadder(string seriesKey)
    {
        // If window already exists, bring to front
        if (_ladderWindows.TryGetValue(seriesKey, out var existing))
        {
            existing.Activate();
            return;
        }

        var ladderVm = new LadderViewModel(seriesKey);
        _ladderViewModels[seriesKey] = ladderVm;

        var window = new LadderWindow(ladderVm);
        _ladderWindows[seriesKey] = window;

        window.Closed += (_, _) =>
        {
            _ladderWindows.Remove(seriesKey);
            _ladderViewModels.Remove(seriesKey);
            var item = MarketPicker.FindByKey(seriesKey);
            if (item != null) item.IsLadderOpen = false;
        };

        var item = MarketPicker.FindByKey(seriesKey);
        if (item != null) item.IsLadderOpen = true;

        window.Show();
    }

    private void CloseLadder(string seriesKey)
    {
        if (_ladderWindows.TryGetValue(seriesKey, out var window))
        {
            window.Close();
        }
    }

    private void OnError(string error)
    {
        Application.Current?.Dispatcher.BeginInvoke(DispatcherPriority.Background, new Action(() =>
        {
            StatusText = $"Error: {error}";
        }));
    }

    private void OnSnapshot(EngineSnapshot snapshot)
    {
        lock (_snapshotGate)
        {
            _pendingSnapshot = snapshot;
        }
        if (Interlocked.Exchange(ref _snapshotApplyQueued, 1) != 0)
            return;

        Application.Current?.Dispatcher.BeginInvoke(DispatcherPriority.Background,
            new Action(ApplyQueuedSnapshot));
    }

    // --- Watcher event handlers ---

    private void OnSeriesListReceived(SeriesListMessage msg)
    {
        Application.Current?.Dispatcher.BeginInvoke(DispatcherPriority.Background, new Action(() =>
        {
            MarketPicker.OnSeriesListReceived(msg);
        }));
    }

    private void OnWatcherBooksReceived(WatcherBooksMessage msg)
    {
        Application.Current?.Dispatcher.BeginInvoke(DispatcherPriority.Background, new Action(() =>
        {
            if (_ladderViewModels.TryGetValue(msg.SeriesKey, out var vm))
            {
                vm.OnBooksReceived(msg);
            }
        }));
    }

    private void OnWatcherStatusReceived(WatcherStatusMessage msg)
    {
        Application.Current?.Dispatcher.BeginInvoke(DispatcherPriority.Background, new Action(() =>
        {
            MarketPicker.OnWatcherStatusReceived(msg);
            if (_ladderViewModels.TryGetValue(msg.SeriesKey, out var vm))
            {
                vm.OnStatusReceived(msg);
            }
        }));
    }

    private void QueueStatusText(string message)
    {
        lock (_statusGate)
        {
            _pendingStatusText = message;
        }
        if (Interlocked.Exchange(ref _statusApplyQueued, 1) != 0)
            return;

        Application.Current?.Dispatcher.BeginInvoke(DispatcherPriority.Background,
            new Action(ApplyQueuedStatusText));
    }

    private void ApplyQueuedStatusText()
    {
        string? message;
        lock (_statusGate)
        {
            message = _pendingStatusText;
            _pendingStatusText = null;
        }
        if (!string.IsNullOrEmpty(message))
        {
            StatusText = message;
        }

        Interlocked.Exchange(ref _statusApplyQueued, 0);
        lock (_statusGate)
        {
            if (_pendingStatusText != null &&
                Interlocked.Exchange(ref _statusApplyQueued, 1) == 0)
            {
                Application.Current?.Dispatcher.BeginInvoke(DispatcherPriority.Background,
                    new Action(ApplyQueuedStatusText));
            }
        }
    }

    private void ApplyQueuedSnapshot()
    {
        EngineSnapshot? snapshot;
        lock (_snapshotGate)
        {
            snapshot = _pendingSnapshot;
            _pendingSnapshot = null;
        }

        if (snapshot != null)
        {
            ApplySnapshot(snapshot);
        }

        Interlocked.Exchange(ref _snapshotApplyQueued, 0);
        lock (_snapshotGate)
        {
            if (_pendingSnapshot != null &&
                Interlocked.Exchange(ref _snapshotApplyQueued, 1) == 0)
            {
                Application.Current?.Dispatcher.BeginInvoke(DispatcherPriority.Background,
                    new Action(ApplyQueuedSnapshot));
            }
        }
    }

    private void ApplySnapshot(EngineSnapshot snapshot)
    {
        Positions.Update(snapshot.Markets, snapshot.State);
        WorkingOrders.Update(snapshot.State, snapshot.Markets);
        Metrics.Update(snapshot.Metrics, snapshot.Gateway, snapshot.State, snapshot.Latency);
        ControlPanel.UpdateFromState(snapshot.State);

        // Feed working orders to open ladder windows
        if (snapshot.State?.WorkingOrders != null && _ladderViewModels.Count > 0)
        {
            foreach (var vm in _ladderViewModels.Values)
                vm.UpdateWorkingOrders(snapshot.State.WorkingOrders);
        }

        if (snapshot.Account != null)
        {
            ActiveAccountName = snapshot.Account.Name;
            ActiveAccountAddress = snapshot.Account.Address;
        }

        if (snapshot.Balance is { Available: true })
        {
            PositionValue = $"{snapshot.Balance.PositionValue:F2}";
        }
        else if (snapshot.Balance != null && !string.IsNullOrEmpty(snapshot.Balance.Error))
        {
            PositionValue = snapshot.Balance.Error;
        }

        if (snapshot.Balance != null)
        {
            UsdcBalance = $"${snapshot.Balance.UsdcBalance:F2}";
            var pnl = snapshot.Balance.RealizedPnl;
            RealizedPnl = $"${pnl:F2}";
            PnlColor = pnl > 0.005 ? "#2E7D32" : pnl < -0.005 ? "#D32F2F" : "Black";
            PolBalance = $"{snapshot.Balance.PolBalance:F2} POL";
        }

        if (snapshot.Rotation != null && snapshot.Rotation.WindowEnd > 0)
        {
            RotationActive = true;
            RotationMarket = snapshot.Rotation.Market.Length > 12
                ? snapshot.Rotation.Market[..12] + "..."
                : snapshot.Rotation.Market;
            RotationCount = snapshot.Rotation.Rotations;
            IsInNoTradeZone = snapshot.Rotation.InNoTrade;

            var nowUnix = DateTimeOffset.UtcNow.ToUnixTimeSeconds();
            var remaining = snapshot.Rotation.WindowEnd - nowUnix;
            if (remaining < 0) remaining = 0;
            RotationTimeRemaining = $"{remaining / 60}:{remaining % 60:D2}";
        }
        else
        {
            RotationActive = false;
        }
    }

    public void Dispose()
    {
        // Close all ladder windows
        foreach (var window in _ladderWindows.Values.ToList())
        {
            window.Close();
        }
        _ladderWindows.Clear();
        _ladderViewModels.Clear();

        UnwireEvents(_connection);
        _connection.Dispose();
        _engineProcess.Dispose();
    }
}
