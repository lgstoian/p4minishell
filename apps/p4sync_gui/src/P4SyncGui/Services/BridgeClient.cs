// SPDX-FileCopyrightText: 2026 Stoian Alexandru
// SPDX-License-Identifier: MIT
// @module p4sync_gui/bridge-client
// @layer app
// @domain P4Sync desktop client
// @provides JSON-lines bridge process lifecycle, id correlation, events
// @status prototype
// @test README.md
// @hard-rule C# never opens a serial port or parses SDFX/BMPX/PIMX; bridge.py owns that.
using System.Collections.Concurrent;
using System.Diagnostics;
using System.IO;
using System.Text;
using P4SyncBridge;

namespace P4SyncGui.Services;

/// <summary>
/// Owns the `python tools/p4sync/bridge.py [--mock]` child process. Every
/// command is a JSON line on stdin; every reply/event is a JSON line on stdout.
/// </summary>
public sealed class BridgeClient : IDisposable
{
    private sealed record Pending(
        TaskCompletionSource<BridgeMessage> Tcs,
        IProgress<BridgeProgress>? Progress);

    private readonly SettingsService _settings;
    private readonly CredentialService _credentials;
    private readonly ConcurrentDictionary<int, Pending> _pending = new();
    private readonly SemaphoreSlim _startGate = new(1, 1);
    private readonly SemaphoreSlim _writeGate = new(1, 1);
    private Process? _process;
    private int _nextId;
    private bool _disposed;
    private CancellationTokenSource _readCts = new();

    public event Action<BridgeMessage>? MessageReceived;
    public event Action? ConnectionChanged;

    public bool IsMock => _settings.MockMode;

    public bool IsConnected
    {
        get
        {
            try { return _process is not null && !_process.HasExited; }
            catch { return false; }
        }
    }

    public string BridgeScript => _settings.ResolveBridgeScript();

    public BridgeClient(SettingsService settings, CredentialService credentials)
    {
        _settings = settings;
        _credentials = credentials;
    }

    /// <summary>Send one request and await only the response with the same id.</summary>
    public async Task<BridgeMessage> SendAsync(
        string cmd,
        IReadOnlyDictionary<string, object?>? args,
        IProgress<BridgeProgress>? progress,
        CancellationToken cancellationToken)
    {
        await EnsureStartedAsync(cancellationToken).ConfigureAwait(false);

        int id = Interlocked.Increment(ref _nextId);
        var tcs = new TaskCompletionSource<BridgeMessage>(TaskCreationOptions.RunContinuationsAsynchronously);
        var pending = new Pending(tcs, progress);
        _pending[id] = pending;

        using var registration = cancellationToken.Register(() =>
        {
            if (_pending.TryRemove(id, out var cancelled))
                cancelled.Tcs.TrySetCanceled(cancellationToken);
        });

        try
        {
            string line = BridgeProtocol.SerializeRequest(id, cmd, args);
            await WriteLineAsync(line, cancellationToken).ConfigureAwait(false);
            return await tcs.Task.WaitAsync(cancellationToken).ConfigureAwait(false);
        }
        finally
        {
            _pending.TryRemove(id, out _);
        }
    }

    /// <summary>Cancel means stop waiting and recycle the bridge process.</summary>
    public void Kill()
    {
        var process = _process;
        _process = null;
        if (process is not null)
        {
            try { if (!process.HasExited) process.Kill(entireProcessTree: true); } catch { }
            try { process.Dispose(); } catch { }
        }
        FailAll(new OperationCanceledException("bridge restarted"));
        ConnectionChanged?.Invoke();
    }

