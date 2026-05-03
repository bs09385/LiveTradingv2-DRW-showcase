using System.Collections.ObjectModel;
using System.Windows.Media;
using System.Windows.Threading;
using PolymarketUI.Logic;
using PolymarketUI.Models;
using static PolymarketUI.Models.QtyConvert;

namespace PolymarketUI.ViewModels;

public class LadderRow : ViewModelBase
{
    private string _priceStr = "";
    public string PriceStr { get => _priceStr; set => SetField(ref _priceStr, value); }

    private int _rawPrice;
    public int RawPrice { get => _rawPrice; set => SetField(ref _rawPrice, value); }

    private string _buySizeStr = "";
    public string BuySizeStr { get => _buySizeStr; set => SetField(ref _buySizeStr, value); }

    private string _sellSizeStr = "";
    public string SellSizeStr { get => _sellSizeStr; set => SetField(ref _sellSizeStr, value); }

    private Brush _bidBrush = LadderBrushes.White;
    public Brush BidBrush { get => _bidBrush; set => SetField(ref _bidBrush, value); }

    private Brush _askBrush = LadderBrushes.White;
    public Brush AskBrush { get => _askBrush; set => SetField(ref _askBrush, value); }

    private Brush _priceBrush = LadderBrushes.Transparent;
    public Brush PriceBrush { get => _priceBrush; set => SetField(ref _priceBrush, value); }

    private Brush _priceForeground = LadderBrushes.Black;
    public Brush PriceForeground { get => _priceForeground; set => SetField(ref _priceForeground, value); }

    private string _bidOrderStr = "";
    public string BidOrderStr { get => _bidOrderStr; set => SetField(ref _bidOrderStr, value); }

    private string _askOrderStr = "";
    public string AskOrderStr { get => _askOrderStr; set => SetField(ref _askOrderStr, value); }

    private Brush _bidOrderBrush = LadderBrushes.Transparent;
    public Brush BidOrderBrush { get => _bidOrderBrush; set => SetField(ref _bidOrderBrush, value); }

    private Brush _askOrderBrush = LadderBrushes.Transparent;
    public Brush AskOrderBrush { get => _askOrderBrush; set => SetField(ref _askOrderBrush, value); }
}

public static class LadderBrushes
{
    public static readonly Brush BlueBid = Freeze(new SolidColorBrush(Color.FromRgb(0x55, 0x88, 0xBB)));
    public static readonly Brush RedAsk = Freeze(new SolidColorBrush(Color.FromRgb(0xBB, 0x55, 0x55)));
    public static readonly Brush GreenSpread = Freeze(new SolidColorBrush(Color.FromRgb(0xBB, 0xEE, 0xBB)));
    public static readonly Brush White = Freeze(new SolidColorBrush(Colors.White));
    public static readonly Brush Transparent = Freeze(new SolidColorBrush(Colors.Transparent));
    public static readonly Brush YellowFlash = Freeze(new SolidColorBrush(Colors.Yellow));
    public static readonly Brush Black = Freeze(new SolidColorBrush(Colors.Black));
    public static readonly Brush Orange = Freeze(new SolidColorBrush(Color.FromRgb(0xFF, 0xA5, 0x00)));

    // Faint tints for zero-volume levels in the bid/ask zone (~15% base color over white)
    public static readonly Brush BlueBidFaint = Freeze(new SolidColorBrush(Color.FromRgb(0xE6, 0xED, 0xF5)));
    public static readonly Brush RedAskFaint = Freeze(new SolidColorBrush(Color.FromRgb(0xF5, 0xE6, 0xE6)));

    // Raw colors for gradient stop construction
    public static readonly Color BlueBidColor = Color.FromRgb(0x55, 0x88, 0xBB);
    public static readonly Color BlueFaintColor = Color.FromRgb(0xE6, 0xED, 0xF5);
    public static readonly Color RedAskColor = Color.FromRgb(0xBB, 0x55, 0x55);
    public static readonly Color RedFaintColor = Color.FromRgb(0xF5, 0xE6, 0xE6);

    private static Brush Freeze(Brush b) { b.Freeze(); return b; }
}

public static class VolumeBarBrushCache
{
    private static readonly Dictionary<int, Brush> _bidCache = new();
    private static readonly Dictionary<int, Brush> _askCache = new();

