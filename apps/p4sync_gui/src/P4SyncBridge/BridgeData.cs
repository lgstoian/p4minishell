// SPDX-FileCopyrightText: 2026 Stoian Alexandru
// SPDX-License-Identifier: MIT
// @module p4sync_gui/bridge-data
// @layer leaf
// @domain P4Sync desktop client
// @provides tolerant projections of bridge data payloads into view models
// @status prototype
// @test tests/P4SyncBridge.Tests
// @hard-rule Missing keys are reported as unknown, never defaulted to plausible values.
using System.Text.Json;

namespace P4SyncBridge;

/// <summary>One discovered board. Raw is the exact bridge object as JSON.</summary>
public sealed record DeviceItem(string Slug, string Port, string State, string Raw);

/// <summary>Capability snapshot from `status` plus a strict version verdict.</summary>
public sealed record StatusSnapshot(
    string Proto,
    string Board,
    string Sd,
    string Lock,
    IReadOnlyList<KeyValuePair<string, string>> Capabilities,
    bool VersionOk,
    string VersionError,
    string Raw);

/// <summary>One file row in the files pane.</summary>
public sealed record FileItem(string Path, long? Size, string Raw);

/// <summary>One step of a dry-run plan.</summary>
public sealed record PlanItem(string Action, string Path, string Detail, string Raw);

/// <summary>Combined dry-run report (`plan` command).</summary>
public sealed record PlanReport(
    IReadOnlyList<FileItem> LocalFiles,
    IReadOnlyList<FileItem> DeviceFiles,
    IReadOnlyList<PlanItem> Steps,
    string ConflictPolicy,
    string Summary,
    string Raw);

/// <summary>
/// Decoded screenshot metadata. Path and Base64 may both be present: bridge.py
/// reports the saved PNG path and its base64 bytes together. HasBase64Field
/// distinguishes "bridge sent a bad base64 string" from "bridge sent no PNG".
/// </summary>
public sealed record ScreenshotResult(
    string? Path, string? Base64, int? Width, int? Height, string Raw,
    string? Sha256, long? Size, bool HasBase64Field);

/// <summary>Result of a `full-image` request; stale/rebuilt flags are authoritative from the bridge.</summary>
public sealed record ImageResult(
    bool? StaleManifest, bool? Rebuilt, string StaleReason, bool? Ok,
    string ManifestPath, IReadOnlyList<KeyValuePair<string, string>> Rows);

/// <summary>Result of a bridge `shell` command (all serial work stays in bridge.py).</summary>
public sealed record ShellResult(
    string Port,
    string Command,
    string Output,
    int? ErrorLevel,
    bool? Ok,
    string Raw);

/// <summary>Result of a `pkg` verb (shelled out by the bridge, never by C#).</summary>
public sealed record PkgResult(
    string Verb,
    string Package,
    string Command,
    string Output,
    IReadOnlyList<string> Rows,
    bool? Ok,
    string Raw);

/// <summary>Pure, defensive projections. All lookups are case-insensitive.</summary>
public static class BridgeData
{
    private static readonly string[] RequiredCaps =
    {
        "proto", "board", "sd", "lock",
        "magic.sdfx", "magic.pimx", "magic.bmpx",
        "limit.transfer",
    };

    /// <summary>Devices from an array, a wrapped array, or a single object.</summary>
    public static IReadOnlyList<DeviceItem> Devices(JsonElement data)
    {
        var result = new List<DeviceItem>();
        var array = FindArray(data, "devices", "boards", "results", "items");
        if (array is not null)
        {
            foreach (var item in array.Value.EnumerateArray())
                result.Add(ToDevice(item));
        }
        else if (data.ValueKind == JsonValueKind.Object)
        {
            result.Add(ToDevice(data));
        }
        return result;
    }

    private static DeviceItem ToDevice(JsonElement item)
    {
        if (item.ValueKind != JsonValueKind.Object)
            return new DeviceItem("unknown", "", "unknown", item.GetRawText());

        string slug = FirstString(item, "slug", "board", "id", "name") ?? "unknown";
        string port = FirstString(item, "port", "com", "device", "path") ?? "";
        string state = FirstString(item, "state", "status", "online", "connected") ?? "unknown";
        return new DeviceItem(slug, port, state, item.GetRawText());
    }

