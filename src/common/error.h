#pragma once

#include <cstdint>
#include <variant>

namespace lt {

enum class ErrorCode : uint8_t {
    OK = 0,
    // Parse errors
    EMPTY_INPUT,
    INVALID_FORMAT,
    OUT_OF_RANGE,
    NEGATIVE_VALUE,
    // JSON errors
    JSON_PARSE_ERROR,
    JSON_MISSING_FIELD,
    JSON_TYPE_ERROR,
    UNKNOWN_EVENT_TYPE,
    PONG_MESSAGE,
    // Book errors
    BOOK_INVALID_PRICE,
    BOOK_NEGATIVE_QTY,
    BOOK_CROSSED,
    // Config errors
    CONFIG_FILE_NOT_FOUND,
    CONFIG_PARSE_ERROR,
    // User WS errors
    USER_UNKNOWN_STATUS,
    USER_UNKNOWN_EVENT_TYPE,
    USER_WS_SERVER_ERROR,   // Server sent {"error":"..."} message
    QTY_OVERFLOW,           // integer_part * kQtyScale would overflow int64
    // Queue errors
    QUEUE_FULL,
    // REST/exec errors (M4)
    REST_CONNECTION_FAILED,
    REST_TIMEOUT,
    REST_AUTH_FAILED,
    EXEC_SIGNING_FAILED,
    EXEC_BUILD_FAILED,
    // Network/relayer errors
    NETWORK_ERROR,
    TIMEOUT,
};

inline const char* error_name(ErrorCode code) {
    switch (code) {
        case ErrorCode::OK: return "OK";
        case ErrorCode::EMPTY_INPUT: return "EMPTY_INPUT";
        case ErrorCode::INVALID_FORMAT: return "INVALID_FORMAT";
        case ErrorCode::OUT_OF_RANGE: return "OUT_OF_RANGE";
        case ErrorCode::NEGATIVE_VALUE: return "NEGATIVE_VALUE";
        case ErrorCode::JSON_PARSE_ERROR: return "JSON_PARSE_ERROR";
        case ErrorCode::JSON_MISSING_FIELD: return "JSON_MISSING_FIELD";
        case ErrorCode::JSON_TYPE_ERROR: return "JSON_TYPE_ERROR";
        case ErrorCode::UNKNOWN_EVENT_TYPE: return "UNKNOWN_EVENT_TYPE";
        case ErrorCode::PONG_MESSAGE: return "PONG_MESSAGE";
        case ErrorCode::BOOK_INVALID_PRICE: return "BOOK_INVALID_PRICE";
        case ErrorCode::BOOK_NEGATIVE_QTY: return "BOOK_NEGATIVE_QTY";
        case ErrorCode::BOOK_CROSSED: return "BOOK_CROSSED";
        case ErrorCode::CONFIG_FILE_NOT_FOUND: return "CONFIG_FILE_NOT_FOUND";
        case ErrorCode::CONFIG_PARSE_ERROR: return "CONFIG_PARSE_ERROR";
        case ErrorCode::USER_UNKNOWN_STATUS: return "USER_UNKNOWN_STATUS";
        case ErrorCode::USER_UNKNOWN_EVENT_TYPE: return "USER_UNKNOWN_EVENT_TYPE";
        case ErrorCode::USER_WS_SERVER_ERROR: return "USER_WS_SERVER_ERROR";
        case ErrorCode::QTY_OVERFLOW: return "QTY_OVERFLOW";
        case ErrorCode::QUEUE_FULL: return "QUEUE_FULL";
        case ErrorCode::REST_CONNECTION_FAILED: return "REST_CONNECTION_FAILED";
        case ErrorCode::REST_TIMEOUT: return "REST_TIMEOUT";
        case ErrorCode::REST_AUTH_FAILED: return "REST_AUTH_FAILED";
        case ErrorCode::EXEC_SIGNING_FAILED: return "EXEC_SIGNING_FAILED";
        case ErrorCode::EXEC_BUILD_FAILED: return "EXEC_BUILD_FAILED";
        case ErrorCode::NETWORK_ERROR: return "NETWORK_ERROR";
        case ErrorCode::TIMEOUT: return "TIMEOUT";
    }
    return "UNKNOWN";
}

template <typename T>
struct Expected {
    T value;
    ErrorCode error;

    Expected() : value{}, error{ErrorCode::OK} {}
    explicit Expected(T val) : value{std::move(val)}, error{ErrorCode::OK} {}
    Expected(ErrorCode err) : value{}, error{err} {}

    bool ok() const { return error == ErrorCode::OK; }
    explicit operator bool() const { return ok(); }
};

}  // namespace lt
