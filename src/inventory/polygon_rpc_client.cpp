#include "inventory/polygon_rpc_client.h"
#include "common/discovery.h"
#include "crypto/hex_utils.h"

#include "simdjson.h"

#include <cstdio>
#include <cstring>

namespace lt {

PolygonRpcClient::PolygonRpcClient(const std::string& rpc_url, int timeout_ms)
    : host_(extract_host(rpc_url)),
      path_(extract_path(rpc_url)),
      timeout_ms_(timeout_ms) {
    if (path_.empty()) path_ = "/";
}

std::string PolygonRpcClient::do_rpc(const std::string& body) {
    return sync_https_post(host_, path_, body, timeout_ms_);
}

uint64_t PolygonRpcClient::parse_hex_uint64(const char* hex, size_t len) {
    // Skip "0x" prefix
    if (len >= 2 && hex[0] == '0' && (hex[1] == 'x' || hex[1] == 'X')) {
        hex += 2;
        len -= 2;
    }
    uint64_t val = 0;
    for (size_t i = 0; i < len; ++i) {
        val <<= 4;
        char c = hex[i];
        if (c >= '0' && c <= '9') val |= static_cast<uint64_t>(c - '0');
        else if (c >= 'a' && c <= 'f') val |= static_cast<uint64_t>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') val |= static_cast<uint64_t>(c - 'A' + 10);
    }
    return val;
}

Expected<uint64_t> PolygonRpcClient::get_gas_price() {
    char body[128];
    std::snprintf(body, sizeof(body),
        R"({"jsonrpc":"2.0","method":"eth_gasPrice","params":[],"id":%d})",
        rpc_id_++);

    auto response = do_rpc(body);
    if (response.empty()) {
        last_error_ = "eth_gasPrice: empty response";
        return ErrorCode::NETWORK_ERROR;
    }

    simdjson::dom::parser parser;
    simdjson::dom::element doc;
    if (parser.parse(response).get(doc) != simdjson::SUCCESS) {
        last_error_ = "eth_gasPrice: JSON parse failed";
        return ErrorCode::JSON_PARSE_ERROR;
    }

    // Check for RPC error
    simdjson::dom::object err_obj;
    if (doc["error"].get_object().get(err_obj) == simdjson::SUCCESS) {
        std::string_view msg;
        err_obj["message"].get_string().get(msg);
        last_error_ = "eth_gasPrice error: " + std::string(msg);
        return ErrorCode::NETWORK_ERROR;
    }

    std::string_view result;
    if (doc["result"].get_string().get(result) != simdjson::SUCCESS) {
        last_error_ = "eth_gasPrice: missing result";
        return ErrorCode::JSON_MISSING_FIELD;
    }

    return Expected<uint64_t>(parse_hex_uint64(result.data(), result.size()));
}

Expected<uint64_t> PolygonRpcClient::get_transaction_count(const std::string& address) {
    char body[256];
    std::snprintf(body, sizeof(body),
        R"({"jsonrpc":"2.0","method":"eth_getTransactionCount","params":["%s","pending"],"id":%d})",
        address.c_str(), rpc_id_++);

    auto response = do_rpc(body);
    if (response.empty()) {
        last_error_ = "eth_getTransactionCount: empty response";
        return ErrorCode::NETWORK_ERROR;
    }

    simdjson::dom::parser parser;
    simdjson::dom::element doc;
    if (parser.parse(response).get(doc) != simdjson::SUCCESS) {
        last_error_ = "eth_getTransactionCount: JSON parse failed";
        return ErrorCode::JSON_PARSE_ERROR;
    }

    simdjson::dom::object err_obj;
    if (doc["error"].get_object().get(err_obj) == simdjson::SUCCESS) {
        std::string_view msg;
        err_obj["message"].get_string().get(msg);
        last_error_ = "eth_getTransactionCount error: " + std::string(msg);
        return ErrorCode::NETWORK_ERROR;
    }

    std::string_view result;
    if (doc["result"].get_string().get(result) != simdjson::SUCCESS) {
        last_error_ = "eth_getTransactionCount: missing result";
        return ErrorCode::JSON_MISSING_FIELD;
    }

    return Expected<uint64_t>(parse_hex_uint64(result.data(), result.size()));
}

Expected<std::string> PolygonRpcClient::send_raw_transaction(const std::vector<uint8_t>& signed_tx) {
    std::string tx_hex = "0x" + hex_encode(signed_tx.data(), signed_tx.size());

    // Build JSON-RPC body
    std::string body;
    body.reserve(tx_hex.size() + 128);
    body += R"({"jsonrpc":"2.0","method":"eth_sendRawTransaction","params":[")" ;
    body += tx_hex;
    body += R"("],"id":)";
    body += std::to_string(rpc_id_++);
    body += "}";

    auto response = do_rpc(body);
    if (response.empty()) {
        last_error_ = "eth_sendRawTransaction: empty response";
        return ErrorCode::NETWORK_ERROR;
    }

    simdjson::dom::parser parser;
    simdjson::dom::element doc;
    if (parser.parse(response).get(doc) != simdjson::SUCCESS) {
        last_error_ = "eth_sendRawTransaction: JSON parse failed";
        return ErrorCode::JSON_PARSE_ERROR;
    }

    simdjson::dom::object err_obj;
    if (doc["error"].get_object().get(err_obj) == simdjson::SUCCESS) {
        std::string_view msg;
        err_obj["message"].get_string().get(msg);
        last_error_ = "eth_sendRawTransaction error: " + std::string(msg);
        return ErrorCode::NETWORK_ERROR;
    }

    std::string_view result;
    if (doc["result"].get_string().get(result) != simdjson::SUCCESS) {
        last_error_ = "eth_sendRawTransaction: missing result";
        return ErrorCode::JSON_MISSING_FIELD;
    }

    return Expected<std::string>(std::string(result));
}

