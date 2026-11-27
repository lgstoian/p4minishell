// SPDX-FileCopyrightText: 2026 Stoian Alexandru
// SPDX-License-Identifier: MIT
using System.Collections.ObjectModel;
using System.IO;
using System.Windows;
using System.Windows.Input;
using Microsoft.Win32;
using P4SyncBridge;
using P4SyncGui.Services;

namespace P4SyncGui.ViewModels;

/// <summary>Contacts/Calendar view over bridge `full-image` (existing pim engine).</summary>
public sealed class PimViewModel : ObservableObject
{
    private readonly AppState _app;

    public AppState App => _app;

    public ObservableCollection<string> Contacts { get; } = new();
    public ObservableCollection<string> Calendar { get; } = new();
    public ObservableCollection<KeyValuePair<string, string>> Details { get; } = new();

    public string MirrorFolder
    {
        get => _app.Settings.MirrorFolder;
        set { _app.Settings.MirrorFolder = value; OnPropertyChanged(); }
    }

    public string DatabaseName
    {
        get => _app.Settings.DatabaseName;
        set { _app.Settings.DatabaseName = value; OnPropertyChanged(); }
    }

    public bool IncludeFiles { get; set; } = true;
    public bool IncludeShot { get; set; } = true;

    public string ManifestPath { get; private set; } = "(no image built yet)";

    private string _staleWarning = "";
    /// <summary>Authoritative stale/rebuild status from the bridge, never inferred from cached rows.</summary>
    public string StaleWarning
    {
        get => _staleWarning;
        private set
        {
            if (Set(ref _staleWarning, value))
                OnPropertyChanged(nameof(StaleWarningVisibility));
        }
    }

    public Visibility StaleWarningVisibility =>
        string.IsNullOrWhiteSpace(StaleWarning) ? Visibility.Collapsed : Visibility.Visible;

    public ICommand BrowseMirrorCommand { get; }
    public ICommand RunCommand { get; }
    public ICommand RetryCleanCommand { get; }

    public PimViewModel(AppState app)
    {
        _app = app;
        BrowseMirrorCommand = new RelayCommand(_ =>
        {
            var dialog = new OpenFolderDialog { Title = "PIM mirror directory" };
            if (!string.IsNullOrWhiteSpace(MirrorFolder)) dialog.InitialDirectory = MirrorFolder;
            if (dialog.ShowDialog() == true) MirrorFolder = dialog.FolderName;
        });
        RunCommand = new AsyncRelayCommand(_ => Run());
        RetryCleanCommand = new AsyncRelayCommand(_ => Run(forceRebuild: true));
    }

    /// <summary>
    /// Build the image. Old rows are dropped before the request so a failed or
    /// stale build can never be shown as if it were freshly rebuilt data.
    /// </summary>
    private async Task Run(bool forceRebuild = false)
    {
        ClearResults();
        StaleWarning = "";

        if (!string.IsNullOrWhiteSpace(MirrorFolder))
        {
            string manifest = Path.Combine(MirrorFolder, "image.json");
            if (File.Exists(manifest))
                Warn("Existing image.json found in the mirror. The bridge will validate it and rebuild the manifest from the current mirror before any results are shown.");
        }

        var args = new Dictionary<string, object?>
        {
            ["mirror"] = MirrorFolder,
            ["db"] = DatabaseName,
            ["files"] = IncludeFiles,
            ["shot"] = IncludeShot,
            ["force_rebuild"] = forceRebuild,
        };
        var reply = await _app.RunAsync("Build full image (PIM + files + screenshot)", "full-image", args);
        if (reply?.Ok != true)
        {
            Warn("Full image failed: " + (reply?.Error ?? _app.LastError ?? "no bridge reply"));
            return;
        }

        var image = BridgeData.FullImage(reply.Data);
        if (image.Ok == false)
        {
            // The bridge rejected even the rebuilt manifest. Keep every pane
            // empty rather than presenting stale rows as a successful image.
            Warn("Bridge rejected the rebuilt image.json; no data from this run is shown as fresh. " + image.StaleReason);
            return;
        }

        if (image.StaleManifest == true || image.Rebuilt == true)
        {
            string reason = string.IsNullOrWhiteSpace(image.StaleReason) ? "" : " Reason: " + image.StaleReason;
            Warn(image.StaleManifest == true
                ? "Stale image.json was detected and removed. The manifest was rebuilt from the current mirror before these results were shown." + reason
                : "Manifest was rebuilt from the current mirror before these results were shown." + reason);
        }

        var rows = image.Rows;
        foreach (var row in rows) Details.Add(row);

        foreach (string item in BridgeData.TextItems(reply.Data, "contacts", "vcard", "vcf"))
            Contacts.Add(item);

        foreach (string item in BridgeData.TextItems(reply.Data, "calendar", "events", "agenda", "ical"))
            Calendar.Add(item);

        ManifestPath = string.IsNullOrEmpty(image.ManifestPath) ? "(manifest path not reported)" : image.ManifestPath;
        OnPropertyChanged(nameof(ManifestPath));

        if (Contacts.Count == 0 && Calendar.Count == 0)
            _app.AddLog("info", "Bridge reported no contacts/calendar arrays; see Details and raw JSON.");
    }

    private void ClearResults()
    {
        Contacts.Clear();
        Calendar.Clear();
        Details.Clear();
        ManifestPath = "(no image built yet)";
        OnPropertyChanged(nameof(ManifestPath));
    }

    private void Warn(string message)
    {
        StaleWarning = message;
        _app.AddLog("warn", message);
    }
}
