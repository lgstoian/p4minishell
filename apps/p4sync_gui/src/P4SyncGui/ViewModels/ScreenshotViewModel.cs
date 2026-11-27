// SPDX-FileCopyrightText: 2026 Stoian Alexandru
// SPDX-License-Identifier: MIT
using System.IO;
using System.Security.Cryptography;
using System.Windows.Input;
using System.Windows.Media.Imaging;
using Microsoft.Win32;
using P4SyncBridge;
using P4SyncGui.Services;

namespace P4SyncGui.ViewModels;

/// <summary>BMPX screenshot via bridge, PNG preview and save-as.</summary>
public sealed class ScreenshotViewModel : ObservableObject
{
    private readonly AppState _app;

    public AppState App => _app;
    private string? _sourcePath;
    private byte[]? _pngBytes;

    private BitmapImage? _preview;
    public BitmapImage? Preview
    {
        get => _preview;
        private set => Set(ref _preview, value);
    }

    private string _details = "(no capture yet)";
    public string Details
    {
        get => _details;
        private set => Set(ref _details, value);
    }

    private string _integrity = "";
    /// <summary>Byte/hash checks against bridge-declared size and sha256.</summary>
    public string Integrity
    {
        get => _integrity;
        private set => Set(ref _integrity, value);
    }

    public ICommand CaptureCommand { get; }
    public ICommand SaveAsCommand { get; }

    public ScreenshotViewModel(AppState app)
    {
        _app = app;
        CaptureCommand = new AsyncRelayCommand(_ => Capture());
        SaveAsCommand = new RelayCommand(_ => SaveAs());
    }

    private async Task Capture()
    {
        Preview = null;
        _pngBytes = null;
        _sourcePath = null;
        Details = "(capturing…)";
        Integrity = "";

        var reply = await _app.RunAsync("Capture screenshot", "screenshot");
        if (reply?.Ok != true)
        {
            Details = "Capture failed: " + (reply?.Error ?? _app.LastError ?? "no bridge reply");
            return;
        }

        var shot = BridgeData.Screenshot(reply.Data);
        byte[]? bytes = null;
        string source = "";
        var errors = new List<string>();

        // Preferred path: bridge.py writes the capture once, then reports both
        // the absolute path and the base64 bytes of that same PNG. Either one
        // is enough; a bad base64 string must not hide a readable path (or vice
        // versa), and the exact decode error is kept in the log.
        if (shot.Base64 is not null)
        {
            try
            {
                string base64 = shot.Base64;
                if (base64.StartsWith("data:", StringComparison.OrdinalIgnoreCase))
                {
                    int comma = base64.IndexOf(',');
                    if (comma >= 0) base64 = base64[(comma + 1)..];
                }
                bytes = Convert.FromBase64String(base64);
                source = "png_base64 payload";
            }
            catch (Exception ex)
            {
                string error = "PNG base64 decode failed: " + ex.Message;
                errors.Add(error);
                _app.AddLog("error", error);
            }
        }

        if (bytes is null && !string.IsNullOrWhiteSpace(shot.Path))
        {
            try
            {
                _sourcePath = Path.GetFullPath(shot.Path);
                bytes = File.ReadAllBytes(_sourcePath);
                source = _sourcePath;
            }
            catch (Exception ex)
            {
                string error = "PNG path read failed: " + ex.Message;
                errors.Add(error);
                _app.AddLog("error", error);
            }
        }

        if (bytes is null)
        {
            string error = shot.HasBase64Field
                ? "Bridge PNG payload could not be decoded or read; see log for the exact error."
                : "Bridge returned neither png_base64 nor a readable out/path.";
            _app.AddLog("error", error);
            Details = error;
            Integrity = string.Join(" | ", errors);
            return;
        }

        try
        {
            Preview = LoadImage(bytes);
        }
        catch (Exception ex)
        {
            string error = "PNG preview failed: " + ex.Message;
            _app.AddLog("error", error);
            _pngBytes = null;
            Details = error;
            Integrity = string.Join(" | ", errors);
            return;
        }

        _pngBytes = bytes;
        string size = shot.Width is not null && shot.Height is not null
            ? $" · {shot.Width}x{shot.Height}" : "";
        Details = $"Source: {source} · {bytes.Length} bytes{size}";

        var checks = new List<string>();
        if (shot.Size is not null && shot.Size.Value != bytes.Length)
            checks.Add($"size mismatch: bridge={shot.Size.Value}, decoded={bytes.Length}");
        if (!string.IsNullOrWhiteSpace(shot.Sha256))
        {
            string actual = Convert.ToHexString(SHA256.HashData(bytes)).ToLowerInvariant();
            if (!string.Equals(actual, shot.Sha256.Trim(), StringComparison.OrdinalIgnoreCase))
                checks.Add($"sha256 mismatch: bridge={shot.Sha256.Trim()}, decoded={actual}");
        }
        foreach (string check in checks) _app.AddLog("warn", check);
        Integrity = checks.Count == 0
            ? (shot.Sha256 is null && shot.Size is null
                ? "No bridge checksum/size supplied; preview is the bridge PNG bytes."
                : "Bridge sha256/size match the preview bytes.")
            : string.Join(" | ", checks);
        if (errors.Count > 0)
            Integrity = string.Join(" | ", errors.Concat(checks));
    }

    private static BitmapImage LoadImage(byte[] bytes)
    {
        var image = new BitmapImage();
        using var stream = new MemoryStream(bytes);
        image.BeginInit();
        image.CacheOption = BitmapCacheOption.OnLoad;
        image.StreamSource = stream;
        image.EndInit();
        image.Freeze();
        return image;
    }

    private void SaveAs()
    {
        if (_pngBytes is null)
        {
            _app.AddLog("warn", "Capture a screenshot before saving.");
            return;
        }

        var dialog = new SaveFileDialog
        {
            Title = "Save screenshot as PNG",
            Filter = "PNG image (*.png)|*.png",
            DefaultExt = ".png",
            AddExtension = true,
            FileName = "p4sync-shot.png",
        };
        if (dialog.ShowDialog() != true) return;

        try
        {
            // Always write the exact bytes that produced the preview, even when
            // the bridge supplied a path: never copy a stale or renamed file.
            File.WriteAllBytes(dialog.FileName, _pngBytes);
            _app.AddLog("info", "Screenshot saved to " + dialog.FileName);
        }
        catch (Exception ex)
        {
            _app.AddLog("error", "Save failed: " + ex.Message);
        }
    }
}
