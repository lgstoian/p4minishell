// SPDX-FileCopyrightText: 2026 Stoian Alexandru
// SPDX-License-Identifier: MIT
// @module p4sync_gui/tests/protocol-tests
// @layer leaf
// @domain P4Sync desktop client
// @provides hardware-free assertions over mock bridge JSON lines
// @status prototype
// @test dotnet run --project apps/p4sync_gui/tests/P4SyncBridge.Tests
using System.Security.Cryptography;
using System.Text.Json;
using System.Xml.Linq;
using P4SyncBridge;

namespace P4SyncBridge.Tests;

/// <summary>
/// Tiny dependency-free test runner. Exit code 0 = all assertions passed.
/// Deliberately no NuGet test framework: runs offline on a clean SDK.
/// </summary>
public static class Program
{
    private static int _passed;
    private static int _failed;

    public static int Main()
    {
        Run("response: ok + id + data", () =>
        {
            var m = Parse(MockBridgeData.Devices);
            Eq(BridgeMessageKind.Response, m.Kind);
            Eq(1, m.Id);
            IsTrue(m.Ok);
            IsTrue(m.Data.ValueKind == JsonValueKind.Object);
        });

        Run("response: error frame", () =>
        {
            var m = Parse(MockBridgeData.Error);
            IsTrue(!m.Ok);
            Eq("proto mismatch: device='p4sync-0' host='p4sync-1'", m.Error);
        });

        Run("event: progress", () =>
        {
            var m = Parse(MockBridgeData.Progress);
            Eq(BridgeMessageKind.Progress, m.Kind);
            Eq(45, m.Pct!.Value);
            Eq("receive notes.md (45%)", m.Message);
        });

        Run("event: log", () =>
        {
            var m = Parse(MockBridgeData.Log);
            Eq(BridgeMessageKind.Log, m.Kind);
            Eq("warn", m.Level);
            Eq("sd descriptor pool low", m.Message);
        });

        Run("unknown JSON stays visible, invalid JSON is rejected", () =>
        {
            var m = Parse(MockBridgeData.UnknownFrame);
            Eq(BridgeMessageKind.Unknown, m.Kind);
            IsTrue(!BridgeProtocol.TryParse("not-json", out _, out string err) && err.Length > 0);
        });

        Run("request serialisation uses exact contract keys", () =>
        {
            var args = new Dictionary<string, object?>
            {
                ["port"] = "COM6",
                ["remote"] = "notes.md",
                ["local"] = @"C:\tmp\notes.md",
                ["dry_run"] = true,
            };
            string line = BridgeProtocol.SerializeRequest(12, "push", args);
            using var doc = JsonDocument.Parse(line);
            var root = doc.RootElement;
            Eq(12, root.GetProperty("id").GetInt32());
            Eq("push", root.GetProperty("cmd").GetString());
            Eq("COM6", root.GetProperty("port").GetString());
            Eq("notes.md", root.GetProperty("remote").GetString());
            IsTrue(root.GetProperty("dry_run").GetBoolean());
        });

        Run("devices: port/board/status projection", () =>
        {
            var devices = BridgeData.Devices(Parse(MockBridgeData.Devices).Data);
            Eq(2, devices.Count);
            Eq("tab5", devices[0].Slug);
            Eq("COM6", devices[0].Port);
            Eq("ready", devices[0].State);
        });

        Run("status: nested caps validate against p4sync-1", () =>
        {
            var s = BridgeData.Status(Parse(MockBridgeData.Status).Data, "p4sync-1");
            IsTrue(s.VersionOk, s.VersionError);
            Eq("p4sync-1", s.Proto);
            Eq("jc1060p470c", s.Board);
            Eq("mounted", s.Sd);
            Eq("unlocked", s.Lock);
            IsTrue(s.Capabilities.Any(c => c.Key == "magic.sdfx"));
            IsTrue(s.Capabilities.Any(c => c.Key == "limit.transfer"));
        });

        Run("status: proto mismatch is a hard refusal", () =>
        {
            var s = BridgeData.Status(Parse(MockBridgeData.StatusMismatch).Data, "p4sync-1");
            IsTrue(!s.VersionOk);
            IsTrue(s.VersionError.Contains("proto mismatch", StringComparison.Ordinal), s.VersionError);
            IsTrue(s.VersionError.Contains("missing capabilities", StringComparison.Ordinal), s.VersionError);
        });

        Run("plan: actions[] projected into steps + listings", () =>
        {
            var plan = BridgeData.Plan(Parse(MockBridgeData.Plan).Data);
            Eq(2, plan.Steps.Count);
            Eq("push", plan.Steps[0].Action);
            Eq("notes.md", plan.Steps[0].Path);
            Eq("newer local mtime", plan.Steps[0].Detail);
            Eq(1, plan.LocalFiles.Count);
            Eq("notes.md", plan.LocalFiles[0].Path);
            Eq(42L, plan.LocalFiles[0].Size!.Value);
            Eq(0, plan.DeviceFiles.Count);
            IsTrue(plan.Summary.Contains("push", StringComparison.Ordinal));
        });

        Run("screenshot: out path and base64 variants", () =>
        {
            var shot = BridgeData.Screenshot(Parse(MockBridgeData.Screenshot).Data);
            Eq(@"C:\temp\shot.png", shot.Path);
            Eq(320, shot.Width!.Value);
            var encoded = BridgeData.Screenshot(Parse(MockBridgeData.ScreenshotBase64).Data);
            IsTrue(encoded.Base64 is not null && encoded.Base64.StartsWith("iVBOR", StringComparison.Ordinal));
        });

        Run("shell: output + errorlevel projection", () =>
        {
            var shell = BridgeData.Shell(Parse(MockBridgeData.Shell).Data);
            Eq("ver", shell.Command);
            Eq("P4MiniShell 1.6.0\r\nbuild ok", shell.Output);
            Eq(0, shell.ErrorLevel!.Value);
            IsTrue(shell.Ok!.Value);
        });

        Run("verify + pkg + full-image projections", () =>
        {
            var verify = Parse(MockBridgeData.Verify).Data;
            Eq("notes.md", BridgeData.Rows(verify).First(c => c.Key == "remote").Value);

            var pkg = BridgeData.Pkg(Parse(MockBridgeData.Pkg).Data);
            Eq("list", pkg.Verb);
            Eq("pkg list /b", pkg.Command);
            Eq(2, pkg.Rows.Count);
            IsTrue(pkg.Ok!.Value);

            var image = Parse(MockBridgeData.FullImage).Data;
            var rows = BridgeData.Rows(image);
            IsTrue(rows.Any(c => c.Key == "manifest_path"));
            IsTrue(rows.Any(c => c.Key == "manifest.version"));
        });

        Run("status: limit.screenshot strict when present, optional when absent", () =>
        {
            var advertised = BridgeData.Status(Parse(MockBridgeData.Status).Data, "p4sync-1");
            IsTrue(advertised.VersionOk, advertised.VersionError);
            var absent = BridgeData.Status(Parse(MockBridgeData.StatusWithoutShotLimit).Data, "p4sync-1");
            IsTrue(absent.VersionOk, absent.VersionError);
            var invalid = BridgeData.Status(Parse(MockBridgeData.StatusBadShotLimit).Data, "p4sync-1");
            IsTrue(!invalid.VersionOk);
            IsTrue(invalid.VersionError.Contains("invalid capability limit.screenshot='huge'", StringComparison.Ordinal),
                invalid.VersionError);
        });

        Run("screenshot: png_base64 + sha256/size metadata match the PNG bytes", () =>
        {
            var shot = BridgeData.Screenshot(Parse(MockBridgeData.ScreenshotFull).Data);
            IsTrue(shot.HasBase64Field);
            Eq(@"C:\temp\shot.png", shot.Path);
            Eq(69L, shot.Size!.Value);
            byte[] bytes = Convert.FromBase64String(shot.Base64!);
            Eq(69, bytes.Length);
            Eq("2edda04ac06bd5fb14f3651024824c34a7a15e5f17dd0d3928029ca1a6a0c962",
                Convert.ToHexString(SHA256.HashData(bytes)).ToLowerInvariant());
        });

        Run("screenshot: malformed explicit base64 stays visible for verbatim errors", () =>
        {
            var shot = BridgeData.Screenshot(Parse(MockBridgeData.ScreenshotBadBase64).Data);
            IsTrue(shot.HasBase64Field);
            Eq("not!base64", shot.Base64);
            Eq(@"C:\temp\shot.png", shot.Path);
        });

        Run("full-image: stale manifest flags are projected from the reply", () =>
        {
            var image = BridgeData.FullImage(Parse(MockBridgeData.FullImageStale).Data);
            IsTrue(image.StaleManifest!.Value);
            IsTrue(image.Rebuilt!.Value);
            IsTrue(image.Ok!.Value);
            Eq("manifest shot is not a path", image.StaleReason);
            IsTrue(image.Rows.Any(c => c.Key == "manifest_path"));
        });

        Run("full-image: rejected rebuilt manifest is not presented as success", () =>
        {
            var image = BridgeData.FullImage(Parse(MockBridgeData.FullImageRejected).Data);
            IsTrue(!image.Ok!.Value);
            IsTrue(image.StaleManifest!.Value);
            IsTrue(image.StaleReason.Contains("invalid image.json", StringComparison.Ordinal));
        });

        Run("drop-down brushes keep >=4.5:1 contrast in both themes", () =>
        {
            string root = FindAppRoot();
            string themesDir = Path.Combine(root, "src", "P4SyncGui", "Themes");
            string styles = File.ReadAllText(Path.Combine(themesDir, "Styles.xaml"));
            IsTrue(styles.Contains("x:Key=\"P4SyncComboBoxItem\""), "explicit ComboBoxItem style missing");
            IsTrue(styles.Contains("ItemContainerStyle"), "item style is not applied to the popup");
            IsTrue(styles.Contains("x:Name=\"PART_Popup\""), "themed popup template missing");
            IsTrue(styles.Contains("DynamicResource SurfaceAltBrush"), "popup/field background is not theme-driven");

            foreach (string theme in new[] { "Dark", "Light" })
            {
                var tokens = XDocument.Load(Path.Combine(themesDir, theme + "Theme.xaml")).Root!;
                var fg = Rgb(BrushColor(tokens, "TextBrush"));
                var bg = Rgb(BrushColor(tokens, "SurfaceAltBrush"));
                var sel = Rgb(BrushColor(tokens, "SelectionBrush"));
                double plain = Contrast(fg, bg);
                double selected = Contrast(fg, sel);
                IsTrue(plain >= 4.5, $"{theme}: field/popup {plain:F2}:1 (TextBrush vs SurfaceAltBrush)");
                IsTrue(selected >= 4.5, $"{theme}: highlighted item {selected:F2}:1 (TextBrush vs SelectionBrush)");
            }
        });

        Run("every ComboBox is reachable by an accessible name", () =>
        {
            string root = FindAppRoot();
            XNamespace presentation = "http://schemas.microsoft.com/winfx/2006/xaml/presentation";
            int count = 0;
            foreach (string file in Directory.GetFiles(Path.Combine(root, "src", "P4SyncGui"), "*.xaml", SearchOption.AllDirectories))
            {
                foreach (var combo in XDocument.Load(file).Descendants(presentation + "ComboBox"))
                {
                    count++;
                    bool named = combo.Attributes().Any(a => a.Name.LocalName == "AutomationProperties.Name");
                    IsTrue(named, $"{Path.GetFileName(file)}: ComboBox has no AutomationProperties.Name");
                }
            }
            Eq(2, count); // Apps verb + Settings theme; no hidden third drop-down
        });

        Console.WriteLine();
        Console.WriteLine($"RESULT: {_passed} passed, {_failed} failed");
        return _failed == 0 ? 0 : 1;
    }

