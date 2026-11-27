// SPDX-FileCopyrightText: 2026 Stoian Alexandru
// SPDX-License-Identifier: MIT
using System.Collections.ObjectModel;
using System.Windows.Input;
using P4SyncBridge;
using P4SyncGui.Services;

namespace P4SyncGui.ViewModels;

/// <summary>Discovery page. Ports come only from bridge `devices`.</summary>
public sealed class DevicesViewModel : ObservableObject
{
    private readonly AppState _app;

    public AppState App => _app;

    public ObservableCollection<DeviceItem> Devices { get; } = new();

    public DeviceItem? SelectedDevice
    {
        get => _app.SelectedDevice;
        set
        {
            _app.SelectedDevice = value;
            OnPropertyChanged();
        }
    }

    public ICommand RefreshCommand { get; }
    public ICommand ConnectCommand { get; }

    public DevicesViewModel(AppState app)
    {
        _app = app;
        RefreshCommand = new AsyncRelayCommand(_ => Refresh());
        ConnectCommand = new RelayCommand(_ =>
        {
            if (SelectedDevice is not null)
                _app.AddLog("info", $"Selected {SelectedDevice.Slug} on {SelectedDevice.Port}");
        });
        _app.PropertyChanged += (_, e) =>
        {
            if (e.PropertyName == nameof(AppState.SelectedDevice)) OnPropertyChanged(nameof(SelectedDevice));
        };
    }

    public async Task Refresh()
    {
        var reply = await _app.RunAsync("Discover devices", "devices");
        if (reply?.Ok != true) return;

        Devices.Clear();
        foreach (var device in BridgeData.Devices(reply.Data))
            Devices.Add(device);
        OnPropertyChanged(nameof(Devices));
    }
}