    /// <summary>
    /// Flatten capability data (nested objects use dotted keys) and compare the
    /// advertised proto tag. A mismatch or missing key is a hard refusal.
    /// </summary>
    public static StatusSnapshot Status(JsonElement data, string expectedProto)
    {
        var caps = new List<KeyValuePair<string, string>>();
        if (data.ValueKind == JsonValueKind.Object)
            FlattenStatus(data, caps);

        string Get(string key) => caps.FirstOrDefault(c => c.Key.Equals(key, StringComparison.OrdinalIgnoreCase)).Value ?? "";

        string proto = Get("proto");
        string board = Get("board");
        string sd = Get("sd");
        string lockState = Get("lock");

        string versionError = "";
        if (!proto.Equals(expectedProto, StringComparison.OrdinalIgnoreCase))
            versionError = $"proto mismatch: device='{proto}' host='{expectedProto}'";

        var missing = RequiredCaps.Where(k => !caps.Any(c => c.Key.Equals(k, StringComparison.OrdinalIgnoreCase))).ToList();
        if (missing.Count > 0)
        {
            string m = "missing capabilities: " + string.Join(", ", missing);
            versionError = versionError.Length == 0 ? m : versionError + "; " + m;
        }

        // limit.screenshot is optional by presence (old firmware does not print
        // it), but once advertised it must be a usable positive byte count.
        var shotLimit = caps.FirstOrDefault(c => c.Key.Equals("limit.screenshot", StringComparison.OrdinalIgnoreCase));
        if (shotLimit.Key is not null &&
            (!long.TryParse(shotLimit.Value, out long limit) || limit <= 0))
        {
            string m = $"invalid capability limit.screenshot='{shotLimit.Value}'";
            versionError = versionError.Length == 0 ? m : versionError + "; " + m;
        }

        return new StatusSnapshot(proto, board, sd, lockState, caps,
            versionError.Length == 0, versionError,
            data.ValueKind == JsonValueKind.Undefined ? "(no data payload)" : data.GetRawText());
    }

    /// <summary>Generic file arrays from `plan` or any command that lists files.</summary>
    public static IReadOnlyList<FileItem> Files(JsonElement data)
    {
        var result = new List<FileItem>();
        var array = FindArray(data, "files", "entries", "items", "local", "localFiles", "local_files",
            "device", "deviceFiles", "remote", "remoteFiles");
        if (array is null)
            return result;
        foreach (var item in array.Value.EnumerateArray())
            result.Add(ToFile(item));
        return result;
    }

    /// <summary>Parse a `plan` dry-run payload into two listings and step rows.</summary>
    public static PlanReport Plan(JsonElement data)
    {
        var local = new List<FileItem>(ToFiles(FindArray(data, "local", "localFiles", "local_files", "source")));
        var device = new List<FileItem>(ToFiles(FindArray(data, "device", "deviceFiles", "remote", "remoteFiles", "target")));
        var steps = new List<PlanItem>();

        var stepArray = FindArray(data, "steps", "plan", "actions", "changes", "operations", "items");
        if (stepArray is not null)
        {
            foreach (var item in stepArray.Value.EnumerateArray())
            {
                if (item.ValueKind != JsonValueKind.Object)
                {
                    steps.Add(new PlanItem("", item.GetRawText(), "", item.GetRawText()));
                    continue;
                }
                string action = FirstString(item, "op", "action", "kind", "policy", "verb") ?? "";
                string path = FirstString(item, "path", "file", "remote", "local", "name") ?? "";
                string detail = FirstString(item, "reason", "detail", "message", "note", "conflict") ?? "";
                steps.Add(new PlanItem(action, path, detail, item.GetRawText()));

                // The bridge's plan reports actions[] instead of separate
                // local/device arrays: derive the two listings from the ops.
                if (local.Count == 0 && action.Equals("push", StringComparison.OrdinalIgnoreCase))
                    local.Add(new FileItem(path, GetInt64(item, "size", "bytes", "length"), item.GetRawText()));
                if (device.Count == 0 && action.Equals("pull", StringComparison.OrdinalIgnoreCase))
                    device.Add(new FileItem(path, GetInt64(item, "size", "bytes", "length"), item.GetRawText()));
            }
        }

        string policy = "";
        if (data.ValueKind == JsonValueKind.Object)
        {
            policy = FirstString(data, "conflictPolicy", "conflict_policy", "policy", "conflict") ?? "";
        }

        string summary = "";
        if (data.ValueKind == JsonValueKind.Object)
        {
            foreach (var name in new[] { "summary", "report", "totals" })
            {
                if (TryGetCI(data, name, out var value) && value.ValueKind == JsonValueKind.Object)
                {
                    summary = BridgeProtocol.Pretty(value);
                    break;
                }
            }
        }

        return new PlanReport(local, device, steps, policy, summary,
            data.ValueKind == JsonValueKind.Undefined ? "(no data payload)" : data.GetRawText());
    }

