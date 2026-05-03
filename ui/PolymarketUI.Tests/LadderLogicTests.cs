using PolymarketUI.Logic;
using Xunit;
using static PolymarketUI.Logic.LadderLogic;

namespace PolymarketUI.Tests;

public class ColorMappingTests
{
    [Theory]
    [InlineData(5000, 5200, CellColor.Blue)]   // below bestBid
    [InlineData(5200, 5200, CellColor.Blue)]   // at bestBid
    [InlineData(5300, 5200, CellColor.White)]  // above bestBid
    [InlineData(5000, -1, CellColor.White)]    // no bestBid
    public void BidCellColor(int price, int bestBid, CellColor expected)
    {
        Assert.Equal(expected, GetBidCellColor(price, bestBid));
    }

    [Theory]
    [InlineData(5500, 5300, CellColor.Red)]    // above bestAsk
    [InlineData(5300, 5300, CellColor.Red)]    // at bestAsk
    [InlineData(5200, 5300, CellColor.White)]  // below bestAsk
    [InlineData(5000, 10001, CellColor.White)] // no bestAsk
    public void AskCellColor(int price, int bestAsk, CellColor expected)
    {
        Assert.Equal(expected, GetAskCellColor(price, bestAsk));
    }

    [Theory]
    [InlineData(5250, 5200, 5300, CellColor.Green)]  // strictly between
    [InlineData(5200, 5200, 5300, CellColor.White)]   // at bestBid (not green)
    [InlineData(5300, 5200, 5300, CellColor.White)]   // at bestAsk (not green)
    [InlineData(5100, 5200, 5300, CellColor.White)]   // below bestBid
    [InlineData(5400, 5200, 5300, CellColor.White)]   // above bestAsk
    [InlineData(5250, -1, 5300, CellColor.White)]     // no bestBid
    [InlineData(5250, 5200, 10001, CellColor.White)]  // no bestAsk
    public void PriceCellColor(int price, int bestBid, int bestAsk, CellColor expected)
    {
        Assert.Equal(expected, GetPriceCellColor(price, bestBid, bestAsk));
    }

    [Fact]
    public void GreenOnlyStrictlyBetween_MultipleLevels()
    {
        // bestBid=5200, bestAsk=5400
        // Level 5200 (bestBid) -> NOT green
        // Level 5300 (between) -> GREEN
        // Level 5400 (bestAsk) -> NOT green
        Assert.Equal(CellColor.White, GetPriceCellColor(5200, 5200, 5400));
        Assert.Equal(CellColor.Green, GetPriceCellColor(5300, 5200, 5400));
        Assert.Equal(CellColor.White, GetPriceCellColor(5400, 5200, 5400));
    }

    [Fact]
    public void AdjacentBidAsk_NoGreen()
    {
        // bestBid=5200, bestAsk=5300 (adjacent, one tick apart with tick=100)
        // No price is strictly between 5200 and 5300 on a 100-tick grid
        Assert.Equal(CellColor.White, GetPriceCellColor(5200, 5200, 5300));
        Assert.Equal(CellColor.White, GetPriceCellColor(5300, 5200, 5300));
        // But 5250 would be green if it existed in a finer grid
        Assert.Equal(CellColor.Green, GetPriceCellColor(5250, 5200, 5300));
    }
}

public class SpreadDetectionTests
{
    [Fact]
    public void IsInSpread_StrictlyBetween()
    {
        Assert.True(IsInSpread(5250, 5200, 5300));
        Assert.True(IsInSpread(5201, 5200, 5300));
        Assert.True(IsInSpread(5299, 5200, 5300));
    }

    [Fact]
    public void IsInSpread_AtTouch_NotInSpread()
    {
        // bestBid and bestAsk themselves are NOT in the spread
        Assert.False(IsInSpread(5200, 5200, 5300));
        Assert.False(IsInSpread(5300, 5200, 5300));
    }

    [Fact]
    public void IsInSpread_Outside_NotInSpread()
    {
        Assert.False(IsInSpread(5100, 5200, 5300));
        Assert.False(IsInSpread(5400, 5200, 5300));
    }

    [Fact]
    public void IsInSpread_NoBbo()
    {
        Assert.False(IsInSpread(5250, -1, 5300));
        Assert.False(IsInSpread(5250, 5200, 10001));
    }