    public static Brush GetBidBrush(double percent)
    {
        int bucket = (int)Math.Round(percent * 50.0);
        bucket = Math.Clamp(bucket, 0, 50);

        if (bucket == 0) return LadderBrushes.BlueBidFaint;
        if (bucket == 50) return LadderBrushes.BlueBid;

        if (_bidCache.TryGetValue(bucket, out var cached))
            return cached;

        double stop = bucket / 50.0;
        var brush = new LinearGradientBrush
        {
            StartPoint = new System.Windows.Point(1, 0.5),
            EndPoint = new System.Windows.Point(0, 0.5),
        };
        brush.GradientStops.Add(new GradientStop(LadderBrushes.BlueBidColor, 0.0));
        brush.GradientStops.Add(new GradientStop(LadderBrushes.BlueBidColor, stop));
        brush.GradientStops.Add(new GradientStop(LadderBrushes.BlueFaintColor, stop));
        brush.GradientStops.Add(new GradientStop(LadderBrushes.BlueFaintColor, 1.0));
        brush.Freeze();

        _bidCache[bucket] = brush;
        return brush;
    }

    public static Brush GetAskBrush(double percent)
    {
        int bucket = (int)Math.Round(percent * 50.0);
        bucket = Math.Clamp(bucket, 0, 50);

        if (bucket == 0) return LadderBrushes.RedAskFaint;
        if (bucket == 50) return LadderBrushes.RedAsk;

        if (_askCache.TryGetValue(bucket, out var cached))
            return cached;

        double stop = bucket / 50.0;
        var brush = new LinearGradientBrush
        {
            StartPoint = new System.Windows.Point(0, 0.5),
            EndPoint = new System.Windows.Point(1, 0.5),
        };
        brush.GradientStops.Add(new GradientStop(LadderBrushes.RedAskColor, 0.0));
        brush.GradientStops.Add(new GradientStop(LadderBrushes.RedAskColor, stop));
        brush.GradientStops.Add(new GradientStop(LadderBrushes.RedFaintColor, stop));
        brush.GradientStops.Add(new GradientStop(LadderBrushes.RedFaintColor, 1.0));
        brush.Freeze();

        _askCache[bucket] = brush;
        return brush;
    }
}

public class LadderViewModel : ViewModelBase
{
    public const int RowHeight = 26;
    private const int DefaultVisibleRows = 30;
    private int _visibleRows = DefaultVisibleRows;
    private const int UpdateIntervalMs = 100;

    public string SeriesKey { get; }

    private string _conditionId = "";
    public string ConditionId { get => _conditionId; set => SetField(ref _conditionId, value); }

    private string _status = "DISCONNECTED";
    public string Status { get => _status; set => SetField(ref _status, value); }

    private string _lastUpdateText = "Never";
    public string LastUpdateText { get => _lastUpdateText; set => SetField(ref _lastUpdateText, value); }

    private string _tickSizeText = "0.01";
    public string TickSizeText { get => _tickSizeText; set => SetField(ref _tickSizeText, value); }

    private string _levelCountText = "0";
    public string LevelCountText { get => _levelCountText; set => SetField(ref _levelCountText, value); }

    public ObservableCollection<LadderRow> Rows { get; } = new();

    // Internal book state — keyed by price
    private readonly Dictionary<int, long> _bidLevels = new();
    private readonly Dictionary<int, long> _askLevels = new();
    private readonly HashSet<int> _tradedPricesThisTick = new();
    private readonly HashSet<int> _pendingTrades = new();

    private int _tickSize = LadderLogic.DefaultTickSize;
    private int _scrollOffset;
    private int _bestBid = -1;
    private int _bestAsk = LadderLogic.PriceMax + 1;
    private int _msgCount;
    private DateTime _msgCountStart = DateTime.UtcNow;
    private DateTime _lastManualScroll = DateTime.MinValue;
    private const double ManualScrollCooldownSec = 5.0;
    private int _dataVersion;

    // Smoothed max for volume bar scaling: instant rise, slow decay
    private long _smoothedMax;
    private int _lastRenderScrollOffset = -1;
    private const double MaxDecayFactor = 0.97;

    // Our working orders by price
    private readonly Dictionary<int, long> _ourBidOrders = new();
    private readonly Dictionary<int, long> _ourAskOrders = new();

    private WatcherBooksMessage? _pendingData;
    private readonly DispatcherTimer _updateTimer;

    public LadderViewModel(string seriesKey)
    {
        SeriesKey = seriesKey;

        // Pre-create rows
        for (int i = 0; i < _visibleRows; i++)
            Rows.Add(new LadderRow());

        // Default scroll to center on 0.50 (price 5000)
        _scrollOffset = LadderLogic.ScrollOffsetForCenter(5000, _tickSize, _visibleRows);

        // 10ms update timer
        _updateTimer = new DispatcherTimer
        {
            Interval = TimeSpan.FromMilliseconds(UpdateIntervalMs)
        };
        _updateTimer.Tick += (_, _) => OnTick();
        _updateTimer.Start();
    }

