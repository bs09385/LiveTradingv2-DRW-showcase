using PolymarketUI.Models;
using PolymarketUI.ViewModels;
using Xunit;

namespace PolymarketUI.Tests;

public class OrderLifecycleModelTests
{
    [Theory]
    [InlineData("WORKING")]
    [InlineData("FILLED")]
    [InlineData("CANCELED_NO_FILL")]
    [InlineData("CANCELED_WITH_FILL")]
    [InlineData("REJECTED")]
    public void WorkingOrderModel_Status_UsesLifecycleState(string lifecycle)
    {
        var model = new WorkingOrderModel
        {
            LifecycleState = lifecycle
        };

        Assert.Equal(lifecycle, model.Status);
    }

    [Fact]
    public void WorkingOrderModel_Status_FallsBackToWorkingForLegacyFlags()
    {
        var pending = new WorkingOrderModel { LifecycleState = "", IsPending = true, IsLive = false };
        var live = new WorkingOrderModel { LifecycleState = "", IsPending = false, IsLive = true };

        Assert.Equal("WORKING", pending.Status);
        Assert.Equal("WORKING", live.Status);
    }
}

public class WorkingOrdersViewModelTests
{
    [Fact]
    public void Update_PopulatesWorkingClosedAndTradeSections()
    {
        var vm = new WorkingOrdersViewModel();
        var state = new StateSnapshot
        {
            WorkingOrders =
            {
                new WorkingOrderModel { ClientOrderId = "w1", LifecycleState = "WORKING" }
            },
            ClosedOrders =
            {
                new WorkingOrderModel { ClientOrderId = "c1", LifecycleState = "CANCELED_WITH_FILL" }
            },
            Trades =
            {
                new TradeHistoryModel { TradeId = "t1", Status = "MATCHED", Price = 5200, Size = 3 }
            }
        };

        vm.Update(state);

        Assert.Single(vm.WorkingOrders);
        Assert.Single(vm.ClosedOrders);
        Assert.Single(vm.TradeHistory);
        Assert.Equal("c1", vm.ClosedOrders[0].ClientOrderId);
        Assert.Equal("CANCELED_WITH_FILL", vm.ClosedOrders[0].Status);
    }

    [Fact]
    public void Update_DerivesMarketLabelsFromMetadata()
    {
        var vm = new WorkingOrdersViewModel();
        var state = new StateSnapshot
        {
            WorkingOrders =
            {
                new WorkingOrderModel
                {
                    ClientOrderId = "w1",
                    MarketId = "cond-5m",
                    AssetId = "tok-up",
                    LifecycleState = "WORKING"
                }
            },
            Trades =
            {
                new TradeHistoryModel
                {
                    TradeId = "t1",
                    MarketId = "cond-5m",
                    AssetId = "tok-down",
                    Status = "MATCHED"
                }
            }
        };

        var markets = new List<MarketSnapshot>
        {
            new()
            {
                ConditionId = "cond-5m",
                TokenIdUp = "tok-up",
                TokenIdDown = "tok-down",
                SeriesLabel = "BTC 5M"
            }
        };

        vm.Update(state, markets);

        Assert.Equal("BTC 5M UP", vm.WorkingOrders[0].MarketLabel);
        Assert.Equal("BTC 5M DOWN", vm.TradeHistory[0].MarketLabel);
    }

    [Fact]
    public void SizeFields_AreFormattedToTwoDecimalPlaces()
    {
        var order = new WorkingOrderModel
        {
            OriginalSize = 5,
            FilledSize = 2
        };
        var trade = new TradeHistoryModel { Size = 3 };

        Assert.Equal("5.00", order.OriginalSizeStr);
        Assert.Equal("2.00", order.FilledSizeStr);
        Assert.Equal("3.00", order.RemainingStr);
        Assert.Equal("3.00", trade.SizeStr);
    }

    [Theory]
    [InlineData("MATCHED", true)]
    [InlineData("MINED", true)]
    [InlineData("CONFIRMED", true)]
    [InlineData("FAILED", false)]
    [InlineData("RETRYING", false)]
    public void TradeHistoryModel_IdentifiesFillRelatedStates(string status, bool expected)
    {
        var trade = new TradeHistoryModel { Status = status };
        Assert.Equal(expected, trade.IsFillRelatedState);
    }
}
