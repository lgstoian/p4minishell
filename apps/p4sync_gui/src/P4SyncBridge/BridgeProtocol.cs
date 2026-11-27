// SPDX-FileCopyrightText: 2026 Stoian Alexandru
// SPDX-License-Identifier: MIT
// @module p4sync_gui/bridge-protocol
// @layer leaf
// @domain P4Sync desktop client
// @provides JSON-lines request/response codec shared by GUI and tests
// @status prototype
// @test tests/P4SyncBridge.Tests
// @hard-rule Never invent payload fields; unknown JSON is surfaced verbatim.
using System.Text;
using System.Text.Json;

namespace P4SyncBridge;

/// <summary>Which kind of JSON-lines frame arrived from the bridge.</summary>
public enum BridgeMessageKind
{
    Response,
    Progress,
    Log,
    Unknown,
}

/// <summary>
/// One decoded bridge frame. <see cref="RawJson"/> is always preserved so the
/// GUI can show exactly what the bridge said instead of a guessed projection.
/// </summary>
public sealed class BridgeMessage
{
    public required BridgeMessageKind Kind { get; init; }
    public int Id { get; init; }
    public bool Ok { get; init; }
    public string? Error { get; init; }

    /// <summary>Owned clone of the response payload (undefined for events).</summary>
    public JsonElement Data { get; init; }

    public int? Pct { get; init; }
    public string? Message { get; init; }
    public string? Level { get; init; }
    public required string RawJson { get; init; }
    public DateTimeOffset ReceivedAt { get; init; } = DateTimeOffset.Now;
}

/// <summary>Progress snapshot delivered to a specific request id.</summary>
public sealed record BridgeProgress(int Pct, string Message);

/// <summary>
/// Pure codec for the bridge contract. No process, serial or framing logic
/// lives here: it only reads and writes JSON lines.
/// </summary>
public static class BridgeProtocol
{
    /// <summary>Parse one line. Returns false only for non-JSON/non-object lines.</summary>
    public static bool TryParse(string line, out BridgeMessage message, out string error)
    {
        message = null!;
        error = "";
        if (string.IsNullOrWhiteSpace(line))
        {
            error = "empty line";
            return false;
        }

        JsonDocument doc;
        try
        {
            doc = JsonDocument.Parse(line);
        }
        catch (JsonException ex)
        {
            error = ex.Message;
            return false;
        }

        using (doc)
        {
            var root = doc.RootElement;
            if (root.ValueKind != JsonValueKind.Object)
            {
                error = "bridge frame is not a JSON object";
                return false;
            }

            int id = GetInt(root, "id") ?? 0;
            string raw = line.Trim();

            if (GetString(root, "event") is { } ev &&
                ev.Equals("progress", StringComparison.OrdinalIgnoreCase))
            {
                message = new BridgeMessage
                {
                    Kind = BridgeMessageKind.Progress,
                    Id = id,
                    Pct = GetInt(root, "pct"),
                    Message = GetString(root, "message"),
                    RawJson = raw,
                };
                return true;
            }

            if (GetString(root, "event") is { } logEv &&
                logEv.Equals("log", StringComparison.OrdinalIgnoreCase))
            {
                message = new BridgeMessage
                {
                    Kind = BridgeMessageKind.Log,
                    Id = id,
                    Level = GetString(root, "level"),
                    Message = GetString(root, "message"),
                    RawJson = raw,
                };
                return true;
            }

            if (root.TryGetProperty("ok", out var okProp) &&
                (okProp.ValueKind == JsonValueKind.True || okProp.ValueKind == JsonValueKind.False))
            {
                JsonElement data = default;
                if (root.TryGetProperty("data", out var dataProp))
                    data = dataProp.Clone();

                message = new BridgeMessage
                {
                    Kind = BridgeMessageKind.Response,
                    Id = id,
                    Ok = okProp.GetBoolean(),
                    Error = GetString(root, "error"),
                    Data = data,
                    RawJson = raw,
                };
                return true;
            }

            // Valid JSON but not part of the understood contract: keep it visible.
            message = new BridgeMessage
            {
                Kind = BridgeMessageKind.Unknown,
                Id = id,
                Message = GetString(root, "message"),
                RawJson = raw,
            };
            return true;
        }
    }

    /// <summary>Serialise a request line: {"id":N,"cmd":"...","...args"}.</summary>
    public static string SerializeRequest(int id, string cmd, IReadOnlyDictionary<string, object?>? args)
    {
        using var stream = new MemoryStream();
        using (var writer = new Utf8JsonWriter(stream))
        {
            writer.WriteStartObject();
            writer.WriteNumber("id", id);
            writer.WriteString("cmd", cmd);
            if (args is not null)
            {
                foreach (var pair in args)
                {
                    if (pair.Value is null)
                        continue;
                    writer.WritePropertyName(pair.Key);
                    WriteValue(writer, pair.Value);
                }
            }
            writer.WriteEndObject();
        }
        return Encoding.UTF8.GetString(stream.ToArray());
    }

    /// <summary>Indented JSON for read-only "raw payload" panes.</summary>
    public static string Pretty(JsonElement element)
    {
        if (element.ValueKind is JsonValueKind.Undefined or JsonValueKind.Null)
            return "(no data payload)";
        return JsonSerializer.Serialize(element, new JsonSerializerOptions { WriteIndented = true });
    }

    private static void WriteValue(Utf8JsonWriter writer, object value)
    {
        switch (value)
        {
            case JsonElement json:
                json.WriteTo(writer);
                break;
            case string s:
                writer.WriteStringValue(s);
                break;
            case bool b:
                writer.WriteBooleanValue(b);
                break;
            case int i:
                writer.WriteNumberValue(i);
                break;
            case long l:
                writer.WriteNumberValue(l);
                break;
            case double d:
                writer.WriteNumberValue(d);
                break;
            case float f:
                writer.WriteNumberValue(f);
                break;
            case decimal m:
                writer.WriteNumberValue(m);
                break;
            case IDictionary<string, object?> map:
                writer.WriteStartObject();
                foreach (var pair in map)
                {
                    if (pair.Value is null) continue;
                    writer.WritePropertyName(pair.Key);
                    WriteValue(writer, pair.Value);
                }
                writer.WriteEndObject();
                break;
            case System.Collections.IEnumerable list:
                writer.WriteStartArray();
                foreach (var item in list)
                {
                    if (item is null) continue;
                    WriteValue(writer, item);
                }
                writer.WriteEndArray();
                break;
            default:
                // Last resort: a string. We never guess a shape for unknown types.
                writer.WriteStringValue(value.ToString());
                break;
        }
    }

    private static string? GetString(JsonElement obj, string name)
    {
        if (obj.TryGetProperty(name, out var v) && v.ValueKind == JsonValueKind.String)
            return v.GetString();
        foreach (var prop in obj.EnumerateObject())
        {
            if (prop.Name.Equals(name, StringComparison.OrdinalIgnoreCase) &&
                prop.Value.ValueKind == JsonValueKind.String)
                return prop.Value.GetString();
        }
        return null;
    }

    private static int? GetInt(JsonElement obj, string name)
    {
        if (obj.TryGetProperty(name, out var v) &&
            v.ValueKind == JsonValueKind.Number && v.TryGetInt32(out int exact))
            return exact;
        foreach (var prop in obj.EnumerateObject())
        {
            if (prop.Name.Equals(name, StringComparison.OrdinalIgnoreCase) &&
                prop.Value.ValueKind == JsonValueKind.Number &&
                prop.Value.TryGetInt32(out int n))
                return n;
        }
        return null;
    }
}