Expected<TransactionReceipt> PolygonRpcClient::get_transaction_receipt(const std::string& tx_hash) {
    char body[256];
    std::snprintf(body, sizeof(body),
        R"({"jsonrpc":"2.0","method":"eth_getTransactionReceipt","params":["%s"],"id":%d})",
        tx_hash.c_str(), rpc_id_++);

    auto response = do_rpc(body);
    if (response.empty()) {
        last_error_ = "eth_getTransactionReceipt: empty response";
        return ErrorCode::NETWORK_ERROR;
    }

    simdjson::dom::parser parser;
    simdjson::dom::element doc;
    if (parser.parse(response).get(doc) != simdjson::SUCCESS) {
        last_error_ = "eth_getTransactionReceipt: JSON parse failed";
        return ErrorCode::JSON_PARSE_ERROR;
    }

    simdjson::dom::object err_obj;
    if (doc["error"].get_object().get(err_obj) == simdjson::SUCCESS) {
        std::string_view msg;
        err_obj["message"].get_string().get(msg);
        last_error_ = "eth_getTransactionReceipt error: " + std::string(msg);
        return ErrorCode::NETWORK_ERROR;
    }

    // result is null when tx is still pending
    if (doc["result"].is_null()) {
        last_error_ = "receipt pending";
        return ErrorCode::JSON_MISSING_FIELD;
    }

    simdjson::dom::object receipt_obj;
    if (doc["result"].get_object().get(receipt_obj) != simdjson::SUCCESS) {
        last_error_ = "eth_getTransactionReceipt: result not an object";
        return ErrorCode::JSON_PARSE_ERROR;
    }

    TransactionReceipt receipt;

    std::string_view status_hex;
    if (receipt_obj["status"].get_string().get(status_hex) == simdjson::SUCCESS) {
        receipt.status = parse_hex_uint64(status_hex.data(), status_hex.size());
    }

    std::string_view gas_hex;
    if (receipt_obj["gasUsed"].get_string().get(gas_hex) == simdjson::SUCCESS) {
        receipt.gas_used = parse_hex_uint64(gas_hex.data(), gas_hex.size());
    }

    std::string_view block_hex;
    if (receipt_obj["blockNumber"].get_string().get(block_hex) == simdjson::SUCCESS) {
        receipt.block_number = parse_hex_uint64(block_hex.data(), block_hex.size());
    }

    std::string_view tx_hash_sv;
    if (receipt_obj["transactionHash"].get_string().get(tx_hash_sv) == simdjson::SUCCESS) {
        receipt.transaction_hash = std::string(tx_hash_sv);
    }

    return Expected<TransactionReceipt>(std::move(receipt));
}

}  // namespace lt