    /// <summary>
    /// Decode screenshot metadata: a saved path and/or a base64 PNG string.
    /// An explicit *_base64 field is kept even when malformed so the caller can
    /// report Convert.FromBase64String's error verbatim instead of silently
    /// pretending the bridge sent no PNG.
    /// </summary>
    public static ScreenshotResult Screenshot(JsonElement data)
    {
        string? path = null;
        string? base64 = null;
        bool hasBase64Field = false;
        int? width = null;
        int? height = null;
        string? sha256 = null;
        long? size = null;

        if (data.ValueKind == JsonValueKind.Object)
        {
            path = FirstString(data, "out", "path", "file", "png", "pngPath", "png_path", "output", "savedTo");

            // Explicit base64 keys are trusted enough to attempt a decode.
            foreach (string name in new[] { "png_base64", "base64", "pngBase64", "b64" })
            {
                if (TryGetCI(data, name, out var value) && value.ValueKind == JsonValueKind.String)
                {
                    hasBase64Field = true;
                    base64 = value.GetString();
                    break;
                }
            }
            if (base64 is null)
            {
                string? candidate = FirstStringOnly(data, "data", "image", "bytes");
                if (LooksLikeBase64(candidate)) base64 = candidate;
            }

            width = GetInt(data, "width");
            height = GetInt(data, "height");
            if ((width is null || height is null) && TryGetCI(data, "size", out var sizeNode) &&
                sizeNode.ValueKind == JsonValueKind.Object)
            {
                width ??= GetInt(sizeNode, "width", "w");
                height ??= GetInt(sizeNode, "height", "h");
            }
            else
            {
                size = GetInt64(data, "size", "bytes");
            }
            sha256 = FirstStringOnly(data, "sha256", "sha_256", "hash");
        }
        else if (data.ValueKind == JsonValueKind.String)
        {
            string? s = data.GetString();
            if (LooksLikeBase64(s)) base64 = s;
            else path = s;
        }

        return new ScreenshotResult(path, base64, width, height,
            data.ValueKind == JsonValueKind.Undefined ? "(no data payload)" : data.GetRawText(),
            sha256, size, hasBase64Field);
    }

    /// <summary>Authoritative stale/rebuild flags from a `full-image` reply.</summary>
    public static ImageResult FullImage(JsonElement data)
    {
        string manifestPath = FirstString(data, "manifest_path", "manifestPath", "manifest") ?? "";
        return new ImageResult(
            GetBool(data, "stale_manifest", "staleManifest"),
            GetBool(data, "rebuilt"),
            FirstStringOnly(data, "stale_reason", "staleReason") ?? "",
            GetBool(data, "ok"),
            manifestPath,
            Rows(data));
    }

    /// <summary>Display strings for PIM contacts/calendar arrays (generic, no parser).</summary>
    public static IReadOnlyList<string> TextItems(JsonElement data, params string[] names)
    {
        var result = new List<string>();
        var array = FindArray(data, names);
        if (array is null) return result;
        foreach (var item in array.Value.EnumerateArray())
        {
            if (item.ValueKind == JsonValueKind.Object)
            {
                string? s = FirstString(item, "name", "app", "displayName", "summary", "uid", "title", "path", "id");
                result.Add(s ?? item.GetRawText());
            }
            else
            {
                result.Add(item.ToString());
            }
        }
        return result;
    }