    private async Task EnsureStartedAsync(CancellationToken cancellationToken)
    {
        if (IsConnected) return;
        await _startGate.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            if (IsConnected) return;

            string script = _settings.ResolveBridgeScript();
            if (!File.Exists(script))
            {
                throw new FileNotFoundException(
                    "Bridge script not found: " + script + ". Set its path in Settings.", script);
            }

            var start = new ProcessStartInfo
            {
                FileName = string.IsNullOrWhiteSpace(_settings.PythonPath) ? "python" : _settings.PythonPath,
                UseShellExecute = false,
                CreateNoWindow = true,
                RedirectStandardInput = true,
                RedirectStandardOutput = true,
                RedirectStandardError = true,
                StandardOutputEncoding = new UTF8Encoding(false),
                StandardErrorEncoding = new UTF8Encoding(false),
            };
            start.ArgumentList.Add(script);
            if (_settings.MockMode) start.ArgumentList.Add("--mock");
            start.Environment["PYTHONIOENCODING"] = "utf-8";
            start.Environment["PYTHONUNBUFFERED"] = "1";

            // Secrets are passed only as child-process environment variables.
            // Never logged, never written to disk by this app.
            try
            {
                var credential = _credentials.Load(_settings.CredentialTarget);
                if (credential is not null)
                {
                    start.Environment["P4SYNC_CRED_USER"] = credential.UserName;
                    start.Environment["P4SYNC_CRED_PASSWORD"] = credential.Password;
                }
            }
            catch
            {
                // A broken credential store must not prevent an offline demo.
            }

            var process = new Process { StartInfo = start, EnableRaisingEvents = true };
            process.Exited += (_, _) =>
            {
                FailAll(new IOException("bridge process exited"));
                ConnectionChanged?.Invoke();
            };

            if (!process.Start())
                throw new InvalidOperationException("Failed to start the P4Sync bridge.");

            _process = process;
            _readCts.Dispose();
            _readCts = new CancellationTokenSource();
            var token = _readCts.Token;
            _ = Task.Run(() => ReadLoopAsync(process.StandardOutput, token), token);
            _ = Task.Run(() => ReadStdErrorAsync(process.StandardError, token), token);
            ConnectionChanged?.Invoke();
        }
        finally
        {
            _startGate.Release();
        }
    }

    private async Task WriteLineAsync(string line, CancellationToken cancellationToken)
    {
        await _writeGate.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            var process = _process ?? throw new InvalidOperationException("Bridge is not running.");
            if (process.HasExited) throw new IOException("Bridge exited before the request was sent.");
            await process.StandardInput.WriteLineAsync(line.AsMemory(), cancellationToken).ConfigureAwait(false);
            await process.StandardInput.FlushAsync(cancellationToken).ConfigureAwait(false);
        }
        finally
        {
            _writeGate.Release();
        }
    }

    private async Task ReadLoopAsync(StreamReader reader, CancellationToken cancellationToken)
    {
        try
        {
            string? line;
            while ((line = await reader.ReadLineAsync(cancellationToken).ConfigureAwait(false)) is not null)
            {
                if (BridgeProtocol.TryParse(line, out var message, out string error))
                {
                    Dispatch(message);
                }
                else
                {
                    MessageReceived?.Invoke(new BridgeMessage
                    {
                        Kind = BridgeMessageKind.Unknown,
                        Message = "[invalid bridge JSON] " + error,
                        RawJson = line,
                    });
                }
            }
        }
        catch (OperationCanceledException) { }
        catch (Exception ex)
        {
            FailAll(ex);
        }
        finally
        {
            FailAll(new IOException("Bridge output stream closed."));
            ConnectionChanged?.Invoke();
        }
    }

    private async Task ReadStdErrorAsync(StreamReader reader, CancellationToken cancellationToken)
    {
        try
        {
            string? line;
            while ((line = await reader.ReadLineAsync(cancellationToken).ConfigureAwait(false)) is not null)
            {
                // stderr is diagnostic text, not JSON. Keep it verbatim and labelled.
                MessageReceived?.Invoke(new BridgeMessage
                {
                    Kind = BridgeMessageKind.Log,
                    Level = "warn",
                    Message = "[bridge stderr] " + line,
                    RawJson = line,
                });
            }
        }
        catch (OperationCanceledException) { }
        catch { /* process teardown */ }
    }

    private void Dispatch(BridgeMessage message)
    {
        if (message.Kind == BridgeMessageKind.Response &&
            _pending.TryGetValue(message.Id, out var pending))
        {
            pending.Tcs.TrySetResult(message);
        }
        else if (message.Kind == BridgeMessageKind.Progress &&
                 _pending.TryGetValue(message.Id, out var active))
        {
            active.Progress?.Report(new BridgeProgress(message.Pct ?? 0, message.Message ?? ""));
        }
        MessageReceived?.Invoke(message);
    }

    private void FailAll(Exception error)
    {
        foreach (var id in _pending.Keys)
        {
            if (_pending.TryRemove(id, out var pending))
                pending.Tcs.TrySetException(error);
        }
    }

    public void Dispose()
    {
        if (_disposed) return;
        _disposed = true;
        Kill();
        _readCts.Cancel();
        _readCts.Dispose();
        _startGate.Dispose();
        _writeGate.Dispose();
    }
}
