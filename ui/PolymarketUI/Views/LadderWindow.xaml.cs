using System.Windows;
using PolymarketUI.ViewModels;

namespace PolymarketUI.Views;

public partial class LadderWindow : Window
{
    public string SeriesKey { get; }

    public LadderWindow(LadderViewModel viewModel)
    {
        InitializeComponent();
        DataContext = viewModel;
        SeriesKey = viewModel.SeriesKey;
    }

    protected override void OnClosed(EventArgs e)
    {
        if (DataContext is LadderViewModel vm)
            vm.Dispose();
        base.OnClosed(e);
    }
}
