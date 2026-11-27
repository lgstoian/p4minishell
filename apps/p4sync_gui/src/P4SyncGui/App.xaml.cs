// SPDX-FileCopyrightText: 2026 Stoian Alexandru
// SPDX-License-Identifier: MIT
// @module p4sync_gui/app
// @layer app
// @domain P4Sync desktop client
// @provides WPF bootstrap, --mock launch mode,theme init
// @status prototype
// @test README.md
using System.Windows;
using P4SyncGui.Services;
using P4SyncGui.ViewModels;

namespace P4SyncGui;

public partial class App : Application
{
    protected override void OnStartup(StartupEventArgs e)
    {
        base.OnStartup(e);

        var settings = SettingsService.Load();
        if (e.Args.Any(a => a.Equals("--mock", StringComparison.OrdinalIgnoreCase)))
            settings.MockMode = true; // --mock always wins for an offline demo

        ThemeService.Apply(settings.Theme);
        var state = new AppState(settings);
        var model = new MainViewModel(state);

        // Optional deep link for smoke tests / demos: --page <devices|dashboard|...>
        int pageArg = Array.FindIndex(e.Args, a => a.Equals("--page", StringComparison.OrdinalIgnoreCase));
        if (pageArg >= 0 && pageArg + 1 < e.Args.Length)
        {
            string key = e.Args[pageArg + 1];
            model.SelectedNav = model.Pages.FirstOrDefault(p =>
                p.Key.Equals(key, StringComparison.OrdinalIgnoreCase)) ?? model.Pages[0];
        }

        var window = new MainWindow { DataContext = model };
        MainWindow = window;
        window.Show();
    }

    protected override void OnExit(ExitEventArgs e)
    {
        SettingsService.Current?.Save();
        base.OnExit(e);
    }
}