    [Fact]
    public void SpreadGreenAppliesToAllColumns()
    {
        // Verify the spread zone logic: for a price in the spread,
        // ALL columns (bid, price, ask) should be green.
        // This tests the IsInSpread function which the ViewModel uses
        // to set green on all three column brushes.
        int bestBid = 5200, bestAsk = 5400;

        // Spread level -> green everywhere
        Assert.True(IsInSpread(5300, bestBid, bestAsk));

        // At bestBid -> NOT spread (bid gets blue, not green)
        Assert.False(IsInSpread(5200, bestBid, bestAsk));
        Assert.Equal(CellColor.Blue, GetBidCellColor(5200, bestBid));

        // At bestAsk -> NOT spread (ask gets red, not green)
        Assert.False(IsInSpread(5400, bestBid, bestAsk));
        Assert.Equal(CellColor.Red, GetAskCellColor(5400, bestAsk));
    }
}

public class FlashLogicTests
{
    [Fact]
    public void FlashForTradedPrice()
    {
        var traded = new HashSet<int> { 5200, 5300 };
        Assert.True(ShouldFlashPrice(5200, traded));
        Assert.True(ShouldFlashPrice(5300, traded));
        Assert.False(ShouldFlashPrice(5100, traded));
    }

    [Fact]
    public void NoFlashWhenEmpty()
    {
        var empty = new HashSet<int>();
        Assert.False(ShouldFlashPrice(5200, empty));
    }

    [Fact]
    public void NoFlashWhenNull()
    {
        Assert.False(ShouldFlashPrice(5200, null));
    }

    [Fact]
    public void FlashClearedBetweenTicks()
    {
        // Simulate: tick 1 has trade at 5200, tick 2 does not
        var tick1Trades = new HashSet<int> { 5200 };
        Assert.True(ShouldFlashPrice(5200, tick1Trades));

        // Next tick: clear the set
        tick1Trades.Clear();
        Assert.False(ShouldFlashPrice(5200, tick1Trades));
    }

    [Fact]
    public void MultiplePricesFlashPerTick()
    {
        var trades = new HashSet<int> { 5100, 5200, 5300 };
        Assert.True(ShouldFlashPrice(5100, trades));
        Assert.True(ShouldFlashPrice(5200, trades));
        Assert.True(ShouldFlashPrice(5300, trades));
        Assert.False(ShouldFlashPrice(5400, trades));
    }
}

public class TickSizeFormattingTests
{
    [Theory]
    [InlineData(5200, 100, "0.52")]     // tick=0.01 -> 2dp
    [InlineData(5000, 100, "0.50")]
    [InlineData(100, 100, "0.01")]
    [InlineData(0, 100, "0.00")]
    [InlineData(10000, 100, "1.00")]
    [InlineData(9900, 100, "0.99")]
    public void FormatWithStandardTick(int price, int tickSize, string expected)
    {
        Assert.Equal(expected, FormatPrice(price, tickSize));
    }

    [Theory]
    [InlineData(9, 10, "0.001")]        // tick=0.001 -> 3dp
    [InlineData(90, 10, "0.009")]
    [InlineData(5200, 10, "0.520")]
    [InlineData(9991, 10, "0.999")]
    [InlineData(10000, 10, "1.000")]
    [InlineData(0, 10, "0.000")]
    public void FormatWithFineTick(int price, int tickSize, string expected)
    {
        Assert.Equal(expected, FormatPrice(price, tickSize));
    }

    [Fact]
    public void FormatNineMilliprice()
    {
        // Explicit check: 0.009 must format correctly
        Assert.Equal("0.009", FormatPrice(90, 10));
    }
}

public class SnapToTickTests
{
    [Theory]
    [InlineData(5200, 100, 5200)]   // already aligned
    [InlineData(5250, 100, 5200)]   // snaps down
    [InlineData(5299, 100, 5200)]   // snaps down
    [InlineData(5300, 100, 5300)]   // already aligned
    [InlineData(0, 100, 0)]         // min
    [InlineData(10000, 100, 10000)] // max
    [InlineData(99, 100, 0)]        // below first tick
    public void SnapToTickStandard(int price, int tickSize, int expected)
    {
        Assert.Equal(expected, SnapToTick(price, tickSize));
    }

