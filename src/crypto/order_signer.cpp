#include "crypto/order_signer.h"
#include "crypto/keccak.h"
#include "crypto/eip712.h"

#include <secp256k1.h>
#include <secp256k1_recovery.h>

#include <cstring>
#include <stdexcept>

#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#endif

namespace lt {

struct Secp256k1OrderSigner::Impl {
    secp256k1_context* ctx = nullptr;
    uint8_t private_key[32]{};
    uint8_t address[20]{};
    bool address_valid = false;

    Impl(const uint8_t pk[32]) {
        std::memcpy(private_key, pk, 32);

        ctx = secp256k1_context_create(SECP256K1_CONTEXT_SIGN);
        if (!ctx) {
            throw std::runtime_error("secp256k1_context_create failed");
        }

        // Randomize context with OS entropy
        uint8_t seed[32];
#ifdef _WIN32
        NTSTATUS status = BCryptGenRandom(nullptr, seed, sizeof(seed),
                                          BCRYPT_USE_SYSTEM_PREFERRED_RNG);
        if (status != 0) {
            secp256k1_context_destroy(ctx);
            throw std::runtime_error("BCryptGenRandom failed");
        }
#else
        FILE* f = fopen("/dev/urandom", "rb");
        if (!f || fread(seed, 1, 32, f) != 32) {
            if (f) fclose(f);
            secp256k1_context_destroy(ctx);
            throw std::runtime_error("Failed to read /dev/urandom");
        }
        fclose(f);
#endif
        if (!secp256k1_context_randomize(ctx, seed)) {
            secp256k1_context_destroy(ctx);
            throw std::runtime_error("secp256k1_context_randomize failed");
        }

        // Verify private key is valid
        if (!secp256k1_ec_seckey_verify(ctx, private_key)) {
            secp256k1_context_destroy(ctx);
            throw std::runtime_error("Invalid private key");
        }

        // Derive address
        derive_address();
    }

    ~Impl() {
        if (ctx) {
            secp256k1_context_destroy(ctx);
        }
        // Zero private key
        volatile uint8_t* p = private_key;
        for (int i = 0; i < 32; ++i) {
            p[i] = 0;
        }
    }

    void derive_address() {
        secp256k1_pubkey pubkey;
        if (!secp256k1_ec_pubkey_create(ctx, &pubkey, private_key)) {
            return;
        }

        // Serialize uncompressed: 65 bytes (0x04 || x || y)
        uint8_t pub_serialized[65];
        size_t pub_len = 65;
        secp256k1_ec_pubkey_serialize(ctx, pub_serialized, &pub_len,
                                       &pubkey, SECP256K1_EC_UNCOMPRESSED);

        // Address = keccak256(pubkey[1:]) -> last 20 bytes
        Bytes32 hash = keccak256(pub_serialized + 1, 64);
        std::memcpy(address, hash.data() + 12, 20);
        address_valid = true;
    }
};

Secp256k1OrderSigner::Secp256k1OrderSigner(const uint8_t private_key[32])
    : impl_(new Impl(private_key)) {}

Secp256k1OrderSigner::~Secp256k1OrderSigner() {
    delete impl_;
}

bool Secp256k1OrderSigner::sign_order(const OrderFields& fields, const Bytes32& domain_sep,
                                       uint8_t signature_out[65]) {
    // 1. Compute struct hash
    Bytes32 struct_hash = hash_order_struct(fields);

    // 2. Compute EIP-712 signing hash
    Bytes32 signing_hash = eip712_signing_hash(domain_sep, struct_hash);

    // 3. Sign with recoverable ECDSA
    secp256k1_ecdsa_recoverable_signature sig;
    if (!secp256k1_ecdsa_sign_recoverable(impl_->ctx, &sig,
                                           signing_hash.data(),
                                           impl_->private_key,
                                           nullptr, nullptr)) {
        return false;
    }

    // 4. Serialize: r(32) || s(32) || v(1)
    int recid = 0;
    secp256k1_ecdsa_recoverable_signature_serialize_compact(
        impl_->ctx, signature_out, &recid, &sig);

    // Ethereum v = 27 + recovery_id
    signature_out[64] = static_cast<uint8_t>(27 + recid);

    return true;
}

bool Secp256k1OrderSigner::personal_sign(const std::string& message, uint8_t signature_out[65]) {
    // Build EIP-191 prefix: "\x19Ethereum Signed Message:\n" + length + message
    std::string prefix = "\x19""Ethereum Signed Message:\n" + std::to_string(message.size()) + message;

    // Hash with Keccak-256
    Bytes32 hash = keccak256(reinterpret_cast<const uint8_t*>(prefix.data()), prefix.size());

    // Sign with recoverable ECDSA
    secp256k1_ecdsa_recoverable_signature sig;
    if (!secp256k1_ecdsa_sign_recoverable(impl_->ctx, &sig,
                                           hash.data(),
                                           impl_->private_key,
                                           nullptr, nullptr)) {
        return false;
    }

    // Serialize: r(32) || s(32) || v(1)
    int recid = 0;
    secp256k1_ecdsa_recoverable_signature_serialize_compact(
        impl_->ctx, signature_out, &recid, &sig);

    // Ethereum v = 27 + recovery_id
    signature_out[64] = static_cast<uint8_t>(27 + recid);
    return true;
}

bool Secp256k1OrderSigner::sign_hash(const Bytes32& hash, uint8_t signature_out[65]) {
    secp256k1_ecdsa_recoverable_signature sig;
    if (!secp256k1_ecdsa_sign_recoverable(impl_->ctx, &sig,
                                           hash.data(),
                                           impl_->private_key,
                                           nullptr, nullptr)) {
        return false;
    }

    int recid = 0;
    secp256k1_ecdsa_recoverable_signature_serialize_compact(
        impl_->ctx, signature_out, &recid, &sig);

    signature_out[64] = static_cast<uint8_t>(27 + recid);
    return true;
}

bool Secp256k1OrderSigner::get_signer_address(uint8_t address_out[20]) const {
    if (!impl_->address_valid) return false;
    std::memcpy(address_out, impl_->address, 20);
    return true;
}

}  // namespace lt
