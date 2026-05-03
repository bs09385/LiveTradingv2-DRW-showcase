#include "common/position_bootstrap.h"

#include <cmath>
#include <cstdio>
#include <string>

#include "common/discovery.h"
#include "common/types.h"
#include "simdjson.h"

namespace lt {

namespace {

constexpr const char* kDataApiHost = "data-api.polymarket.com";

}  // namespace

int bootstrap_positions(const std::string& user_address,
                        const std::vector<std::string>& condition_ids,
                        TokenInventory& inventory,
                        int timeout_ms) {
    if (user_address.empty() || condition_ids.empty()) return 0;

    int seeded = 0;

    for (const auto& condition_id : condition_ids) {
        std::string target = "/v1/market-positions?market=" + condition_id +
                             "&user=" + user_address + "&status=OPEN";

        auto body = sync_https_get(kDataApiHost, target, timeout_ms);
        if (body.empty()) {
            std::fprintf(stderr, "  position_bootstrap: fetch failed for market %.40s...\n",
                         condition_id.c_str());
            continue;
        }

        try {
            simdjson::dom::parser parser;
            auto doc_result = parser.parse(body);
            if (doc_result.error()) {
                std::fprintf(stderr, "  position_bootstrap: JSON parse error for market %.40s...\n",
                             condition_id.c_str());
                continue;
            }

            simdjson::dom::array token_arr;
            if (doc_result.value().get_array().get(token_arr)) {
                std::fprintf(stderr, "  position_bootstrap: expected array for market %.40s...\n",
                             condition_id.c_str());
                continue;
            }

            for (auto token_obj : token_arr) {
                simdjson::dom::array positions;
                if (token_obj["positions"].get_array().get(positions)) continue;

                for (auto pos : positions) {
                    std::string_view asset_sv;
                    if (pos["asset"].get_string().get(asset_sv)) continue;

                    double size_d = 0.0;
                    if (pos["size"].get_double().get(size_d)) continue;

                    Qty_t size = static_cast<Qty_t>(std::llround(size_d * kQtyScale));
                    if (size <= 0) continue;

                    AssetId token_id(asset_sv);
                    inventory.set_position(token_id, size);
                    ++seeded;

                    std::printf("  %.*s: %.2f shares\n",
                                static_cast<int>(asset_sv.size()), asset_sv.data(),
                                size_d);
                }
            }
        } catch (...) {
            std::fprintf(stderr, "  position_bootstrap: parse exception for market %.40s...\n",
                         condition_id.c_str());
        }
    }

    return seeded;
}

}  // namespace lt