    [Theory]
    [InlineData(5200, 10, 5200)]    // already aligned
    [InlineData(5205, 10, 5200)]    // snaps down
    [InlineData(5209, 10, 5200)]    // snaps down
    [InlineData(5210, 10, 5210)]    // already aligned
    public void SnapToTickFine(int price, int tickSize, int expected)
    {
        Assert.Equal(expected, SnapToTick(price, tickSize));
    }

    [Fact]
    public void SnapWithZeroTick_ReturnsPrice()
    {
        Assert.Equal(5250, SnapToTick(5250, 0));
    }
}

public class TickSizeRemappingTests
{
    [Fact]
    public void RemapFromCoarseToFine()
    {
        // Viewing around price 5000, tick=100, offset=50
        // topPrice = 10000 - 50*100 = 5000
        // With tick=10: newOffset = (10000-5000)/10 = 500
        Assert.Equal(500, RemapScrollOffset(50, 100, 10));
    }

    [Fact]
    public void RemapFromFineToCoarse()
    {
        // offset=500, tick=10 -> topPrice = 10000 - 500*10 = 5000
        // With tick=100: newOffset = (10000-5000)/100 = 50
        Assert.Equal(50, RemapScrollOffset(500, 10, 100));
    }

    [Fact]
    public void RemapAtTop()
    {
        // offset=0 -> topPrice=10000 -> newOffset=0
        Assert.Equal(0, RemapScrollOffset(0, 100, 10));
    }

    [Fact]
    public void RemapAtBottom()
    {
        // offset=71 (max for tick=100, 30 visible: total 101 levels, max=71)
        // topPrice = 10000 - 71*100 = 2900
        // With tick=10: newOffset = (10000-2900)/10 = 710
        Assert.Equal(710, RemapScrollOffset(71, 100, 10));
    }

    [Fact]
    public void RemapPreservesVisibleRegion()
    {
        // After remap, the price at the top visible row should be the same
        int oldOffset = 35;
        int oldTick = 100;
        int topPrice = PriceAtGridIndex(oldOffset, oldTick);

        int newTick = 10;
        int newOffset = RemapScrollOffset(oldOffset, oldTick, newTick);
        int newTopPrice = PriceAtGridIndex(newOffset, newTick);

        Assert.Equal(topPrice, newTopPrice);
    }

    [Fact]
    public void MaxScrollOffset_StandardTick()
    {
        // tick=100: 101 total levels, 30 visible -> max=71
        Assert.Equal(71, MaxScrollOffset(100, 30));
    }

    [Fact]
    public void MaxScrollOffset_FineTick()
    {
        // tick=10: 1001 total levels, 30 visible -> max=971
        Assert.Equal(971, MaxScrollOffset(10, 30));
    }

    [Fact]
    public void ScrollOffsetForCenter_Middle()
    {
        // Center on 5000, tick=100: index=(10000-5000)/100=50, offset=50-15=35
        Assert.Equal(35, ScrollOffsetForCenter(5000, 100, 30));
    }

    [Fact]
    public void ScrollOffsetForCenter_ClampsLow()
    {
        // Center on 9900 (near top): index=1, offset=1-15=-14 -> clamped to 0
        Assert.Equal(0, ScrollOffsetForCenter(9900, 100, 30));
    }

    [Fact]
    public void ScrollOffsetForCenter_ClampsHigh()
    {
        // Center on 100 (near bottom), tick=100: index=99, offset=99-15=84 -> max=71
        Assert.Equal(71, ScrollOffsetForCenter(100, 100, 30));
    }
}

public class GridIndexTests
{
    [Theory]
    [InlineData(0, 100, 10000)]    // top of grid
    [InlineData(1, 100, 9900)]
    [InlineData(50, 100, 5000)]    // middle
    [InlineData(100, 100, 0)]      // bottom
    public void PriceAtIndex(int index, int tickSize, int expected)
    {
        Assert.Equal(expected, PriceAtGridIndex(index, tickSize));
    }

    [Theory]
    [InlineData(0, 10, 10000)]
    [InlineData(1, 10, 9990)]
    [InlineData(500, 10, 5000)]
    [InlineData(1000, 10, 0)]
    public void PriceAtIndexFineTick(int index, int tickSize, int expected)
    {
        Assert.Equal(expected, PriceAtGridIndex(index, tickSize));
    }
}
