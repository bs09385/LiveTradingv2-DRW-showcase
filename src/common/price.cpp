#include "common/price.h"

#include <algorithm>
#include <charconv>
#include <cstring>
#include <limits>

namespace lt {

Expected<Price_t> parse_price(std::string_view sv) {
    if (sv.empty()) return ErrorCode::EMPTY_INPUT;

    // Handle optional leading sign (shouldn't appear for Polymarket prices, but be safe)
    bool negative = false;
    if (sv[0] == '-') {
        negative = true;
        sv.remove_prefix(1);
        if (sv.empty()) return ErrorCode::INVALID_FORMAT;
    }

    // Parse integer and fractional parts
    int64_t integer_part = 0;
    int64_t frac_part = 0;
    int frac_digits = 0;

    // Find decimal point
    auto dot_pos = sv.find('.');
    if (dot_pos == std::string_view::npos) {
        // Pure integer like "1" or "0"
        auto result = std::from_chars(sv.data(), sv.data() + sv.size(), integer_part);
        if (result.ec != std::errc{} || result.ptr != sv.data() + sv.size()) {
            return ErrorCode::INVALID_FORMAT;
        }
    } else {
        // Has decimal point
        if (dot_pos > 0) {
            auto int_sv = sv.substr(0, dot_pos);
            auto result = std::from_chars(int_sv.data(), int_sv.data() + int_sv.size(), integer_part);
            if (result.ec != std::errc{} || result.ptr != int_sv.data() + int_sv.size()) {
                return ErrorCode::INVALID_FORMAT;
            }
        }
        // else: leading dot like ".48" means integer_part = 0

        auto frac_sv = sv.substr(dot_pos + 1);
        frac_digits = static_cast<int>(frac_sv.size());
        if (frac_digits > 0) {
            auto result = std::from_chars(frac_sv.data(), frac_sv.data() + frac_sv.size(), frac_part);
            if (result.ec != std::errc{} || result.ptr != frac_sv.data() + frac_sv.size()) {
                return ErrorCode::INVALID_FORMAT;
            }
        }
    }

    // Convert to 10000x fixed-point
    // integer_part * 10000 + frac_part * (10000 / 10^frac_digits)
    int64_t price = integer_part * kPriceScale;

    if (frac_digits > 0) {
        // Scale frac_part to 4 decimal places
        if (frac_digits <= 4) {
            int multiplier = 1;
            for (int i = 0; i < 4 - frac_digits; ++i) multiplier *= 10;
            price += frac_part * multiplier;
        } else {
            // More than 4 decimal places - truncate
            int divisor = 1;
            for (int i = 0; i < frac_digits - 4; ++i) divisor *= 10;
            price += frac_part / divisor;
        }
    }

    if (negative) price = -price;

    if (price < kPriceMin || price > kPriceMax) {
        return ErrorCode::OUT_OF_RANGE;
    }

    return Expected<Price_t>(static_cast<Price_t>(price));
}

Expected<Qty_t> parse_qty(std::string_view sv) {
    if (sv.empty()) return ErrorCode::EMPTY_INPUT;

    // Parse integer and fractional parts, scale to kQtyScale (10^6).
    // "30" → 30000000, "219.217767" → 219217767, "150.5" → 150500000
    auto dot_pos = sv.find('.');
    std::string_view int_sv = (dot_pos != std::string_view::npos)
        ? sv.substr(0, dot_pos) : sv;

    int64_t integer_part = 0;
    if (!int_sv.empty()) {
        auto result = std::from_chars(int_sv.data(), int_sv.data() + int_sv.size(), integer_part);
        if (result.ec != std::errc{} || result.ptr != int_sv.data() + int_sv.size()) {
            return ErrorCode::INVALID_FORMAT;
        }
    }
    // else: leading dot like ".5" → integer_part = 0

    int64_t frac_part = 0;
    if (dot_pos != std::string_view::npos) {
        auto frac_sv = sv.substr(dot_pos + 1);
        int frac_digits = static_cast<int>(frac_sv.size());
        if (frac_digits > 0) {
            int64_t frac_val = 0;
            auto result = std::from_chars(frac_sv.data(), frac_sv.data() + frac_sv.size(), frac_val);
            if (result.ec != std::errc{} || result.ptr != frac_sv.data() + frac_sv.size()) {
                return ErrorCode::INVALID_FORMAT;
            }
            // Scale fractional part to 6 decimal places
            if (frac_digits <= kQtyScaleDigits) {
                int64_t multiplier = 1;
                for (int i = 0; i < kQtyScaleDigits - frac_digits; ++i) multiplier *= 10;
                frac_part = frac_val * multiplier;
            } else {
                // More than 6 fractional digits — truncate
                int64_t divisor = 1;
                for (int i = 0; i < frac_digits - kQtyScaleDigits; ++i) divisor *= 10;
                frac_part = frac_val / divisor;
            }
        }
    }

    if (integer_part < 0) return ErrorCode::NEGATIVE_VALUE;

    // Overflow guard: integer_part * kQtyScale must fit in int64_t
    if (integer_part > std::numeric_limits<int64_t>::max() / kQtyScale) {
        return ErrorCode::QTY_OVERFLOW;
    }

    int64_t val = integer_part * kQtyScale + frac_part;
    return Expected<Qty_t>(val);
}

std::string format_qty(Qty_t qty) {
    if (qty < 0) return "-" + format_qty(-qty);

    int64_t integer = qty / kQtyScale;
    int64_t frac = qty % kQtyScale;

    if (frac == 0) return std::to_string(integer);

    char buf[32];
    int len = std::snprintf(buf, sizeof(buf), "%lld.%06lld",
                            static_cast<long long>(integer),
                            static_cast<long long>(frac));

    // Trim trailing zeros
    while (len > 0 && buf[len - 1] == '0') --len;
    return std::string(buf, len);
}

Expected<Side> parse_side(std::string_view sv) {
    if (sv == "BUY" || sv == "BID" || sv == "buy" || sv == "bid" || sv == "Buy" || sv == "Bid") {
        return Expected<Side>(Side::BID);
    }
    if (sv == "SELL" || sv == "ASK" || sv == "sell" || sv == "ask" || sv == "Sell" || sv == "Ask") {
        return Expected<Side>(Side::ASK);
    }
    return ErrorCode::INVALID_FORMAT;
}

std::string format_price(Price_t price) {
    if (price < 0) return "-" + format_price(-price);

    int integer = price / kPriceScale;
    int frac = price % kPriceScale;

    if (frac == 0) return std::to_string(integer);

    char buf[16];
    int len = std::snprintf(buf, sizeof(buf), "%d.%04d", integer, frac);

    // Trim trailing zeros
    while (len > 0 && buf[len - 1] == '0') --len;
    return std::string(buf, len);
}

bool is_on_tick(Price_t price, TickSize_t tick_size) {
    if (tick_size <= 0) return false;
    return (price % tick_size) == 0;
}

}  // namespace lt
