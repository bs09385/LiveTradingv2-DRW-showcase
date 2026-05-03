#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "common/error.h"
#include "ui_bridge/watcher_types.h"

namespace lt {

// Discovered market from Polymarket gamma API response.
struct DiscoveredMarket {
    std::string condition_id;
    std::string token_id_up;
    std::string token_id_down;
    bool is_closed = false;
    bool neg_risk = false;
    uint16_t fee_rate_bps = 0;
};

// Return the URL slug for a BTC timeframe (e.g. "5m", "15m").
const char* timeframe_slug(BtcTimeframe tf);

// Return the period in seconds for a BTC timeframe (e.g. 300 for 5m).
int64_t timeframe_period_seconds(BtcTimeframe tf);

// Extract hostname from an HTTPS URL (e.g. "https://gamma-api.polymarket.com/..." -> "gamma-api.polymarket.com").
std::string extract_host(const std::string& url);

// Synchronous HTTPS GET — blocking, suitable for non-hot-path threads only.
// Returns response body on HTTP 200, empty string on any failure.
// Logs HTTP status and error details on failure.
std::string sync_https_get(const std::string& host, const std::string& target,
                           int timeout_ms = 10000);

// Synchronous HTTPS POST — blocking, suitable for non-hot-path threads only.
// Sends body with application/json content type.
// Returns response body on HTTP 200, empty string on any failure.
std::string sync_https_post(const std::string& host, const std::string& target,
                            const std::string& body, int timeout_ms = 10000);

// Extract path component from an HTTPS URL.
// e.g. "https://rpc.ankr.com/polygon" -> "/polygon", "https://foo.com" -> "/"
std::string extract_path(const std::string& url);

// Parse gamma-api.polymarket.com /events response for a BTC Up/Down market.
// Response format: [{..., "markets": [{"conditionId":"...", "clobTokenIds":[...], "outcomes":["Up","Down"], "closed":false}]}]
std::optional<DiscoveredMarket> parse_gamma_response(const std::string& body);

// Return candidate current window starts for a timeframe at unix_ts.
// BTC 4H includes both UTC-aligned and +1h aligned windows to handle EST/EDT anchoring.
std::vector<int64_t> discovery_window_candidates(BtcTimeframe tf, int64_t unix_ts);

// Discover a BTC market for a specific timeframe/window by trying supported slug formats.
// For 1H this includes the human-readable "bitcoin-up-or-down-...-et" slug variant.
std::optional<DiscoveredMarket> discover_btc_market_for_window(
    const std::string& host,
    BtcTimeframe tf,
    int64_t window_start_unix_s,
    int timeout_ms = 10000);

// Query CLOB API for negRisk status of a token.
// Returns Expected<bool> — NETWORK_ERROR on failure so callers can distinguish
// "API said false" from "request failed".
Expected<bool> query_neg_risk(const std::string& token_id);

// Query CLOB API for the fee rate (basis points) for a token.
// Returns Expected<uint16_t> — NETWORK_ERROR on failure so callers can distinguish
// "API said 0" from "request failed".
Expected<uint16_t> query_fee_rate(const std::string& token_id);

}  // namespace lt
