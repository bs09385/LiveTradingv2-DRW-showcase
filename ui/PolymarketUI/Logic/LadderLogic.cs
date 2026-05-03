namespace PolymarketUI.Logic;

/// <summary>
/// Pure, testable logic for DOM ladder display.
/// No WPF dependencies. All prices are 10000x fixed-point (0.52 = 5200).
/// </summary>
public static class LadderLogic
{
    public const int PriceMax = 10000;
    public const int DefaultTickSize = 100; // 0.01

    public enum CellColor { White, Blue, Red, Green }

    /// <summary>
    /// Whether a price is strictly between bestBid and bestAsk (the spread zone).
    /// </summary>
    public static bool IsInSpread(int price, int bestBid, int bestAsk)
    {
        return bestBid >= 0 && bestAsk <= PriceMax && price > bestBid && price < bestAsk;
    }

    /// <summary>
    /// Bid cell (left column): BLUE if price is at/below bestBid, else WHITE.
    /// </summary>
    public static CellColor GetBidCellColor(int price, int bestBid)
    {
        if (bestBid < 0) return CellColor.White;
        return price <= bestBid ? CellColor.Blue : CellColor.White;
    }

    /// <summary>
    /// Ask cell (right column): RED if price is at/above bestAsk, else WHITE.
    /// </summary>
    public static CellColor GetAskCellColor(int price, int bestAsk)
    {
        if (bestAsk > PriceMax) return CellColor.White;
        return price >= bestAsk ? CellColor.Red : CellColor.White;
    }

    /// <summary>
    /// Price cell (center column): GREEN only if strictly between bestBid and bestAsk.
    /// </summary>
    public static CellColor GetPriceCellColor(int price, int bestBid, int bestAsk)
    {
        if (bestBid < 0 || bestAsk > PriceMax) return CellColor.White;
        if (price > bestBid && price < bestAsk) return CellColor.Green;
        return CellColor.White;
    }

    /// <summary>
    /// Price text should flash yellow if the price traded since the last UI tick.
    /// </summary>
    public static bool ShouldFlashPrice(int price, HashSet<int>? tradedPrices)
    {
        return tradedPrices != null && tradedPrices.Contains(price);
    }

    /// <summary>
    /// Format a 10000x price for display based on tick size.
    /// tick=100 (0.01) -> 2dp. tick<=10 (0.001) -> 3dp.
    /// </summary>
    public static string FormatPrice(int price10k, int tickSize)
    {
        double val = price10k / 10000.0;
        if (tickSize <= 10) return val.ToString("F3");
        return val.ToString("F2");
    }

    /// <summary>
    /// Snap a raw price to the nearest tick-aligned grid price.
    /// E.g., price=5250, tick=100 -> 5200 (floor to tick boundary).
    /// </summary>
    public static int SnapToTick(int price, int tickSize)
    {
        if (tickSize <= 0) return price;
        return (price / tickSize) * tickSize;
    }

    /// <summary>
    /// Remap scroll offset when tick size changes. Preserves the price at the
    /// top of the visible window.
    /// </summary>
    public static int RemapScrollOffset(int oldOffset, int oldTickSize, int newTickSize)
    {
        if (newTickSize <= 0) return oldOffset;
        int topPrice = PriceMax - oldOffset * oldTickSize;
        topPrice = Math.Clamp(topPrice, 0, PriceMax);
        int newOffset = (PriceMax - topPrice) / newTickSize;
        return Math.Max(0, newOffset);
    }

    /// <summary>
    /// Maximum valid scroll offset for a given tick size and visible row count.
    /// </summary>
    public static int MaxScrollOffset(int tickSize, int visibleRows)
    {
        if (tickSize <= 0) return 0;
        int totalLevels = PriceMax / tickSize + 1;
        return Math.Max(0, totalLevels - visibleRows);
    }

    /// <summary>
    /// The price at a given grid index (0 = top = PriceMax, descending).
    /// </summary>
    public static int PriceAtGridIndex(int index, int tickSize)
    {
        return PriceMax - index * tickSize;
    }

    /// <summary>
    /// Compute the scroll offset that centers a given price in the visible window.
    /// </summary>
    public static int ScrollOffsetForCenter(int centerPrice, int tickSize, int visibleRows)
    {
        if (tickSize <= 0) return 0;
        int centerIndex = (PriceMax - centerPrice) / tickSize;
        int offset = centerIndex - visibleRows / 2;
        return Math.Clamp(offset, 0, MaxScrollOffset(tickSize, visibleRows));
    }
}
