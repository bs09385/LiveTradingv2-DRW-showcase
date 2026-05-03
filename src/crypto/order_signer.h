#pragma once

#include <cstdint>
#include <string>
#include "crypto/hex_utils.h"
#include "crypto/eip712.h"

namespace lt {

// Abstract interface for order signing
class OrderSigner {
public:
    virtual ~OrderSigner() = default;

    // Sign an order: compute EIP-712 hash and produce a 65-byte recoverable signature.
    // signature_out must point to at least 65 bytes: [r(32) || s(32) || v(1)]
    virtual bool sign_order(const OrderFields& fields, const Bytes32& domain_sep,
                            uint8_t signature_out[65]) = 0;

    // Ethereum personal_sign (EIP-191): hash "\x19Ethereum Signed Message:\n<len><msg>"
    // and produce a 65-byte recoverable signature [r(32) || s(32) || v(1)].
    virtual bool personal_sign(const std::string& message, uint8_t signature_out[65]) = 0;

    // Sign a raw 32-byte hash with recoverable ECDSA.
    // signature_out must point to at least 65 bytes: [r(32) || s(32) || v(1)]
    virtual bool sign_hash(const Bytes32& hash, uint8_t signature_out[65]) = 0;

    // Get the signer's Ethereum address (20 bytes)
    virtual bool get_signer_address(uint8_t address_out[20]) const = 0;
};

// secp256k1-based order signer using a raw private key
class Secp256k1OrderSigner : public OrderSigner {
public:
    // Takes a 32-byte private key. Copies it internally.
    explicit Secp256k1OrderSigner(const uint8_t private_key[32]);
    ~Secp256k1OrderSigner() override;

    // Non-copyable
    Secp256k1OrderSigner(const Secp256k1OrderSigner&) = delete;
    Secp256k1OrderSigner& operator=(const Secp256k1OrderSigner&) = delete;

    bool sign_order(const OrderFields& fields, const Bytes32& domain_sep,
                    uint8_t signature_out[65]) override;

    bool personal_sign(const std::string& message, uint8_t signature_out[65]) override;

    bool sign_hash(const Bytes32& hash, uint8_t signature_out[65]) override;

    bool get_signer_address(uint8_t address_out[20]) const override;

private:
    struct Impl;
    Impl* impl_;
};

}  // namespace lt