    public void OnBooksReceived(WatcherBooksMessage msg)
    {
        if (msg.SeriesKey != SeriesKey) return;

        _pendingData = msg;
        _msgCount++;

        // Accumulate trades from all messages between UI ticks
        if (msg.Trades != null)
        {
            foreach (var t in msg.Trades)
                _pendingTrades.Add(t.Price);
        }
    }

    public void OnStatusReceived(WatcherStatusMessage msg)
    {
        if (msg.SeriesKey != SeriesKey) return;
        Status = msg.Status;
    }

    public void UpdateWorkingOrders(IReadOnlyList<WorkingOrderModel> orders)
    {
        _ourBidOrders.Clear();
        _ourAskOrders.Clear();

        var cid = _conditionId;
        if (string.IsNullOrEmpty(cid)) return;

        foreach (var o in orders)
        {
            if (o.MarketId != cid) continue;
            var remaining = o.OriginalSize - o.FilledSize;
            if (remaining <= 0) continue;

            var dict = o.Side == "BID" ? _ourBidOrders : _ourAskOrders;
            dict.TryGetValue(o.Price, out var existing);
            dict[o.Price] = existing + remaining;
        }
    }

    /// <summary>
    /// Adjust scroll offset. Positive delta = scroll down (see lower prices).
    /// </summary>
    public void Scroll(int delta)
    {
        int maxOffset = LadderLogic.MaxScrollOffset(_tickSize, _visibleRows);
        _scrollOffset = Math.Clamp(_scrollOffset + delta, 0, maxOffset);
        _lastManualScroll = DateTime.UtcNow;
    }

    public void SetVisibleRows(int count)
    {
        count = Math.Max(5, count);
        if (count == _visibleRows) return;
        _visibleRows = count;

        while (Rows.Count < _visibleRows)
            Rows.Add(new LadderRow());
        while (Rows.Count > _visibleRows)
            Rows.RemoveAt(Rows.Count - 1);
    }

    private void OnTick()
    {
        // 1) Drain pending data
        if (_pendingData != null)
        {
            ApplyBookData(_pendingData);
            _pendingData = null;
        }

        // 2) Move pending trades into this tick's set
        _tradedPricesThisTick.Clear();
        if (_pendingTrades.Count > 0)
        {
            foreach (var p in _pendingTrades)
                _tradedPricesThisTick.Add(p);
            _pendingTrades.Clear();
        }

        // 3) Compute BBO
        ComputeBbo();

        // 4) Render 30 visible rows
        RenderRows();
    }

    private void ApplyBookData(WatcherBooksMessage data)
    {
        // Handle tick size change
        if (data.TickSize > 0 && data.TickSize != _tickSize)
        {
            HandleTickSizeChange(data.TickSize);
        }

        // Replace full book state (C++ sends complete merged ladder each time).
        _bidLevels.Clear();
        foreach (var l in data.BuyLevels)
            _bidLevels[l.Price] = l.Size;

        _askLevels.Clear();
        foreach (var l in data.SellLevels)
            _askLevels[l.Price] = l.Size;

        _dataVersion++;

        // Continuous BBO following — recenter if BBO drifts outside visible window.
        // Pauses for 5s after manual scroll to allow user exploration.
        if ((_bidLevels.Count > 0 || _askLevels.Count > 0) && ShouldAutoFollow())
        {
            AutoScrollToBbo();
        }

        ConditionId = data.ConditionId;
        LastUpdateText = DateTime.Now.ToString("HH:mm:ss.f");

        // Compute message rate
        var elapsed = (DateTime.UtcNow - _msgCountStart).TotalSeconds;
        string rateStr = elapsed > 1.0 ? $"{_msgCount / elapsed:F1}/s" : $"{_msgCount}";
        if (elapsed > 5.0) { _msgCount = 0; _msgCountStart = DateTime.UtcNow; }

        // Diagnostic: show top bid/ask with sizes, visible hit count, data version
        int topBidP = -1; long topBidS = 0;
        int topAskP = LadderLogic.PriceMax + 1; long topAskS = 0;
        foreach (var kv in _bidLevels)
            if (kv.Value > 0 && kv.Key > topBidP) { topBidP = kv.Key; topBidS = kv.Value; }
        foreach (var kv in _askLevels)
            if (kv.Value > 0 && kv.Key < topAskP) { topAskP = kv.Key; topAskS = kv.Value; }

        string bbStr = topBidP >= 0 ? $"{LadderLogic.FormatPrice(topBidP, _tickSize)}@{Format(topBidS)}" : "-";
        string baStr = topAskP <= LadderLogic.PriceMax ? $"{LadderLogic.FormatPrice(topAskP, _tickSize)}@{Format(topAskS)}" : "-";

        int visHits = 0;
        for (int i = 0; i < _visibleRows; i++)
        {
            int p = LadderLogic.PriceAtGridIndex(_scrollOffset + i, _tickSize);
            if (_bidLevels.ContainsKey(p) || _askLevels.ContainsKey(p)) visHits++;
        }

        LevelCountText = $"{data.BuyLevels.Count}b/{data.SellLevels.Count}a | BB:{bbStr} BA:{baStr} | Vis:{visHits} | v{_dataVersion} | Rx:{rateStr}";
    }

