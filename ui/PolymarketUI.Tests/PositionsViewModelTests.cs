using PolymarketUI.Models;
using PolymarketUI.ViewModels;
using Xunit;

namespace PolymarketUI.Tests;

public class PositionsViewModelTests
{
    [Fact]
    public void Update_UsesTokenFallbackWhenMarketsUnavailable()
    {
        var vm = new PositionsViewModel();
        var state = new StateSnapshot
        {
            Positions =
            {
                new TokenPositionModel { TokenId = "tok-up", Position = 7 },
                new TokenPositionModel { TokenId = "tok-down", Position = -3 }
            }
        };

        vm.Update(new List<MarketSnapshot>(), state);

        Assert.Equal(2, vm.Positions.Count);
        Assert.Equal(7, vm.Positions[0].PositionUp);
        Assert.Equal(-3, vm.Positions[1].PositionUp);
    }

    [Fact]
    public void Update_UsesMarketLabelsAndStateBackedTokenValues()
    {
        var vm = new PositionsViewModel();
        var markets = new List<MarketSnapshot>
        {
            new()
            {
                ConditionId = "cond-5m",
                SeriesLabel = "BTC 5M",
                TokenIdUp = "tok-up",
                TokenIdDown = "tok-down",
                PositionUp = 0,
                PositionDown = 0
            }
        };
        var state = new StateSnapshot
        {
            Positions =
            {
                new TokenPositionModel { TokenId = "tok-up", Position = 11 },
                new TokenPositionModel { TokenId = "tok-down", Position = 4 }
            }
        };

        vm.Update(markets, state);

        Assert.Single(vm.Positions);
        Assert.Equal("BTC 5M", vm.Positions[0].ConditionId);
        Assert.Equal(11, vm.Positions[0].PositionUp);
        Assert.Equal(4, vm.Positions[0].PositionDown);
        Assert.Equal(7, vm.Positions[0].NetExposure);
    }
}
