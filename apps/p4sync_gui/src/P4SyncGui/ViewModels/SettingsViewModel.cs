// SPDX-FileCopyrightText: 2026 Stoian Alexandru
// SPDX-License-Identifier: MIT
using System.Collections.ObjectModel;
using System.Windows.Input;
using Microsoft.Win32;
using P4SyncGui.Services;

namespace P4SyncGui.ViewModels;

/// <summary>Settings pane: bridge path, mock mode, theme, OS credential store.</summary>
public sealed class SettingsViewModel : ObservableObject
{
    private readonly AppState _app;

    public AppState App => _app;

    public ObservableCollection<string> Themes { get; } = new() { "Dark", "Light" };

    public string PythonPath
    {
        get => _app.Settings.PythonPath;
        set { _app.Settings.PythonPath = value; OnPropertyChanged(); }
    }

    public string BridgeScriptPath
    {
        get => _app.Settings.BridgeScriptPath;
        set { _app.Settings.BridgeScriptPath = value; OnPropertyChanged(); OnPropertyChanged(nameof(ResolvedBridge)); }
    }

    public string ResolvedBridge => _app.Bridge.BridgeScript;

    public bool MockMode
    {
        get => _app.Settings.MockMode;
        set { _app.Settings.MockMode = value; OnPropertyChanged(); OnPropertyChanged(nameof(ResolvedBridge)); }
    }

    public string CredentialTarget
    {
        get => _app.Settings.CredentialTarget;
        set { _app.Settings.CredentialTarget = value; OnPropertyChanged(); OnPropertyChanged(nameof(CredentialUser)); }
    }

    public string Theme
    {
        get => _app.Settings.Theme;
        set { _app.Settings.Theme = value; OnPropertyChanged(); }
    }

    public string CredentialUser => _app.Credentials.DisplayUser(CredentialTarget) ?? "(none stored)";

    public string LocalFolder
    {
        get => _app.Settings.LocalFolder;
        set { _app.Settings.LocalFolder = value; OnPropertyChanged(); }
    }

    public string MirrorFolder
    {
        get => _app.Settings.MirrorFolder;
        set { _app.Settings.MirrorFolder = value; OnPropertyChanged(); }
    }

    public string RemoteFolder
    {
        get => _app.Settings.RemoteFolder;
        set { _app.Settings.RemoteFolder = value; OnPropertyChanged(); }
    }

    public ICommand SaveCommand { get; }
    public ICommand BrowseBridgeCommand { get; }
    public ICommand DeleteCredentialCommand { get; }

    public SettingsViewModel(AppState app)
    {
        _app = app;
        SaveCommand = new RelayCommand(_ =>
        {
            app.Settings.Save();
            ThemeService.Apply(app.Settings.Theme);
            app.Bridge.Kill(); // next request starts with the new bridge/mock settings
            app.AddLog("info", "Settings saved. Bridge will restart on the next request.");
            OnPropertyChanged(nameof(ResolvedBridge));
        });
        BrowseBridgeCommand = new RelayCommand(_ =>
        {
            var dialog = new OpenFileDialog
            {
                Title = "Select bridge.py",
                Filter = "Python bridge (bridge.py)|bridge.py|Python files (*.py)|*.py",
            };
            if (dialog.ShowDialog() == true) BridgeScriptPath = dialog.FileName;
        });
        DeleteCredentialCommand = new RelayCommand(_ =>
        {
            app.Credentials.Delete(CredentialTarget);
            OnPropertyChanged(nameof(CredentialUser));
            app.AddLog("info", "Stored credential deleted from the OS store.");
        });
    }

    /// <summary>Called from SettingsView code-behind so the password never enters a VM property.</summary>
    public string SaveCredential(string userName, string password)
    {
        string store = _app.Credentials.Save(CredentialTarget, userName, password);
        OnPropertyChanged(nameof(CredentialUser));
        _app.AddLog("info", $"Credential saved in {store}. Value hidden.");
        return store;
    }
}
