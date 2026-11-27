// SPDX-FileCopyrightText: 2026 Stoian Alexandru
// SPDX-License-Identifier: MIT
using System.Collections.ObjectModel;
using System.IO;
using System.Windows.Input;
using Microsoft.Win32;
using P4SyncBridge;
using P4SyncGui.Services;

namespace P4SyncGui.ViewModels;

/// <summary>
/// Local/device listings, dry-run plan and one-file-at-a-time SDFX push/pull.
/// All device I/O goes through the bridge; C# only enumerates local files.
/// </summary>
public sealed class FilesViewModel : ObservableObject
{
    private readonly AppState _app;

    public AppState App => _app;

    public ObservableCollection<FileItem> LocalFiles { get; } = new();
    public ObservableCollection<FileItem> DeviceFiles { get; } = new();
    public ObservableCollection<PlanItem> Steps { get; } = new();

    public FileItem? SelectedLocal { get; set; }
    public FileItem? SelectedDeviceFile { get; set; }

    public string LocalFolder
    {
        get => _app.Settings.LocalFolder;
        set
        {
            _app.Settings.LocalFolder = value;
            OnPropertyChanged();
            LoadLocal();
        }
    }

    public string RemoteFolder
    {
        get => _app.Settings.RemoteFolder;
        set
        {
            _app.Settings.RemoteFolder = value;
            OnPropertyChanged();
        }
    }

    public string ConflictPolicy { get; private set; } = "Not reported by bridge. P4Sync default: newer-wins by mtime.";
    public string PlanSummary { get; private set; } = "(no plan run yet)";

    public ICommand BrowseLocalCommand { get; }
    public ICommand RefreshLocalCommand { get; }
    public ICommand PlanCommand { get; }
    public ICommand PushCommand { get; }
    public ICommand PullCommand { get; }

    public FilesViewModel(AppState app)
    {
        _app = app;
        BrowseLocalCommand = new RelayCommand(_ => BrowseLocal());
        RefreshLocalCommand = new RelayCommand(_ => LoadLocal());
        PlanCommand = new AsyncRelayCommand(_ => RunPlan());
        PushCommand = new AsyncRelayCommand(_ => PushOne());
        PullCommand = new AsyncRelayCommand(_ => PullOne());
        LoadLocal();
    }

    private void BrowseLocal()
    {
        var dialog = new OpenFolderDialog { Title = "Local sync folder" };
        if (!string.IsNullOrWhiteSpace(LocalFolder)) dialog.InitialDirectory = LocalFolder;
        if (dialog.ShowDialog() == true) LocalFolder = dialog.FolderName;
    }

    private void LoadLocal()
    {
        LocalFiles.Clear();
        if (string.IsNullOrWhiteSpace(LocalFolder) || !Directory.Exists(LocalFolder)) return;
        try
        {
            foreach (string path in Directory.EnumerateFiles(LocalFolder, "*", SearchOption.TopDirectoryOnly))
            {
                var info = new FileInfo(path);
                LocalFiles.Add(new FileItem(info.Name, info.Length, path));
            }
        }
        catch (Exception ex)
        {
            _app.AddLog("error", "Local listing failed: " + ex.Message);
        }
    }

    private async Task RunPlan()
    {
        var reply = await _app.RunAsync("Dry-run plan", "plan", new Dictionary<string, object?>
        {
            ["local"] = LocalFolder,
            ["remote"] = RemoteFolder,
            ["dry_run"] = true,
            ["cache"] = false,
        });
        if (reply?.Ok != true) return;

        var plan = BridgeData.Plan(reply.Data);
        Steps.Clear();
        foreach (var step in plan.Steps) Steps.Add(step);

        DeviceFiles.Clear();
        foreach (var file in plan.DeviceFiles) DeviceFiles.Add(file);

        ConflictPolicy = string.IsNullOrWhiteSpace(plan.ConflictPolicy)
            ? "Not reported by bridge. P4Sync default: newer-wins by mtime."
            : plan.ConflictPolicy;
        PlanSummary = string.IsNullOrWhiteSpace(plan.Summary) ? "(no summary reported)" : plan.Summary;
        OnPropertyChanged(nameof(ConflictPolicy));
        OnPropertyChanged(nameof(PlanSummary));
    }

    private static string FileNameOf(string path)
    {
        string trimmed = path.Replace('\\', '/').TrimEnd('/');
        int slash = trimmed.LastIndexOf('/');
        return slash < 0 ? trimmed : trimmed[(slash + 1)..];
    }

    private async Task PushOne()
    {
        if (SelectedLocal is null)
        {
            _app.AddLog("warn", "Select exactly one local file to push.");
            return;
        }

        string localPath = Path.IsPathRooted(SelectedLocal.Path)
            ? SelectedLocal.Path
            : Path.Combine(LocalFolder, SelectedLocal.Path);
        string remote = string.IsNullOrWhiteSpace(RemoteFolder)
            ? FileNameOf(SelectedLocal.Path)
            : RemoteFolder.TrimEnd('/', '\\') + "/" + FileNameOf(SelectedLocal.Path);

        await _app.RunAsync($"Push {FileNameOf(SelectedLocal.Path)}", "push", new Dictionary<string, object?>
        {
            ["local"] = localPath,
            ["remote"] = remote,
        });
    }

    private async Task PullOne()
    {
        if (SelectedDeviceFile is null)
        {
            _app.AddLog("warn", "Select exactly one device file to pull.");
            return;
        }

        string remote = SelectedDeviceFile.Path;
        string local = string.IsNullOrWhiteSpace(LocalFolder)
            ? FileNameOf(remote)
            : Path.Combine(LocalFolder, FileNameOf(remote));

        await _app.RunAsync($"Pull {FileNameOf(remote)}", "pull", new Dictionary<string, object?>
        {
            ["remote"] = remote,
            ["local"] = local,
        });
    }
}
