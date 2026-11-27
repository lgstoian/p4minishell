// SPDX-FileCopyrightText: 2026 Stoian Alexandru
// SPDX-License-Identifier: MIT
using System.Collections.ObjectModel;
using System.Windows.Input;
using P4SyncBridge;
using P4SyncGui.Services;

namespace P4SyncGui.ViewModels;

/// <summary>Apps/Packages pane: every device action is a bridge `pkg`/`verify` request.</summary>
public sealed class AppsViewModel : ObservableObject
{
    private readonly AppState _app;

    public AppState App => _app;

    public ObservableCollection<string> Verbs { get; } = new()
    {
        "list", "install", "verify", "uninstall", "update",
        "info", "check", "purge", "outdated", "unpack",
    };

    public string SelectedVerb { get; set; } = "list";
    public string PackageName { get; set; } = "";
    public string CrcTarget { get; set; } = "";
    public string Crc32Text { get; set; } = "";

    public ObservableCollection<string> Rows { get; } = new();

    private string _output = "(no package command yet)";
    public string Output
    {
        get => _output;
        private set => Set(ref _output, value);
    }

    private string _commandText = "";
    public string CommandText
    {
        get => _commandText;
        private set => Set(ref _commandText, value);
    }

    public ICommand RunCommand { get; }
    public ICommand VerifyCrcCommand { get; }

    public AppsViewModel(AppState app)
    {
        _app = app;
        RunCommand = new AsyncRelayCommand(_ => RunPkg());
        VerifyCrcCommand = new AsyncRelayCommand(_ => VerifyCrc());
    }

    private async Task RunPkg()
    {
        var reply = await _app.RunAsync($"pkg {SelectedVerb}", "pkg", new Dictionary<string, object?>
        {
            ["verb"] = SelectedVerb,
            ["package"] = PackageName,
        });
        if (reply?.Ok != true) return;

        var pkg = BridgeData.Pkg(reply.Data);
        CommandText = string.IsNullOrWhiteSpace(pkg.Command) ? "(bridge did not report a command)" : pkg.Command;
        Output = string.IsNullOrWhiteSpace(pkg.Output) ? "(no output reported)" : pkg.Output;
        Rows.Clear();
        foreach (string row in pkg.Rows) Rows.Add(row);
        OnPropertyChanged(nameof(Output));
        OnPropertyChanged(nameof(CommandText));
    }

    private async Task VerifyCrc()
    {
        uint? crc = null;
        if (uint.TryParse(Crc32Text.Trim(), out uint parsed)) crc = parsed;
        else if (!string.IsNullOrWhiteSpace(Crc32Text))
        {
            _app.AddLog("warn", "CRC-32 must be an unsigned integer; sending without crc32.");
        }

        var reply = await _app.RunAsync($"Verify {CrcTarget}", "verify", new Dictionary<string, object?>
        {
            ["remote"] = CrcTarget,
            ["crc32"] = crc,
        });
        if (reply?.Ok != true) return;

        var rows = BridgeData.Rows(reply.Data);
        Output = string.Join(Environment.NewLine, rows.Select(r => r.Key + ": " + r.Value));
        Rows.Clear();
        OnPropertyChanged(nameof(Output));
    }
}
