using System.Collections.ObjectModel;
using System.Collections.Generic;
using PolymarketUI.Models;

namespace PolymarketUI.ViewModels;

public class PositionDisplay
{
    public string ConditionId { get; set; } = "";
    public long PositionUp { get; set; }
    public long PositionDown { get; set; }
    public long NetExposure => PositionUp - PositionDown;
    public string PositionUpStr => QtyConvert.Format(PositionUp);
    public string PositionDownStr => QtyConvert.Format(PositionDown);
    public string NetExposureStr => QtyConvert.Format(NetExposure);
}

public class PositionsViewModel : ViewModelBase
{
    public ObservableCollection<PositionDisplay> Positions { get; } = new();

    public void Update(List<MarketSnapshot> markets, StateSnapshot? state = null)
    {
        var tokenPositions = new Dictionary<string, long>(StringComparer.Ordinal);
        if (state != null)
        {
            foreach (var pos in state.Positions)
            {
                if (!string.IsNullOrWhiteSpace(pos.TokenId))
                {
                    tokenPositions[pos.TokenId] = pos.Position;
                }
            }
        }

        if (markets.Count > 0)
        {
            while (Positions.Count > markets.Count)
                Positions.RemoveAt(Positions.Count - 1);

            for (int i = 0; i < markets.Count; i++)
            {
                var market = markets[i];
                var up = market.PositionUp;
                var down = market.PositionDown;

                if (tokenPositions.TryGetValue(market.TokenIdUp, out var upFromState))
                    up = upFromState;
                if (tokenPositions.TryGetValue(market.TokenIdDown, out var downFromState))
                    down = downFromState;

                var label = !string.IsNullOrWhiteSpace(market.SeriesLabel)
                    ? market.SeriesLabel
                    : Truncate(market.ConditionId, 16);

                var display = new PositionDisplay
                {
                    ConditionId = label,
                    PositionUp = up,
                    PositionDown = down
                };

                if (i < Positions.Count)
                    Positions[i] = display;
                else
                    Positions.Add(display);
            }
            return;
        }

        // Fallback: if no market metadata yet, still show token-level positions.
        var fallbackCount = state?.Positions.Count ?? 0;
        while (Positions.Count > fallbackCount)
            Positions.RemoveAt(Positions.Count - 1);

        if (state == null) return;

        for (int i = 0; i < state.Positions.Count; i++)
        {
            var token = state.Positions[i];
            var display = new PositionDisplay
            {
                ConditionId = $"Token {Truncate(token.TokenId, 12)}",
                PositionUp = token.Position,
                PositionDown = 0
            };

            if (i < Positions.Count)
                Positions[i] = display;
            else
                Positions.Add(display);
        }
    }

    private static string Truncate(string s, int maxLen)
    {
        if (s.Length <= maxLen) return s;
        return s[..maxLen] + "...";
    }
}