    private static string FindAppRoot()
    {
        var dir = new DirectoryInfo(AppContext.BaseDirectory);
        for (int i = 0; i < 10 && dir is not null; i++, dir = dir.Parent)
        {
            string candidate = Path.Combine(dir.FullName, "src", "P4SyncGui", "Themes", "Styles.xaml");
            if (File.Exists(candidate)) return dir.FullName;
        }
        throw new FileNotFoundException("Could not locate apps/p4sync_gui/src/P4SyncGui/Themes/Styles.xaml");
    }

    private static string BrushColor(XElement dictionary, string key)
    {
        XNamespace xaml = "http://schemas.microsoft.com/winfx/2006/xaml";
        var brush = dictionary.Elements().FirstOrDefault(e => (string?)e.Attribute(xaml + "Key") == key);
        IsTrue(brush is not null, $"theme is missing brush '{key}'");
        string? color = (string?)brush!.Attribute("Color");
        IsTrue(color is not null, $"brush '{key}' has no Color");
        return color!;
    }

    private static (int R, int G, int B) Rgb(string argb)
    {
        string hex = argb.TrimStart('#');
        if (hex.Length == 8) hex = hex[2..]; // #AARRGGBB -> RRGGBB
        IsTrue(hex.Length == 6, $"unsupported colour literal '{argb}'");
        return (Convert.ToInt32(hex[..2], 16), Convert.ToInt32(hex[2..4], 16), Convert.ToInt32(hex[4..6], 16));
    }

