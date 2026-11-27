// SPDX-FileCopyrightText: 2026 Stoian Alexandru
// SPDX-License-Identifier: MIT
using System.Windows.Input;
using P4SyncGui.Services;

namespace P4SyncGui.ViewModels;

/// <summary>Dry-run plan, progress/event log, raw bridge payload and cancel.</summary>
public sealed class PlanViewModel : ObservableObject
{
    private readonly AppState _app;

    public AppState App => _app;

    public string LocalFolder
    {
        get => _app.Settings.LocalFolder;
        set { _app.Settings.LocalFolder = value; OnPropertyChanged(); }
    }

    public string RemoteFolder
    {
        get => _app.Settings.RemoteFolder;
        set { _app.Settings.RemoteFolder = value; OnPropertyChanged(); }
    }

    public string VerifyRemote { get; set; } = "";
    public string Crc32Text { get; set; } = "";

    public string RawJson => _app.LastRawJson;
    public string ErrorText => _app.LastError;

    public ICommand RunPlanCommand { get; }
    public ICommand RunVerifyCommand { get; }
    public ICommand CancelCommand { get; }

    public PlanViewModel(AppState app)
    {
        _app = app;
        RunPlanCommand = new AsyncRelayCommand(_ => RunPlan());
        RunVerifyCommand = new AsyncRelayCommand(_ => RunVerify());
        CancelCommand = new RelayCommand(_ => app.Cancel());
        app.PropertyChanged += (_, e) =>
        {
            if (e.PropertyName == nameof(AppState.LastRawJson)) OnPropertyChanged(nameof(RawJson));
            if (e.PropertyName == nameof(AppState.LastError)) OnPropertyChanged(nameof(ErrorText));
        };
    }

    private async Task RunPlan()
    {
        await _app.RunAsync("Dry-run plan", "plan", new Dictionary<string, object?>
        {
            ["local"] = LocalFolder,
            ["remote"] = RemoteFolder,
            ["dry_run"] = true,
            ["cache"] = false,
        });
    }

    private async Task RunVerify()
    {
        uint? crc = null;
        if (uint.TryParse(Crc32Text.Trim(), out uint parsed)) crc = parsed;
        await _app.RunAsync("Verify " + VerifyRemote, "verify", new Dictionary<string, object?>
        {
            ["remote"] = VerifyRemote,
            ["crc32"] = crc,
        });
    }
}
