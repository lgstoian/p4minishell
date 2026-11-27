// SPDX-FileCopyrightText: 2026 Stoian Alexandru
// SPDX-License-Identifier: MIT
// @module p4sync_gui/state
// @layer app
// @domain P4Sync desktop client
// @provides shared bridge session state, request runner, logs and version refusal
// @status prototype
// @test README.md
using System.Collections.ObjectModel;
using System.Windows;
using P4SyncBridge;

namespace P4SyncGui.Services;

/// <summary>One log row from bridge events or local UI errors.</summary>
public sealed record LogLine(string Time, string Level, string Message);

/// <summary>Shared state behind all page view models.</summary>
public sealed class AppState : ObservableObject
{
    private static readonly HashSet<string> DeviceCommands = new(StringComparer.OrdinalIgnoreCase)
    {
        "status", "push", "pull", "screenshot", "full-image", "verify", "shell", "pkg",
    };

    private CancellationTokenSource? _active;
    private DeviceItem? _selectedDevice;
    private StatusSnapshot? _status;
    private bool _isBusy;
    private int _progressPct;
    private string _progressMessage = "";
    private string _activeRequest = "";
    private string _lastError = "";
    private string _lastRawJson = "(no response yet)";

    public SettingsService Settings { get; }
    public CredentialService Credentials { get; }
    public BridgeClient Bridge { get; }

    public ObservableCollection<LogLine> Logs { get; } = new();

    public string LastRawJson
    {
        get => _lastRawJson;
        private set => Set(ref _lastRawJson, value);
    }

    public string LastError
    {
        get => _lastError;
        private set => Set(ref _lastError, value);
    }

    public DeviceItem? SelectedDevice
    {
        get => _selectedDevice;
        set => Set(ref _selectedDevice, value);
    }

    public StatusSnapshot? Status
    {
        get => _status;
        set => Set(ref _status, value);
    }

    public bool IsBusy
    {
        get => _isBusy;
        private set
        {
            if (Set(ref _isBusy, value)) OnPropertyChanged(nameof(IsIdle));
        }
    }

    public bool IsIdle => !IsBusy;

    public int ProgressPct
    {
        get => _progressPct;
        private set => Set(ref _progressPct, value);
    }

    public string ProgressMessage
    {
        get => _progressMessage;
        private set => Set(ref _progressMessage, value);
    }

    public string ActiveRequest
    {
        get => _activeRequest;
        private set => Set(ref _activeRequest, value);
    }

    public string ConnectionLabel => Bridge.IsMock
        ? "MOCK BRIDGE"
        : (Bridge.IsConnected ? "BRIDGE CONNECTED" : "BRIDGE IDLE");

    public AppState(SettingsService settings)
    {
        Settings = settings;
        Credentials = new CredentialService();
        Bridge = new BridgeClient(settings, Credentials);
        Bridge.MessageReceived += OnBridgeMessage;
        Bridge.ConnectionChanged += () =>
        {
            if (Application.Current is null) { OnPropertyChanged(nameof(ConnectionLabel)); return; }
            Application.Current.Dispatcher.BeginInvoke(() => OnPropertyChanged(nameof(ConnectionLabel)));
        };
    }

    /// <summary>
    /// Run a bridge command. Injects the selected port where the contract
    /// expects one, refuses all device work after a proto mismatch, and never
    /// fabricates a response when the bridge fails.
    /// </summary>
    public async Task<BridgeMessage?> RunAsync(
        string title,
        string cmd,
        Dictionary<string, object?>? args = null)
    {
        if (IsBusy)
        {
            AddLog("warn", "A bridge request is already running: " + ActiveRequest);
            return null;
        }

        if (Status is not null && !Status.VersionOk && DeviceCommands.Contains(cmd))
        {
            LastError = $"Refusing '{cmd}': {Status.VersionError}";
            AddLog("error", LastError);
            return null;
        }

        args ??= new Dictionary<string, object?>();
        if (DeviceCommands.Contains(cmd) && SelectedDevice is not null &&
            !args.ContainsKey("port") && !string.IsNullOrWhiteSpace(SelectedDevice.Port))
        {
            args["port"] = SelectedDevice.Port;
        }

        IsBusy = true;
        ActiveRequest = title;
        ProgressPct = 0;
        ProgressMessage = "";
        LastError = "";
        _active = new CancellationTokenSource();

        var progress = new Progress<BridgeProgress>(p =>
        {
            ProgressPct = Math.Clamp(p.Pct, 0, 100);
            ProgressMessage = p.Message;
        });

        try
        {
            var reply = await Bridge.SendAsync(cmd, args, progress, _active.Token);
            LastRawJson = reply.Kind == BridgeMessageKind.Response
                ? BridgeProtocol.Pretty(reply.Data)
                : reply.RawJson;

            if (reply.Ok)
                AddLog("info", title + ": ok");
            else
            {
                LastError = reply.Error ?? "Bridge reported a failure without an error message.";
                AddLog("error", title + ": " + LastError);
            }
            return reply;
        }
        catch (OperationCanceledException)
        {
            LastError = "Cancelled by user; bridge was restarted.";
            AddLog("warn", title + ": " + LastError);
            return null;
        }
        catch (Exception ex)
        {
            LastError = ex.Message;
            AddLog("error", title + ": " + ex.Message);
            return null;
        }
        finally
        {
            IsBusy = false;
            ActiveRequest = "";
            _active?.Dispose();
            _active = null;
        }
    }

    /// <summary>Cancel is supported on long-running bridge operations.</summary>
    public void Cancel()
    {
        if (!IsBusy) return;
        AddLog("warn", "Cancel requested: " + ActiveRequest);
        _active?.Cancel();
        Bridge.Kill(); // guarantees a device transfer is not left mid-flight
    }

    public void AddLog(string level, string message)
    {
        void Insert()
        {
            Logs.Insert(0, new LogLine(DateTime.Now.ToString("HH:mm:ss"), level, message));
            while (Logs.Count > 500) Logs.RemoveAt(Logs.Count - 1);
        }

        if (Application.Current is null) Insert();
        else if (Application.Current.Dispatcher.CheckAccess()) Insert();
        else Application.Current.Dispatcher.Invoke(Insert);
    }

    private void OnBridgeMessage(BridgeMessage message)
    {
        switch (message.Kind)
        {
            case BridgeMessageKind.Log:
                AddLog(message.Level ?? "info", message.Message ?? message.RawJson);
                break;
            case BridgeMessageKind.Unknown:
                AddLog("warn", message.Message ?? message.RawJson);
                break;
            // Responses are logged by RunAsync; progress only drives the bars.
        }
    }
}