    private static double Contrast((int R, int G, int B) a, (int R, int G, int B) b)
    {
        static double Channel(int value)
        {
            double v = value / 255.0;
            return v <= 0.03928 ? v / 12.92 : Math.Pow((v + 0.055) / 1.055, 2.4);
        }
        static double Luminance((int R, int G, int B) c) =>
            0.2126 * Channel(c.R) + 0.7152 * Channel(c.G) + 0.0722 * Channel(c.B);
        double l1 = Luminance(a), l2 = Luminance(b);
        return (Math.Max(l1, l2) + 0.05) / (Math.Min(l1, l2) + 0.05);
    }

    private static BridgeMessage Parse(string line)
    {
        IsTrue(BridgeProtocol.TryParse(line, out var m, out string err), err);
        return m;
    }

    private static void Run(string name, Action body)
    {
        try
        {
            body();
            _passed++;
            Console.WriteLine($"PASS  {name}");
        }
        catch (Exception ex)
        {
            _failed++;
            Console.WriteLine($"FAIL  {name}: {ex.Message}");
        }
    }

    private static void Eq<T>(T expected, T actual)
    {
        if (!EqualityComparer<T>.Default.Equals(expected, actual))
            throw new Exception($"expected '{expected}', got '{actual}'");
    }

    private static void IsTrue(bool condition, string? detail = null)
    {
        if (!condition) throw new Exception(detail ?? "assertion failed");
    }
}