    private bool ShouldAutoFollow()
    {
        return (DateTime.UtcNow - _lastManualScroll).TotalSeconds > ManualScrollCooldownSec;
    }

    private string FormatBbo()
    {
        // Quick BBO scan for display
        int bb = -1, ba = LadderLogic.PriceMax + 1;
        foreach (var kv in _bidLevels)
            if (kv.Value > 0 && kv.Key > bb) bb = kv.Key;
        foreach (var kv in _askLevels)
            if (kv.Value > 0 && kv.Key < ba) ba = kv.Key;

        string bidStr = bb >= 0 ? LadderLogic.FormatPrice(bb, _tickSize) : "-";
        string askStr = ba <= LadderLogic.PriceMax ? LadderLogic.FormatPrice(ba, _tickSize) : "-";
        return $"{bidStr}/{askStr}";
    }

    private void AutoScrollToBbo()
    {
        // Find best bid and best ask from current book
        int bestBid = -1;
        int bestAsk = LadderLogic.PriceMax + 1;

        foreach (var kv in _bidLevels)
            if (kv.Value > 0 && kv.Key > bestBid) bestBid = kv.Key;
        foreach (var kv in _askLevels)
            if (kv.Value > 0 && kv.Key < bestAsk) bestAsk = kv.Key;

        // Center on midpoint of BBO, or whichever side exists
        int centerPrice;
        if (bestBid >= 0 && bestAsk <= LadderLogic.PriceMax)
            centerPrice = (bestBid + bestAsk) / 2;
        else if (bestBid >= 0)
            centerPrice = bestBid;
        else if (bestAsk <= LadderLogic.PriceMax)
            centerPrice = bestAsk;
        else
            return; // no levels at all

        // Only recenter if BBO is outside the visible window (avoid jitter)
        int topPrice = LadderLogic.PriceAtGridIndex(_scrollOffset, _tickSize);
        int bottomPrice = LadderLogic.PriceAtGridIndex(_scrollOffset + _visibleRows - 1, _tickSize);
        if (centerPrice <= topPrice && centerPrice >= bottomPrice)
            return; // BBO still within view

        _scrollOffset = LadderLogic.ScrollOffsetForCenter(centerPrice, _tickSize, _visibleRows);
    }

    private void HandleTickSizeChange(int newTickSize)
    {
        int oldTickSize = _tickSize;
        _tickSize = newTickSize;
        _scrollOffset = LadderLogic.RemapScrollOffset(_scrollOffset, oldTickSize, newTickSize);

        // Clamp to valid range
        int maxOffset = LadderLogic.MaxScrollOffset(_tickSize, _visibleRows);
        _scrollOffset = Math.Clamp(_scrollOffset, 0, maxOffset);

        // Update display
        TickSizeText = LadderLogic.FormatPrice(_tickSize, _tickSize);
    }

    private void ComputeBbo()
    {
        _bestBid = -1;
        _bestAsk = LadderLogic.PriceMax + 1;

        foreach (var kv in _bidLevels)
        {
            if (kv.Value > 0 && kv.Key > _bestBid)
                _bestBid = kv.Key;
        }

        foreach (var kv in _askLevels)
        {
            if (kv.Value > 0 && kv.Key < _bestAsk)
                _bestAsk = kv.Key;
        }
    }