    /// <summary>Parse a `shell` response; output is the bridge's ANSI-stripped text.</summary>
    public static ShellResult Shell(JsonElement data)
    {
        string port = "", command = "", output = "";
        int? errorLevel = null;
        bool? ok = null;

        if (data.ValueKind == JsonValueKind.Object)
        {
            port = FirstString(data, "port") ?? "";
            command = FirstString(data, "command") ?? "";
            output = FirstString(data, "output") ?? "";
            errorLevel = GetInt(data, "errorlevel", "exitcode", "code");
            foreach (var name in new[] { "ok", "success" })
            {
                if (TryGetCI(data, name, out var value) && value.ValueKind is JsonValueKind.True or JsonValueKind.False)
                {
                    ok = value.GetBoolean();
                    break;
                }
            }
        }

        return new ShellResult(port, command, output, errorLevel, ok,
            data.ValueKind == JsonValueKind.Undefined ? "(no data payload)" : data.GetRawText());
    }

    /// <summary>Parse a `pkg` response; rows/output are kept verbatim.</summary>
    public static PkgResult Pkg(JsonElement data)
    {
        string verb = "", package = "", command = "", output = "";
        bool? ok = null;
        var rows = new List<string>();

        if (data.ValueKind == JsonValueKind.Object)
        {
            verb = FirstString(data, "verb") ?? "";
            package = FirstString(data, "package") ?? "";
            command = FirstString(data, "command") ?? "";
            output = FirstString(data, "output") ?? "";
            foreach (var name in new[] { "ok", "success" })
            {
                if (TryGetCI(data, name, out var value) && value.ValueKind is JsonValueKind.True or JsonValueKind.False)
                {
                    ok = value.GetBoolean();
                    break;
                }
            }
            rows.AddRange(TextItems(data, "rows", "packages", "items"));
        }

        return new PkgResult(verb, package, command, output, rows, ok,
            data.ValueKind == JsonValueKind.Undefined ? "(no data payload)" : data.GetRawText());
    }

    /// <summary>Flat key/value rows for any data payload (drives generic detail panes).</summary>
    public static IReadOnlyList<KeyValuePair<string, string>> Rows(JsonElement data)
    {
        var rows = new List<KeyValuePair<string, string>>();
        if (data.ValueKind == JsonValueKind.Object)
            Flatten(data, "", rows);
        return rows;
    }

    // -- helpers ---------------------------------------------------------

    private static IReadOnlyList<FileItem> ToFiles(JsonElement? array)
    {
        var result = new List<FileItem>();
        if (array is null) return result;
        foreach (var item in array.Value.EnumerateArray())
            result.Add(ToFile(item));
        return result;
    }

    private static FileItem ToFile(JsonElement item)
    {
        if (item.ValueKind == JsonValueKind.Object)
        {
            string path = FirstString(item, "path", "file", "name", "remote", "local") ?? "unknown";
            long? size = GetInt64(item, "size", "bytes", "length");
            return new FileItem(path, size, item.GetRawText());
        }
        return new FileItem(item.ToString(), null, item.GetRawText());
    }

    private static JsonElement? FindArray(JsonElement data, params string[] names)
    {
        if (data.ValueKind == JsonValueKind.Array) return data;
        if (data.ValueKind != JsonValueKind.Object) return null;
        foreach (string name in names)
        {
            if (TryGetCI(data, name, out var value) && value.ValueKind == JsonValueKind.Array)
                return value;
        }
        return null;
    }

    private static string? FirstString(JsonElement obj, params string[] names)
    {
        foreach (string name in names)
        {
            if (TryGetCI(obj, name, out var value))
            {
                if (value.ValueKind == JsonValueKind.String) return value.GetString();
                if (value.ValueKind is JsonValueKind.Number or JsonValueKind.True or JsonValueKind.False)
                    return value.ToString();
            }
        }
        return null;
    }

    private static string? FirstStringOnly(JsonElement obj, params string[] names)
    {
        foreach (string name in names)
        {
            if (TryGetCI(obj, name, out var value) && value.ValueKind == JsonValueKind.String)
                return value.GetString();
        }
        return null;
    }

