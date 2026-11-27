// SPDX-FileCopyrightText: 2026 Stoian Alexandru
// SPDX-License-Identifier: MIT
// @module p4sync_gui/settings
// @layer app
// @domain P4Sync desktop client
// @provides user settings persisted under %APPDATA% (never in the repo)
// @status prototype
// @test README.md
using System.IO;
using System.Text.Json;

namespace P4SyncGui.Services;

/// <summary>Local-only UI settings. Credentials are NOT stored here.</summary>
public sealed class SettingsService
{
    public static SettingsService? Current { get; private set; }

    public string PythonPath { get; set; } = "python";
    public string BridgeScriptPath { get; set; } = "";
    public bool MockMode { get; set; }
    public string Theme { get; set; } = "Dark";
    public string CredentialTarget { get; set; } = "p4sync/default";
    public string LocalFolder { get; set; } = "";
    public string MirrorFolder { get; set; } = "";
    public string RemoteFolder { get; set; } = "";
    public string DatabaseName { get; set; } = "p4sync";

    private static string SettingsPath => Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData),
        "P4SyncGui", "settings.json");

    private static readonly JsonSerializerOptions JsonOptions = new()
    {
        WriteIndented = true,
        PropertyNameCaseInsensitive = true,
    };

    public static SettingsService Load()
    {
        SettingsService? loaded = null;
        try
        {
            if (File.Exists(SettingsPath))
                loaded = JsonSerializer.Deserialize<SettingsService>(File.ReadAllText(SettingsPath), JsonOptions);
        }
        catch
        {
            // A corrupt settings file must never block the app.
        }
        Current = loaded ?? new SettingsService();
        return Current;
    }

    public void Save()
    {
        try
        {
            string dir = Path.GetDirectoryName(SettingsPath)!;
            Directory.CreateDirectory(dir);
            File.WriteAllText(SettingsPath, JsonSerializer.Serialize(this, JsonOptions));
        }
        catch
        {
            // Settings persistence is best effort; no secret is ever involved.
        }
    }

    /// <summary>
    /// Configured script wins; otherwise discover tools/p4sync/bridge.py by
    /// walking up from the build output. No hardcoded COM ports, no repo writes.
    /// </summary>
    public string ResolveBridgeScript()
    {
        if (!string.IsNullOrWhiteSpace(BridgeScriptPath) && File.Exists(BridgeScriptPath))
            return Path.GetFullPath(BridgeScriptPath);

        var dir = new DirectoryInfo(AppContext.BaseDirectory);
        for (int i = 0; i < 8 && dir is not null; i++, dir = dir.Parent)
        {
            string candidate = Path.Combine(dir.FullName, "tools", "p4sync", "bridge.py");
            if (File.Exists(candidate)) return candidate;
        }
        return string.IsNullOrWhiteSpace(BridgeScriptPath)
            ? Path.Combine("tools", "p4sync", "bridge.py")
            : BridgeScriptPath;
    }
}
