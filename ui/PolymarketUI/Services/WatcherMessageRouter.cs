using System.Text.Json;
using PolymarketUI.Models;

namespace PolymarketUI.Services;

/// <summary>
/// Routes type-tagged messages from the engine to appropriate handlers.
/// Discriminates on the "type" field in the JSON message.
/// </summary>
public class WatcherMessageRouter
{
    public event Action<SeriesListMessage>? SeriesListReceived;
    public event Action<WatcherBooksMessage>? WatcherBooksReceived;
    public event Action<WatcherStatusMessage>? WatcherStatusReceived;

    /// <summary>
    /// Try to route a message based on its type field.
    /// Returns true if it was a watcher message (not engine_snapshot).
    /// Returns false if the message should be handled as an engine_snapshot.
    /// </summary>
    public bool TryRoute(string json)
    {
        try
        {
            using var doc = JsonDocument.Parse(json);
            if (!doc.RootElement.TryGetProperty("type", out var typeElem))
                return false;

            var type = typeElem.GetString();
            switch (type)
            {
                case "series_list":
                    var seriesList = JsonSerializer.Deserialize<SeriesListMessage>(json);
                    if (seriesList != null)
                        SeriesListReceived?.Invoke(seriesList);
                    return true;

                case "watcher_books":
                    var books = JsonSerializer.Deserialize<WatcherBooksMessage>(json);
                    if (books != null)
                        WatcherBooksReceived?.Invoke(books);
                    return true;

                case "watcher_status":
                    var status = JsonSerializer.Deserialize<WatcherStatusMessage>(json);
                    if (status != null)
                        WatcherStatusReceived?.Invoke(status);
                    return true;

                case "engine_snapshot":
                default:
                    return false;
            }
        }
        catch
        {
            return false;
        }
    }
}
