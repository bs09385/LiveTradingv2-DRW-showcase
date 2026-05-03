using System;
using System.Collections.ObjectModel;
using System.Windows.Input;
using PolymarketUI.Models;

namespace PolymarketUI.ViewModels;

public class SeriesItem : ViewModelBase
{
    public string SeriesKey { get; }
    public string DisplayName { get; }

    private string _status = "DISCONNECTED";
    public string Status { get => _status; set => SetField(ref _status, value); }

    private string _conditionId = "";
    public string ConditionId { get => _conditionId; set => SetField(ref _conditionId, value); }

    private bool _isLadderOpen;
    public bool IsLadderOpen
    {
        get => _isLadderOpen;
        set
        {
            if (!SetField(ref _isLadderOpen, value)) return;
            OnPropertyChanged(nameof(ToggleButtonText));
        }
    }

    public string ToggleButtonText => _isLadderOpen ? "Close" : "Open";

    public ICommand ToggleLadderCommand { get; }

    private readonly Action<string>? _toggleCallback;

    public SeriesItem(string seriesKey, string displayName, Action<string>? toggleCallback = null)
    {
        SeriesKey = seriesKey;
        DisplayName = displayName;
        _toggleCallback = toggleCallback;
        ToggleLadderCommand = new RelayCommand(() => _toggleCallback?.Invoke(seriesKey));
    }
}

public class MarketPickerViewModel : ViewModelBase
{
    public ObservableCollection<SeriesItem> Series { get; }

    public MarketPickerViewModel(Action<string>? toggleLadderCallback = null)
    {
        Series = new ObservableCollection<SeriesItem>
        {
            new SeriesItem("BTC_5m", "5m", toggleLadderCallback),
            new SeriesItem("BTC_15m", "15m", toggleLadderCallback),
        };
    }

    public void OnSeriesListReceived(SeriesListMessage msg)
    {
        foreach (var info in msg.Series)
        {
            var item = FindByKey(info.SeriesKey);
            if (item != null)
            {
                item.Status = info.Status;
                item.ConditionId = info.ConditionId;
            }
        }
    }

    public void OnWatcherStatusReceived(WatcherStatusMessage msg)
    {
        var item = FindByKey(msg.SeriesKey);
        if (item != null)
        {
            item.Status = msg.Status;
        }
    }

    public SeriesItem? FindByKey(string key)
    {
        foreach (var item in Series)
        {
            if (item.SeriesKey == key)
                return item;
        }
        return null;
    }
}
