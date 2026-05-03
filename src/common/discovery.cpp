// Boost.Asio includes Windows headers that #define ERROR.
#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/asio/ssl.hpp>

#ifdef ERROR
#undef ERROR
#endif

#include "common/discovery.h"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "simdjson.h"

namespace lt {

namespace {

constexpr const char* kMonthNames[] = {
    "january", "february", "march", "april", "may", "june",
    "july", "august", "september", "october", "november", "december"
};

bool gmtime_utc(int64_t unix_ts, std::tm& out) {
    std::time_t t = static_cast<std::time_t>(unix_ts);
#ifdef _WIN32
    return gmtime_s(&out, &t) == 0;
#else
    return gmtime_r(&t, &out) != nullptr;
#endif
}

// days since 1970-01-01 (Howard Hinnant civil calendar algorithm)
int64_t days_from_civil(int year, unsigned month, unsigned day) {
    year -= month <= 2;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(year - era * 400);
    const unsigned doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return static_cast<int64_t>(era) * 146097 + static_cast<int64_t>(doe) - 719468;
}

int64_t unix_from_utc(int year, int month, int day, int hour, int minute, int second) {
    int64_t days = days_from_civil(year, static_cast<unsigned>(month), static_cast<unsigned>(day));
    return days * 86400 + static_cast<int64_t>(hour) * 3600 +
           static_cast<int64_t>(minute) * 60 + second;
}

// 0=Sunday, 1=Monday, ..., 6=Saturday
int day_of_week(int year, int month, int day) {
    int64_t days = days_from_civil(year, static_cast<unsigned>(month), static_cast<unsigned>(day));
    int dow = static_cast<int>((days + 4) % 7);  // 1970-01-01 was Thursday
    if (dow < 0) dow += 7;
    return dow;
}

int nth_sunday_of_month(int year, int month, int nth) {
    int dow_first = day_of_week(year, month, 1);
    int first_sunday = 1 + ((7 - dow_first) % 7);
    return first_sunday + (nth - 1) * 7;
}

bool is_us_eastern_dst(int64_t unix_ts) {
    std::tm utc{};
    if (!gmtime_utc(unix_ts, utc)) return false;
    int year = utc.tm_year + 1900;

    int march_second_sunday = nth_sunday_of_month(year, 3, 2);
    int november_first_sunday = nth_sunday_of_month(year, 11, 1);

    // US Eastern DST transition instants in UTC:
    // start: second Sunday in March, 02:00 local standard = 07:00 UTC
    // end: first Sunday in November, 02:00 local daylight = 06:00 UTC
    int64_t dst_start_utc = unix_from_utc(year, 3, march_second_sunday, 7, 0, 0);
    int64_t dst_end_utc = unix_from_utc(year, 11, november_first_sunday, 6, 0, 0);
    return unix_ts >= dst_start_utc && unix_ts < dst_end_utc;
}

int64_t us_eastern_utc_offset_seconds(int64_t unix_ts) {
    return is_us_eastern_dst(unix_ts) ? -4 * 3600 : -5 * 3600;
}

int64_t align_window_with_offset(int64_t unix_ts, int64_t period, int64_t offset_s) {
    int64_t shifted = unix_ts - offset_s;
    int64_t aligned = (shifted / period) * period + offset_s;
    if (aligned > unix_ts) aligned -= period;
    return aligned;
}

std::string build_btc_1h_human_slug(int64_t window_start_unix_s) {
    int64_t local_ts = window_start_unix_s + us_eastern_utc_offset_seconds(window_start_unix_s);
    std::tm local_tm{};
    if (!gmtime_utc(local_ts, local_tm)) return "";

    int mon = local_tm.tm_mon;
    if (mon < 0 || mon >= 12) return "";

    int day = local_tm.tm_mday;
    int hour24 = local_tm.tm_hour;
    int hour12 = hour24 % 12;
    if (hour12 == 0) hour12 = 12;
    const char* ampm = hour24 < 12 ? "am" : "pm";

    char slug[128];
    std::snprintf(slug, sizeof(slug), "bitcoin-up-or-down-%s-%d-%d%s-et",
                  kMonthNames[mon], day, hour12, ampm);
    return slug;
}

std::vector<std::string> build_discovery_targets(BtcTimeframe tf, int64_t window_start_unix_s) {
    std::vector<std::string> targets;

    char numeric_target[256];
    std::snprintf(numeric_target, sizeof(numeric_target), "/events?slug=btc-updown-%s-%lld",
                  timeframe_slug(tf), static_cast<long long>(window_start_unix_s));
    targets.emplace_back(numeric_target);

    return targets;
}

}  // namespace

const char* timeframe_slug(BtcTimeframe tf) {
    switch (tf) {
        case BtcTimeframe::BTC_5M:  return "5m";
        case BtcTimeframe::BTC_15M: return "15m";
    }
    return "5m";
}

int64_t timeframe_period_seconds(BtcTimeframe tf) {
    switch (tf) {
        case BtcTimeframe::BTC_5M:  return 300;
        case BtcTimeframe::BTC_15M: return 900;
    }
    return 300;
}

std::string extract_host(const std::string& url) {
    auto pos = url.find("://");
    if (pos == std::string::npos) return url;
    auto start = pos + 3;
    auto end = url.find('/', start);
    if (end == std::string::npos) return url.substr(start);
    return url.substr(start, end - start);
}

std::string sync_https_get(const std::string& host, const std::string& target,
                           int timeout_ms) {
    namespace beast = boost::beast;
    namespace http = beast::http;
    namespace net = boost::asio;
    namespace ssl = boost::asio::ssl;
    using tcp = net::ip::tcp;

    try {
        net::io_context ioc;
        ssl::context ctx{ssl::context::tlsv12_client};
        ctx.set_verify_mode(ssl::verify_none);  // Public read-only API

        tcp::resolver resolver{ioc};
        beast::ssl_stream<beast::tcp_stream> stream{ioc, ctx};

        if (!SSL_set_tlsext_host_name(stream.native_handle(), host.c_str())) {
            std::fprintf(stderr, "discovery: SNI failed for %s\n", host.c_str());
            return "";
        }

        auto results = resolver.resolve(host, "443");
        beast::get_lowest_layer(stream).expires_after(std::chrono::milliseconds(timeout_ms));
        beast::get_lowest_layer(stream).connect(results);

        beast::get_lowest_layer(stream).expires_after(std::chrono::milliseconds(timeout_ms));
        stream.handshake(ssl::stream_base::client);

        http::request<http::string_body> req{http::verb::get, target, 11};
        req.set(http::field::host, host);
        req.set(http::field::user_agent, "LiveTradingv2/1.0");
        req.set(http::field::accept, "application/json");
        req.set(http::field::connection, "close");

        beast::get_lowest_layer(stream).expires_after(std::chrono::milliseconds(timeout_ms));
        http::write(stream, req);

        beast::flat_buffer buffer;
        buffer.reserve(1048576);  // 1MB max response body size
        http::response_parser<http::string_body> parser;
        parser.body_limit(1048576);  // 1MB limit

        beast::get_lowest_layer(stream).expires_after(std::chrono::milliseconds(timeout_ms));
        http::read(stream, buffer, parser);

        auto& res = parser.get();

        // Graceful TLS shutdown (ignore errors — common with TLS close)
        beast::error_code ec;
        stream.shutdown(ec);

        unsigned status = res.result_int();
        if (status != 200) {
            std::fprintf(stderr, "discovery: GET %s%s http=%u body=%.200s\n",
                         host.c_str(), target.c_str(), status,
                         res.body().c_str());
            return "";
        }
        return std::move(res.body());
    } catch (const std::exception& e) {
        std::fprintf(stderr, "discovery: GET %s%s failed: %s\n",
                     host.c_str(), target.c_str(), e.what());
        return "";
    } catch (...) {
        std::fprintf(stderr, "discovery: GET %s%s failed: unknown exception\n",
                     host.c_str(), target.c_str());
        return "";
    }
}

std::string sync_https_post(const std::string& host, const std::string& target,
                            const std::string& body, int timeout_ms) {
    namespace beast = boost::beast;
    namespace http = beast::http;
    namespace net = boost::asio;
    namespace ssl = boost::asio::ssl;
    using tcp = net::ip::tcp;

    try {
        net::io_context ioc;
        ssl::context ctx{ssl::context::tlsv12_client};
        ctx.set_verify_mode(ssl::verify_none);

        tcp::resolver resolver{ioc};
        beast::ssl_stream<beast::tcp_stream> stream{ioc, ctx};

        if (!SSL_set_tlsext_host_name(stream.native_handle(), host.c_str())) {
            std::fprintf(stderr, "discovery: POST SNI failed for %s\n", host.c_str());
            return "";
        }

        auto results = resolver.resolve(host, "443");
        beast::get_lowest_layer(stream).expires_after(std::chrono::milliseconds(timeout_ms));
        beast::get_lowest_layer(stream).connect(results);

        beast::get_lowest_layer(stream).expires_after(std::chrono::milliseconds(timeout_ms));
        stream.handshake(ssl::stream_base::client);

        http::request<http::string_body> req{http::verb::post, target, 11};
        req.set(http::field::host, host);
        req.set(http::field::user_agent, "LiveTradingv2/1.0");
        req.set(http::field::content_type, "application/json");
        req.set(http::field::accept, "application/json");
        req.set(http::field::connection, "close");
        req.body() = body;
        req.prepare_payload();

        beast::get_lowest_layer(stream).expires_after(std::chrono::milliseconds(timeout_ms));
        http::write(stream, req);

        beast::flat_buffer buffer;
        buffer.reserve(65536);
        http::response_parser<http::string_body> parser;
        parser.body_limit(65536);

        beast::get_lowest_layer(stream).expires_after(std::chrono::milliseconds(timeout_ms));
        http::read(stream, buffer, parser);

        auto& res = parser.get();

        beast::error_code ec;
        stream.shutdown(ec);

        unsigned status = res.result_int();
        if (status != 200) {
            std::fprintf(stderr, "discovery: POST %s%s http=%u body=%.200s\n",
                         host.c_str(), target.c_str(), status,
                         res.body().c_str());
            return "";
        }
        return std::move(res.body());
    } catch (const std::exception& e) {
        std::fprintf(stderr, "discovery: POST %s%s failed: %s\n",
                     host.c_str(), target.c_str(), e.what());
        return "";
    } catch (...) {
        std::fprintf(stderr, "discovery: POST %s%s failed: unknown exception\n",
                     host.c_str(), target.c_str());
        return "";
    }
}

std::string extract_path(const std::string& url) {
    auto pos = url.find("://");
    if (pos == std::string::npos) return "/";
    auto slash = url.find('/', pos + 3);
    if (slash == std::string::npos) return "/";
    return url.substr(slash);
}

std::optional<DiscoveredMarket> parse_gamma_response(const std::string& body) {
    if (body.empty() || body == "[]") return std::nullopt;

    try {
        simdjson::dom::parser parser;
        auto doc_result = parser.parse(body);
        if (doc_result.error()) return std::nullopt;
        auto doc = doc_result.value();

        simdjson::dom::array events;
        if (doc.get_array().get(events)) return std::nullopt;

        for (auto event : events) {
            simdjson::dom::array markets;
            if (event["markets"].get_array().get(markets)) continue;

            for (auto market : markets) {
                DiscoveredMarket result;

                // conditionId
                std::string_view cid;
                if (market["conditionId"].get_string().get(cid)) continue;
                result.condition_id = std::string(cid);

                // closed
                bool closed_val = false;
                auto closed_err = market["closed"].get_bool().get(closed_val);
                (void)closed_err;
                result.is_closed = closed_val;

                // Determine Up/Down index from outcomes array
                int up_idx = 0, down_idx = 1;
                simdjson::dom::array outcomes;
                if (!market["outcomes"].get_array().get(outcomes)) {
                    int idx = 0;
                    for (auto outcome : outcomes) {
                        std::string_view s;
                        if (!outcome.get_string().get(s)) {
                            if (s == "Up") up_idx = idx;
                            else if (s == "Down") down_idx = idx;
                        }
                        idx++;
                    }
                }

                // Parse clobTokenIds — handle both native JSON array and JSON-encoded string
                std::vector<std::string> token_ids;
                simdjson::dom::array tokens;
                if (!market["clobTokenIds"].get_array().get(tokens)) {
                    for (auto tok : tokens) {
                        std::string_view s;
                        if (!tok.get_string().get(s)) {
                            token_ids.emplace_back(s);
                        }
                    }
                } else {
                    // clobTokenIds might be a JSON-encoded string: "[\"tok1\",\"tok2\"]"
                    std::string_view tok_str;
                    if (!market["clobTokenIds"].get_string().get(tok_str)) {
                        std::string tok_json(tok_str);
                        simdjson::dom::parser inner_parser;
                        auto inner_result = inner_parser.parse(tok_json);
                        if (!inner_result.error()) {
                            simdjson::dom::array tok_arr;
                            if (!inner_result.value().get_array().get(tok_arr)) {
                                for (auto tok : tok_arr) {
                                    std::string_view s;
                                    if (!tok.get_string().get(s)) {
                                        token_ids.emplace_back(s);
                                    }
                                }
                            }
                        }
                    }
                }

                int max_idx = std::max(up_idx, down_idx);
                if (static_cast<int>(token_ids.size()) > max_idx) {
                    result.token_id_up = token_ids[up_idx];
                    result.token_id_down = token_ids[down_idx];
                    return result;
                }
            }
        }
    } catch (...) {
        // JSON parsing failed
    }
    return std::nullopt;
}

std::vector<int64_t> discovery_window_candidates(BtcTimeframe tf, int64_t unix_ts) {
    int64_t period = timeframe_period_seconds(tf);
    if (period <= 0) return {};

    std::vector<int64_t> windows;
    int64_t start = align_window_with_offset(unix_ts, period, 0);
    windows.push_back(start);

    std::sort(windows.begin(), windows.end(), std::greater<int64_t>());
    return windows;
}

std::optional<DiscoveredMarket> discover_btc_market_for_window(
    const std::string& host,
    BtcTimeframe tf,
    int64_t window_start_unix_s,
    int timeout_ms) {
    auto targets = build_discovery_targets(tf, window_start_unix_s);
    for (const auto& target : targets) {
        auto body = sync_https_get(host, target, timeout_ms);
        auto market = parse_gamma_response(body);
        if (market.has_value()) return market;
    }
    return std::nullopt;
}

Expected<bool> query_neg_risk(const std::string& token_id) {
    std::string target = "/neg-risk?token_id=" + token_id;
    auto body = sync_https_get("clob.polymarket.com", target, 5000);
    if (body.empty()) return ErrorCode::REST_CONNECTION_FAILED;

    // Try JSON format first: {"neg_risk": true/false}
    simdjson::dom::parser parser;
    simdjson::dom::element doc;
    if (parser.parse(body).get(doc) == simdjson::SUCCESS) {
        bool val;
        if (doc["neg_risk"].get_bool().get(val) == simdjson::SUCCESS) {
            return Expected<bool>(val);
        }
    }

    // Fallback: bare "true" or "false" text
    if (body == "true") return Expected<bool>(true);
    if (body == "false") return Expected<bool>(false);

    std::fprintf(stderr, "discovery: /neg-risk unexpected body=%.100s\n", body.c_str());
    return ErrorCode::INVALID_FORMAT;
}

Expected<uint16_t> query_fee_rate(const std::string& token_id) {
    std::string target = "/fee-rate?token_id=" + token_id;
    auto body = sync_https_get("clob.polymarket.com", target, 5000);
    if (body.empty()) return ErrorCode::REST_CONNECTION_FAILED;

    // Response: {"base_fee":1000}
    try {
        simdjson::dom::parser parser;
        auto doc_result = parser.parse(body);
        if (doc_result.error()) {
            std::fprintf(stderr, "discovery: /fee-rate parse error body=%.100s\n", body.c_str());
            return ErrorCode::JSON_PARSE_ERROR;
        }
        int64_t fee = 0;
        if (doc_result.value()["base_fee"].get_int64().get(fee)) {
            std::fprintf(stderr, "discovery: /fee-rate missing base_fee body=%.100s\n", body.c_str());
            return ErrorCode::JSON_MISSING_FIELD;
        }
        if (fee < 0 || fee > 10000) {
            std::fprintf(stderr, "discovery: /fee-rate out of range: %lld\n",
                         static_cast<long long>(fee));
            return ErrorCode::OUT_OF_RANGE;
        }
        return Expected<uint16_t>(static_cast<uint16_t>(fee));
    } catch (...) {
        return ErrorCode::JSON_PARSE_ERROR;
    }
}

}  // namespace lt
