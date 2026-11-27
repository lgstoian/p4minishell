// SPDX-FileCopyrightText: 2026 Stoian Alexandru
// SPDX-License-Identifier: MIT
using System.Collections.ObjectModel;
using System.Text.RegularExpressions;
using System.Windows.Input;
using P4SyncBridge;
using P4SyncGui.Services;

namespace P4SyncGui.ViewModels;

/// <summary>
/// Native shell pane. The command is sent to bridge `shell`; C# never opens a
/// serial port. Output is the bridge's ANSI-stripped text.
/// </summary>
public sealed class ShellViewModel : ObservableObject
{
    private static readonly Regex Destructive = new(
        @"\b(reboot|format|erase|fdisk|mkfs|flash|rm|rmdir|del|eraseall|pkg\s+remove|uninstall|install|update)\b",
        RegexOptions.IgnoreCase | RegexOptions.Compiled);

    private static readonly Regex Ansi = new(
        @"\x1B\[[0-9;?]*[ -/]*[@-~]", RegexOptions.Compiled);

    private readonly AppState _app;

    public AppState App => _app;

    public ObservableCollection<string> History { get; } = new();

    private string _command = "";
    public string Command
    {
        get => _command;
        set
        {
            if (Set(ref _command, value))
            {
                OnPropertyChanged(nameof(IsDestructive));
                OnPropertyChanged(nameof(CanRun));
            }
        }
    }

    public int TimeoutSeconds { get; set; } = 25;
    public int SettleMs { get; set; } = 4000;
    public bool PlainMode { get; set; }
    public bool StripAnsi { get; set; } = true;
    public bool Acknowledged { get; set; }

    public bool IsDestructive => Destructive.IsMatch(Command);
    public bool CanRun => !string.IsNullOrWhiteSpace(Command) &&
                          !_app.IsBusy && (!IsDestructive || Acknowledged);

    private string _output = "(no shell command yet)";
    public string Output
    {
        get => _output;
        private set => Set(ref _output, value);
    }

    private string _errorLevel = "";
    public string ErrorLevel
    {
        get => _errorLevel;
        private set => Set(ref _errorLevel, value);
    }

    private int _historyIndex = -1;

    public ICommand RunCommand { get; }
    public ICommand ClearCommand { get; }

    public ShellViewModel(AppState app)
    {
        _app = app;
        RunCommand = new AsyncRelayCommand(_ => Run());
        ClearCommand = new RelayCommand(_ => { Output = ""; ErrorLevel = ""; });
        _app.PropertyChanged += (_, e) =>
        {
            if (e.PropertyName == nameof(AppState.IsBusy)) OnPropertyChanged(nameof(CanRun));
        };
    }

    private async Task Run()
    {
        string command = Command.Trim();
        if (command.Length == 0) return;

        var reply = await _app.RunAsync("Shell: " + command, "shell", new Dictionary<string, object?>
        {
            ["command"] = command,
            ["timeout"] = TimeoutSeconds,
            ["settle"] = SettleMs,
            ["plain"] = PlainMode,
        });

        if (!History.Contains(command)) History.Insert(0, command);
        _historyIndex = -1;

        if (reply?.Ok != true) return;
        var result = BridgeData.Shell(reply.Data);
        string output = result.Output;
        if (StripAnsi) output = Ansi.Replace(output, "");
        Output = string.IsNullOrEmpty(output) ? "(no output)" : output;
        ErrorLevel = result.ErrorLevel is null
            ? "errorlevel: not reported (plain mode)"
            : "errorlevel: " + result.ErrorLevel.Value;
        OnPropertyChanged(nameof(Output));
        OnPropertyChanged(nameof(ErrorLevel));
    }

    public void HistoryBack()
    {
        if (History.Count == 0) return;
        _historyIndex = Math.Clamp(_historyIndex + 1, 0, History.Count - 1);
        Command = History[_historyIndex];
    }

    public void HistoryForward()
    {
        if (History.Count == 0) return;
        _historyIndex = Math.Clamp(_historyIndex - 1, -1, History.Count - 1);
        Command = _historyIndex < 0 ? "" : History[_historyIndex];
    }
}
