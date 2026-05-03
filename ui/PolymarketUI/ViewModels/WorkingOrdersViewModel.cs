using System.Collections.ObjectModel;
using System.Collections.Generic;
using PolymarketUI.Models;

namespace PolymarketUI.ViewModels;

public class WorkingOrdersViewModel : ViewModelBase
{
    public ObservableCollection<WorkingOrderModel> WorkingOrders { get; } = new();
    public ObservableCollection<WorkingOrderModel> ClosedOrders { get; } = new();
    public ObservableCollection<TradeHistoryModel> TradeHistory { get; } = new();

    public void Update(StateSnapshot? state, IReadOnlyList<MarketSnapshot>? markets = null)
    {
        if (state == null)
        {
            WorkingOrders.Clear();
            ClosedOrders.Clear();
            TradeHistory.Clear();
            return;
        }
        var marketMap = BuildMarketMap(markets);

        var working = new List<WorkingOrderModel>(state.WorkingOrders.Count);
        foreach (var order in state.WorkingOrders)
        {
            order.MarketLabel = ResolveMarketLabel(order.MarketLabel, order.MarketId, order.AssetId, marketMap);
            working.Add(order);
        }

        var closed = new List<WorkingOrderModel>(state.ClosedOrders.Count);
        foreach (var order in state.ClosedOrders)
        {
            order.MarketLabel = ResolveMarketLabel(order.MarketLabel, order.MarketId, order.AssetId, marketMap);
            closed.Add(order);
        }

        var trades = new List<TradeHistoryModel>(state.Trades.Count);
        foreach (var trade in state.Trades)
        {
            trade.MarketLabel = ResolveMarketLabel(trade.MarketLabel, trade.MarketId, trade.AssetId, marketMap);
            trades.Add(trade);
        }

        SyncCollection(WorkingOrders, working);
        SyncCollection(ClosedOrders, closed);
        SyncCollection(TradeHistory, trades);
    }

    private readonly record struct MarketLabelInfo(string SeriesLabel, string TokenIdUp, string TokenIdDown);

    private static Dictionary<string, MarketLabelInfo> BuildMarketMap(IReadOnlyList<MarketSnapshot>? markets)
    {
        var map = new Dictionary<string, MarketLabelInfo>(StringComparer.Ordinal);
        if (markets == null) return map;

        foreach (var market in markets)
        {
            if (string.IsNullOrWhiteSpace(market.ConditionId)) continue;
            map[market.ConditionId] = new MarketLabelInfo(
                market.SeriesLabel ?? string.Empty,
                market.TokenIdUp ?? string.Empty,
                market.TokenIdDown ?? string.Empty);
        }

        return map;
    }

    private static string ResolveMarketLabel(string existing,
                                             string marketId,
                                             string assetId,
                                             IReadOnlyDictionary<string, MarketLabelInfo> marketMap)
    {
        if (!string.IsNullOrWhiteSpace(existing)) return existing;

        if (!string.IsNullOrWhiteSpace(marketId) && marketMap.TryGetValue(marketId, out var info))
        {
            var prefix = string.IsNullOrWhiteSpace(info.SeriesLabel) ? "MARKET" : info.SeriesLabel;
            if (!string.IsNullOrWhiteSpace(assetId))
            {
                if (assetId == info.TokenIdUp) return $"{prefix} UP";
                if (assetId == info.TokenIdDown) return $"{prefix} DOWN";
            }
            return prefix;
        }

        return "MARKET";
    }

    private static void SyncCollection<T>(ObservableCollection<T> target, IReadOnlyList<T> source)
    {
        while (target.Count > source.Count)
            target.RemoveAt(target.Count - 1);

        for (int i = 0; i < source.Count; i++)
        {
            if (i < target.Count)
                target[i] = source[i];
            else
                target.Add(source[i]);
        }
    }
}
