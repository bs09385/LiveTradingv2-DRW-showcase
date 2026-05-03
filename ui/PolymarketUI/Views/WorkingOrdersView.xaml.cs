using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;

namespace PolymarketUI.Views;

public partial class WorkingOrdersView : UserControl
{
    public WorkingOrdersView()
    {
        InitializeComponent();
    }

    private void DataGrid_PreviewMouseWheel(object sender, MouseWheelEventArgs e)
    {
        var scrollViewer = FindParentScrollViewer((DependencyObject)sender);
        if (scrollViewer != null)
        {
            scrollViewer.ScrollToVerticalOffset(scrollViewer.VerticalOffset - e.Delta / 3.0);
            e.Handled = true;
        }
    }

    private static ScrollViewer? FindParentScrollViewer(DependencyObject child)
    {
        var parent = VisualTreeHelper.GetParent(child);
        while (parent != null)
        {
            if (parent is ScrollViewer sv)
                return sv;
            parent = VisualTreeHelper.GetParent(parent);
        }
        return null;
    }
}
