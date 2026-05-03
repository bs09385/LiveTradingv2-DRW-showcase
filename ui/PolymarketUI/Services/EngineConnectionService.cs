using System.IO;
using System.Net.WebSockets;
using System.Text;
using System.Text.Json;
using System.Threading.Channels;
using PolymarketUI.Models;

namespace PolymarketUI.Services;

public class EngineConnectionService : IDisposable
{
    private ClientWebSocket? _ws;
    private CancellationTokenSource _cts = new();
    private readonly string _uri;
    private readonly string _authToken;
    private bool _disposed;
    private Task? _receiveTask;
    private Task? _deserializeTask;

    // Bounded channel decouples WS receive from JSON deserialization.
    // Capacity 4: only latest messages matter, older ones are superseded.
    private Channel<string> _messageChannel =
        Channel.CreateBounded<string>(new BoundedChannelOptions(4)
        {
            FullMode = BoundedChannelFullMode.DropOldest,
            SingleWriter = true,
            SingleReader = true
        });

    public event Action<EngineSnapshot>? SnapshotReceived;
    public event Action<bool>? ConnectionChanged;
    public event Action<string>? ErrorOccurred;

    /// <summary>
    /// Message router for watcher-related messages (series_list, watcher_books, watcher_status).
    /// </summary>
    public WatcherMessageRouter WatcherRouter { get; } = new();

    public bool IsConnected => _ws?.State == WebSocketState.Open;

    public EngineConnectionService(string uri = "ws://localhost:9090", string authToken = "")
    {
        _uri = uri;
        _authToken = authToken;
    }

    public async Task ConnectAsync()
    {
        _cts = new CancellationTokenSource();
        _ws = new ClientWebSocket();
        _messageChannel = Channel.CreateBounded<string>(new BoundedChannelOptions(4)
        {
            FullMode = BoundedChannelFullMode.DropOldest,
            SingleWriter = true,
            SingleReader = true
        });

        try
        {
            var connectUri = string.IsNullOrEmpty(_authToken)
                ? _uri
                : (_uri.Contains("?") ? $"{_uri}&token={_authToken}" : $"{_uri}?token={_authToken}");
            await _ws.ConnectAsync(new Uri(connectUri), _cts.Token);
            ConnectionChanged?.Invoke(true);
            _receiveTask = Task.Run(() => ReceiveLoopAsync(_cts.Token));
            _deserializeTask = Task.Run(() => DeserializeLoopAsync(_cts.Token));
        }
        catch (Exception ex)
        {
            ErrorOccurred?.Invoke($"Connect failed: {ex.Message}");
            ConnectionChanged?.Invoke(false);
        }
    }

    public async Task DisconnectAsync()
    {
        if (_disposed) return;
        try { _cts.Cancel(); } catch (ObjectDisposedException) { }
        if (_ws?.State == WebSocketState.Open)
        {
            try
            {
                await _ws.CloseAsync(WebSocketCloseStatus.NormalClosure, "UI closing",
                    CancellationToken.None);
            }
            catch { }
        }
        ConnectionChanged?.Invoke(false);
    }

    public async Task SendCommandAsync(string json)
    {
        if (_ws?.State != WebSocketState.Open) return;

        var bytes = Encoding.UTF8.GetBytes(json);
        try
        {
            await _ws.SendAsync(new ArraySegment<byte>(bytes),
                WebSocketMessageType.Text, true, _cts.Token);
        }
        catch (Exception ex)
        {
            ErrorOccurred?.Invoke($"Send failed: {ex.Message}");
        }
    }

    /// <summary>
    /// Receive loop: reads WS frames as fast as possible, pushes raw JSON
    /// strings into the channel. No deserialization here — keeps the TCP
    /// socket drained to prevent backpressure.
    /// </summary>
    private async Task ReceiveLoopAsync(CancellationToken ct)
    {
        var buffer = new ArraySegment<byte>(new byte[65536]);

        while (!ct.IsCancellationRequested && _ws?.State == WebSocketState.Open)
        {
            try
            {
                // Accumulate fragments until EndOfMessage
                using var ms = new MemoryStream();
                WebSocketReceiveResult result;
                do
                {
                    result = await _ws.ReceiveAsync(buffer, ct);
                    if (result.MessageType == WebSocketMessageType.Close) break;
                    ms.Write(buffer.Array!, buffer.Offset, result.Count);
                } while (!result.EndOfMessage);

                if (result.MessageType == WebSocketMessageType.Close)
                {
                    ConnectionChanged?.Invoke(false);
                    break;
                }

                if (result.MessageType == WebSocketMessageType.Text)
                {
                    var json = Encoding.UTF8.GetString(ms.GetBuffer(), 0, (int)ms.Length);
                    // Non-blocking write; drops oldest if channel is full
                    _messageChannel.Writer.TryWrite(json);
                }
            }
            catch (OperationCanceledException)
            {
                break;
            }
            catch (WebSocketException)
            {
                ConnectionChanged?.Invoke(false);
                break;
            }
            catch (Exception ex)
            {
                ErrorOccurred?.Invoke($"Receive error: {ex.Message}");
            }
        }

        // Signal deserialize loop that no more messages are coming
        _messageChannel.Writer.TryComplete();

        // Auto-reconnect loop
        if (!ct.IsCancellationRequested)
        {
            ConnectionChanged?.Invoke(false);
            await Task.Delay(2000, ct).ContinueWith(_ => { });
            if (!ct.IsCancellationRequested)
            {
                await ConnectAsync();
            }
        }
    }

    /// <summary>
    /// Deserialization loop: runs on a background thread, reads raw JSON
    /// from the channel and deserializes. Heavy JSON parsing happens here
    /// instead of blocking the receive loop.
    /// </summary>
    private async Task DeserializeLoopAsync(CancellationToken ct)
    {
        try
        {
            await foreach (var json in _messageChannel.Reader.ReadAllAsync(ct))
            {
                try
                {
                    // Try routing as a watcher message first
                    if (!WatcherRouter.TryRoute(json))
                    {
                        // Default: parse as engine snapshot
                        var snapshot = JsonSerializer.Deserialize<EngineSnapshot>(json);
                        if (snapshot != null)
                        {
                            SnapshotReceived?.Invoke(snapshot);
                        }
                    }
                }
                catch (Exception ex)
                {
                    ErrorOccurred?.Invoke($"Deserialize error: {ex.Message}");
                }
            }
        }
        catch (OperationCanceledException) { }
    }

    public void Dispose()
    {
        if (_disposed) return;
        _disposed = true;
        _cts.Cancel();
        _messageChannel.Writer.TryComplete();
        _ws?.Dispose();
        _cts.Dispose();
    }
}
