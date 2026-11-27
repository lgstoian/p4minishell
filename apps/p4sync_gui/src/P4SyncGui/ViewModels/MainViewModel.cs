// SPDX-FileCopyrightText: 2026 Stoian Alexandru
// SPDX-License-Identifier: MIT
using System.Collections.ObjectModel;
using System.Windows.Input;
using P4SyncGui.Services;

namespace P4SyncGui.ViewModels;

/// <summary>Left-rail navigation entry.</summary>
public sealed record NavItem(string Key, string Title, string Glyph);

/// <summary>Application shell: navigation, theme, header chips and global actions.</summary>
public sealed class MainViewModel : ObservableObject
{
    public AppState App { get; }
    public ObservableCollection<NavItem> Pages { get; } = new()
    {
        new NavItem("devices", "Devices", "◎"),
        new NavItem("dashboard", "Dashboard", "▦"),
        new NavItem("files", "Files", "⇅"),
        new NavItem("screenshot", "Screenshot", "▣"),
        new NavItem("pim", "PIM", "▥"),
        new NavItem("apps", "Apps & Packages", "◫"),
        new NavItem("shell", "Shell", "›_"),
        new NavItem("plan", "Plan & Report", "☷"),
        new NavItem("settings", "Settings", "⚙"),
    };

    private NavItem? _selectedNav;
    private object? _current;

    public NavItem? SelectedNav
    {
        get => _selectedNav;
        set
        {
            if (Set(ref _selectedNav, value)) Navigate(value);
        }
    }

    public object? Current
    {
        get => _current;
        private set => Set(ref _current, value);
    }

    public DevicesViewModel Devices { get; }
    public DashboardViewModel Dashboard { get; }
    public FilesViewModel Files { get; }
    public ScreenshotViewModel Screenshot { get; }
    public PimViewModel Pim { get; }
    public AppsViewModel Apps { get; }
    public ShellViewModel Shell { get; }
    public PlanViewModel Plan { get; }
    public SettingsViewModel Settings { get; }

    public ICommand ToggleThemeCommand { get; }
    public ICommand RefreshStatusCommand { get; }
    public ICommand CancelCommand { get; }

    public string ThemeLabel => string.Equals(App.Settings.Theme, "Light", StringComparison.OrdinalIgnoreCase)
        ? "Light mode" : "Dark mode";

    public string DeviceLabel => App.SelectedDevice is null
        ? "No device selected"
        : $"{App.SelectedDevice.Slug}  •  {App.SelectedDevice.Port}";

    public string MismatchBanner => App.Status is not null && !App.Status.VersionOk
        ? "PROTOCOL MISMATCH — device operations refused. " + App.Status.VersionError
        : "";

    public MainViewModel(AppState app)
    {
        App = app;
        Devices = new DevicesViewModel(app);
        Dashboard = new DashboardViewModel(app);
        Files = new FilesViewModel(app);
        Screenshot = new ScreenshotViewModel(app);
        Pim = new PimViewModel(app);
        Apps = new AppsViewModel(app);
        Shell = new ShellViewModel(app);
        Plan = new PlanViewModel(app);
        Settings = new SettingsViewModel(app);

        ToggleThemeCommand = new RelayCommand(_ =>
        {
            App.Settings.Theme = string.Equals(App.Settings.Theme, "Light", StringComparison.OrdinalIgnoreCase)
                ? "Dark" : "Light";
            ThemeService.Apply(App.Settings.Theme);
            App.Settings.Save();
            OnPropertyChanged(nameof(ThemeLabel));
        });

        RefreshStatusCommand = new AsyncRelayCommand(_ => Dashboard.Refresh());
        CancelCommand = new RelayCommand(_ => App.Cancel());

        App.PropertyChanged += (_, e) =>
        {
            if (e.PropertyName == nameof(AppState.SelectedDevice)) OnPropertyChanged(nameof(DeviceLabel));
            if (e.PropertyName == nameof(AppState.Status)) OnPropertyChanged(nameof(MismatchBanner));
        };
        Settings.PropertyChanged += (_, e) =>
        {
            if (e.PropertyName == nameof(SettingsViewModel.Theme)) OnPropertyChanged(nameof(ThemeLabel));
        };

        SelectedNav = Pages[0];
    }

    private void Navigate(NavItem? item)
    {
        Current = item?.Key switch
        {
            "devices" => Devices,
            "dashboard" => Dashboard,
            "files" => Files,
            "screenshot" => Screenshot,
            "pim" => Pim,
            "apps" => Apps,
            "shell" => Shell,
            "plan" => Plan,
            "settings" => Settings,
            _ => Devices,
        };
    }
}
