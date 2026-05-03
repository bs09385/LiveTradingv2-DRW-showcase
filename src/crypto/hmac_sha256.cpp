#include "crypto/hmac_sha256.h"

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/bio.h>
#include <openssl/buffer.h>
#include <openssl/params.h>
#include <openssl/core_names.h>

#include <stdexcept>
#include <cstring>

namespace lt {

std::array<uint8_t, 32> hmac_sha256(const uint8_t* key, size_t key_len,
                                     const uint8_t* msg, size_t msg_len) {
    std::array<uint8_t, 32> result{};

    EVP_MAC* mac = EVP_MAC_fetch(nullptr, "HMAC", nullptr);
    if (!mac) {
        throw std::runtime_error("EVP_MAC_fetch(HMAC) failed");
    }

    EVP_MAC_CTX* ctx = EVP_MAC_CTX_new(mac);
    if (!ctx) {
        EVP_MAC_free(mac);
        throw std::runtime_error("EVP_MAC_CTX_new failed");
    }

    OSSL_PARAM params[2];
    params[0] = OSSL_PARAM_construct_utf8_string(
        OSSL_MAC_PARAM_DIGEST, const_cast<char*>("SHA256"), 0);
    params[1] = OSSL_PARAM_construct_end();

    if (EVP_MAC_init(ctx, key, key_len, params) != 1) {
        EVP_MAC_CTX_free(ctx);
        EVP_MAC_free(mac);
        throw std::runtime_error("EVP_MAC_init failed");
    }

    if (EVP_MAC_update(ctx, msg, msg_len) != 1) {
        EVP_MAC_CTX_free(ctx);
        EVP_MAC_free(mac);
        throw std::runtime_error("EVP_MAC_update failed");
    }

    size_t out_len = 32;
    if (EVP_MAC_final(ctx, result.data(), &out_len, 32) != 1) {
        EVP_MAC_CTX_free(ctx);
        EVP_MAC_free(mac);
        throw std::runtime_error("EVP_MAC_final failed");
    }

    EVP_MAC_CTX_free(ctx);
    EVP_MAC_free(mac);
    return result;
}

std::vector<uint8_t> base64_decode(std::string_view b64) {
    // Handle base64url: replace - with +, _ with /
    std::string input(b64);
    for (char& c : input) {
        if (c == '-') c = '+';
        else if (c == '_') c = '/';
    }
    // Add padding if needed
    while (input.size() % 4 != 0) {
        input.push_back('=');
    }

    BIO* b64_bio = BIO_new(BIO_f_base64());
    BIO_set_flags(b64_bio, BIO_FLAGS_BASE64_NO_NL);

    BIO* mem_bio = BIO_new_mem_buf(input.data(), static_cast<int>(input.size()));
    BIO_push(b64_bio, mem_bio);

    std::vector<uint8_t> result(input.size());  // upper bound
    int decoded_len = BIO_read(b64_bio, result.data(), static_cast<int>(result.size()));
    BIO_free_all(b64_bio);

    if (decoded_len < 0) {
        return {};
    }
    result.resize(static_cast<size_t>(decoded_len));
    return result;
}

std::string base64_encode(const uint8_t* data, size_t len) {
    BIO* b64_bio = BIO_new(BIO_f_base64());
    BIO_set_flags(b64_bio, BIO_FLAGS_BASE64_NO_NL);

    BIO* mem_bio = BIO_new(BIO_s_mem());
    BIO_push(b64_bio, mem_bio);

    BIO_write(b64_bio, data, static_cast<int>(len));
    BIO_flush(b64_bio);

    BUF_MEM* buf_ptr = nullptr;
    BIO_get_mem_ptr(b64_bio, &buf_ptr);

    std::string result(buf_ptr->data, buf_ptr->length);
    BIO_free_all(b64_bio);
    return result;
}

std::string base64url_encode(const uint8_t* data, size_t len) {
    std::string b64 = base64_encode(data, len);
    // Replace + with -, / with _, keep padding =
    std::string result;
    result.reserve(b64.size());
    for (char c : b64) {
        if (c == '+') result.push_back('-');
        else if (c == '/') result.push_back('_');
        else result.push_back(c);
    }
    return result;
}

}  // namespace lt