    private static bool? GetBool(JsonElement obj, params string[] names)
    {
        foreach (string name in names)
        {
            if (TryGetCI(obj, name, out var value) && value.ValueKind is JsonValueKind.True or JsonValueKind.False)
                return value.GetBoolean();
        }
        return null;
    }

    private static int? GetInt(JsonElement obj, params string[] names)
    {
        foreach (string name in names)
        {
            if (TryGetCI(obj, name, out var value) &&
                value.ValueKind == JsonValueKind.Number && value.TryGetInt32(out int n))
                return n;
        }
        return null;
    }

    private static long? GetInt64(JsonElement obj, params string[] names)
    {
        foreach (string name in names)
        {
            if (TryGetCI(obj, name, out var value) &&
                value.ValueKind == JsonValueKind.Number && value.TryGetInt64(out long n))
                return n;
        }
        return null;
    }

    private static bool TryGetCI(JsonElement obj, string name, out JsonElement value)
    {
        if (obj.ValueKind == JsonValueKind.Object)
        {
            if (obj.TryGetProperty(name, out value)) return true;
            foreach (var prop in obj.EnumerateObject())
            {
                if (prop.Name.Equals(name, StringComparison.OrdinalIgnoreCase))
                {
                    value = prop.Value;
                    return true;
                }
            }
        }
        value = default;
        return false;
    }

    /// <summary>
    /// `status` wraps capabilities in a `caps` object; flatten that container
    /// transparently so magic.*/limit.* validate exactly like protocol.py.
    /// </summary>
    private static void FlattenStatus(JsonElement obj, List<KeyValuePair<string, string>> rows)
    {
        foreach (var prop in obj.EnumerateObject())
        {
            if (prop.Value.ValueKind == JsonValueKind.Object &&
                (prop.Name.Equals("caps", StringComparison.OrdinalIgnoreCase) ||
                 prop.Name.Equals("capabilities", StringComparison.OrdinalIgnoreCase)))
            {
                foreach (var inner in prop.Value.EnumerateObject())
                {
                    if (inner.Value.ValueKind == JsonValueKind.Object)
                        Flatten(inner.Value, inner.Name, rows);
                    else
                        rows.Add(new KeyValuePair<string, string>(inner.Name, Scalar(inner.Value)));
                }
                continue;
            }
            if (prop.Value.ValueKind == JsonValueKind.Object)
                Flatten(prop.Value, prop.Name, rows);
            else
                rows.Add(new KeyValuePair<string, string>(prop.Name, Scalar(prop.Value)));
        }
    }

    private static string Scalar(JsonElement value) => value.ValueKind switch
    {
        JsonValueKind.Array => $"[{value.GetArrayLength()} items]",
        JsonValueKind.Null => "",
        _ => value.ToString(),
    };

    private static void Flatten(JsonElement obj, string prefix, List<KeyValuePair<string, string>> rows)
    {
        foreach (var prop in obj.EnumerateObject())
        {
            string key = prefix.Length == 0 ? prop.Name : prefix + "." + prop.Name;
            switch (prop.Value.ValueKind)
            {
                case JsonValueKind.Object:
                    Flatten(prop.Value, key, rows);
                    break;
                case JsonValueKind.Array:
                    rows.Add(new KeyValuePair<string, string>(key, $"[{prop.Value.GetArrayLength()} items]"));
                    break;
                case JsonValueKind.Null:
                    rows.Add(new KeyValuePair<string, string>(key, ""));
                    break;
                default:
                    rows.Add(new KeyValuePair<string, string>(key, prop.Value.ToString()));
                    break;
            }
        }
    }

    private static bool LooksLikeBase64(string? s)
    {
        if (string.IsNullOrWhiteSpace(s) || s.Length < 64) return false;
        if (s.Contains(',')) s = s[(s.IndexOf(',') + 1)..]; // tolerate a data URI prefix
        if (s.Length < 64) return false;
        foreach (char c in s)
        {
            if (char.IsWhiteSpace(c) || c == '=' || c == '+' || c == '/' || char.IsLetterOrDigit(c))
                continue;
            return false;
        }
        return true;
    }
}
