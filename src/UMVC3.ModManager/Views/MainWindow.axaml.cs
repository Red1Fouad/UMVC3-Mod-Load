using Avalonia.Controls;
using UMVC3.ModManager.ViewModels;

namespace UMVC3.ModManager.Views;

public partial class MainWindow : Window
{
    public MainWindow()
    {
        InitializeComponent();
    }

    protected override void OnDataContextChanged(EventArgs e)
    {
        base.OnDataContextChanged(e);
        if (DataContext is MainViewModel vm)
            vm.Attach(this);
    }

    private void OnModToggled(object? sender, Avalonia.Interactivity.RoutedEventArgs e)
    {
        if (DataContext is MainViewModel vm)
            vm.NotifyModToggled();
    }
}
