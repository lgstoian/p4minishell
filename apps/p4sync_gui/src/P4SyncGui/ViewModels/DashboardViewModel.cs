// SPDX-FileCopyrightText: 2026 Stoian Alexandru
// SPDX-License-Identifier: MIT
using System.Windows.Input;
using P4SyncBridge;
using P4SyncGui.Services;

namespace P4SyncGui.ViewModels;

/// <summary>`sync status` capability snapshot with a strict version verdict.</summary>
public sealed class DashboardViewModel : ObservableObject
{
    private readonly AppState _app;

    public AppState App => _app;

    public StatusSnapshot? Snapshot
    {
        get => _app.Status;
        set
        {
            _app.Status = value;
            OnPropertyChanged();
            OnPropertyChanged(nameof(VersionBanner));
            OnPropertyChanged(nameof(HasMismatch));
        }
    }

    public bool HasMismatch => Snapshot is not null && !Snapshot.VersionOk;

    public string VersionBanner => Snapshot is null
        ? "No capability snapshot yet. Run Refresh status."
        : Snapshot.VersionOk
            ? $"Compatible: {Snapshot.Proto} on {Snapshot.Board}"
            : "VERSION MISMATCH — " + Snapshot.VersionError;

    public ICommand RefreshCommand { get; }

    public DashboardViewModel(AppState app)
    {
        _app = app;
        RefreshCommand = new AsyncRelayCommand(_ => Refresh());
        _app.PropertyChanged += (_, e) =>
        {
            if (e.PropertyName == nameof(AppState.Status))
            {
                OnPropertyChanged(nameof(Snapshot));
                OnPropertyChanged(nameof(VersionBanner));
                OnPropertyChanged(nameof(HasMismatch));
            }
        };
    }

    public async Task Refresh()
    {
        var reply = await _app.RunAsync("Read sync status", "status");
        if (reply?.Ok != true) return;
        Snapshot = BridgeData.Status(reply.Data, "p4sync-1");
    }
}