    private void RenderRows()
    {
        // Pass 1: find max volume across visible bid/ask levels (shared max for imbalance visibility)
        long currentMax = 0;
        for (int i = 0; i < _visibleRows; i++)
        {
            int price = LadderLogic.PriceAtGridIndex(_scrollOffset + i, _tickSize);
            if (price < 0 || price > LadderLogic.PriceMax) continue;

            if (price <= _bestBid && _bestBid >= 0)
            {
                _bidLevels.TryGetValue(price, out var bv);
                if (bv > currentMax) currentMax = bv;
            }
            if (price >= _bestAsk && _bestAsk <= LadderLogic.PriceMax)
            {
                _askLevels.TryGetValue(price, out var av);
                if (av > currentMax) currentMax = av;
            }
        }

        // Smoothed max: instant rise, slow decay. Reset on scroll.
        if (_scrollOffset != _lastRenderScrollOffset)
        {
            _smoothedMax = currentMax;
            _lastRenderScrollOffset = _scrollOffset;
        }
        else if (currentMax > _smoothedMax)
        {
            _smoothedMax = currentMax;
        }
        else
        {
            _smoothedMax = Math.Max(currentMax, (long)(_smoothedMax * MaxDecayFactor));
        }

        long maxVol = _smoothedMax;

        // Pass 2: render each row with volume bar brushes
        for (int i = 0; i < _visibleRows; i++)
        {
            int gridIndex = _scrollOffset + i;
            int price = LadderLogic.PriceAtGridIndex(gridIndex, _tickSize);
            var row = Rows[i];

            if (price < 0 || price > LadderLogic.PriceMax)
            {
                row.RawPrice = -1;
                row.PriceStr = "";
                row.BuySizeStr = "";
                row.SellSizeStr = "";
                row.BidOrderStr = "";
                row.AskOrderStr = "";
                row.BidBrush = LadderBrushes.White;
                row.AskBrush = LadderBrushes.White;
                row.BidOrderBrush = LadderBrushes.Transparent;
                row.AskOrderBrush = LadderBrushes.Transparent;
                row.PriceBrush = LadderBrushes.Transparent;
                row.PriceForeground = LadderBrushes.Black;
                continue;
            }

            row.RawPrice = price;
            row.PriceStr = LadderLogic.FormatPrice(price, _tickSize);

            // Bid volume
            _bidLevels.TryGetValue(price, out var bidSize);
            row.BuySizeStr = bidSize > 0 ? Format(bidSize) : "";

            // Ask volume
            _askLevels.TryGetValue(price, out var askSize);
            row.SellSizeStr = askSize > 0 ? Format(askSize) : "";

            // Our working orders
            _ourBidOrders.TryGetValue(price, out var ourBidSize);
            row.BidOrderStr = ourBidSize > 0 ? Format(ourBidSize) : "";
            row.BidOrderBrush = ourBidSize > 0 ? LadderBrushes.Orange : LadderBrushes.Transparent;

            _ourAskOrders.TryGetValue(price, out var ourAskSize);
            row.AskOrderStr = ourAskSize > 0 ? Format(ourAskSize) : "";
            row.AskOrderBrush = ourAskSize > 0 ? LadderBrushes.Orange : LadderBrushes.Transparent;

            // Colors — spread levels get green on ALL three columns
            bool inSpread = LadderLogic.IsInSpread(price, _bestBid, _bestAsk);
            if (inSpread)
            {
                row.BidBrush = LadderBrushes.GreenSpread;
                row.PriceBrush = LadderBrushes.GreenSpread;
                row.AskBrush = LadderBrushes.GreenSpread;
            }
            else
            {
                // Bid cell: volume bar in the bid zone, white outside
                if (price <= _bestBid && _bestBid >= 0)
                {
                    double pct = maxVol > 0 ? (double)bidSize / maxVol : 0.0;
                    row.BidBrush = VolumeBarBrushCache.GetBidBrush(pct);
                }
                else
                {
                    row.BidBrush = LadderBrushes.White;
                }

                row.PriceBrush = LadderBrushes.Transparent;

                // Ask cell: volume bar in the ask zone, white outside
                if (price >= _bestAsk && _bestAsk <= LadderLogic.PriceMax)
                {
                    double pct = maxVol > 0 ? (double)askSize / maxVol : 0.0;
                    row.AskBrush = VolumeBarBrushCache.GetAskBrush(pct);
                }
                else
                {
                    row.AskBrush = LadderBrushes.White;
                }
            }

            // Trade flash
            bool flash = LadderLogic.ShouldFlashPrice(price, _tradedPricesThisTick);
            row.PriceForeground = flash ? LadderBrushes.YellowFlash : LadderBrushes.Black;
        }
    }

    public void Dispose()
    {
        _updateTimer.Stop();
    }
}
