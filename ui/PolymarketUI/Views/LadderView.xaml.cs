using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using PolymarketUI.ViewModels;

namespace PolymarketUI.Views;

public partial class LadderView : UserControl
{
    public LadderView()
    {
        InitializeComponent();
    }

    private void OnPreviewMouseWheel(object sender, MouseWheelEventArgs e)
    {
        if (DataContext is LadderViewModel vm)
        {
            // Delta > 0 = scroll up (see higher prices) = negative scroll offset change
            // Delta < 0 = scroll down (see lower prices) = positive scroll offset change
            int lines = e.Delta > 0 ? -3 : 3;
            vm.Scroll(lines);
            e.Handled = true;
        }
    }

    private void OnSizeChanged(object sender, SizeChangedEventArgs e)
    {
        if (DataContext is not LadderViewModel vm) return;

        var itemsControl = LadderItemsControl;
        if (itemsControl.ActualHeight < 1) return;

        int rows = (int)(itemsControl.ActualHeight / LadderViewModel.RowHeight);
        if (rows >= 5)
            vm.SetVisibleRows(rows);
    }
}
