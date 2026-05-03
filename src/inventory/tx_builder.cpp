#include "inventory/tx_builder.h"
#include "inventory/rlp_encoder.h"
#include "crypto/keccak.h"
#include "crypto/order_signer.h"

namespace lt {

// RLP encode the common transaction fields (nonce through data).
static void encode_tx_fields(std::vector<uint8_t>& items, const RawTxParams& p) {
    rlp_encode_uint64(items, p.nonce);
    rlp_encode_uint64(items, p.gas_price);
    rlp_encode_uint64(items, p.gas_limit);
    rlp_encode_string(items, p.to, 20);
    rlp_encode_uint64(items, p.value);
    rlp_encode_string(items, p.data.data(), p.data.size());
}

std::vector<uint8_t> build_signed_transaction(
    const RawTxParams& params,
    OrderSigner& signer) {

    // 1. RLP encode unsigned tx for signing (EIP-155):
    //    [nonce, gasPrice, gasLimit, to, value, data, chainId, 0, 0]
    std::vector<uint8_t> unsigned_items;
    unsigned_items.reserve(512);
    encode_tx_fields(unsigned_items, params);
    rlp_encode_uint64(unsigned_items, params.chain_id);
    rlp_encode_uint64(unsigned_items, 0);  // empty r
    rlp_encode_uint64(unsigned_items, 0);  // empty s

    std::vector<uint8_t> unsigned_rlp;
    unsigned_rlp.reserve(unsigned_items.size() + 4);
    rlp_wrap_list(unsigned_rlp, unsigned_items);

    // 2. keccak256 of unsigned RLP
    Bytes32 tx_hash = keccak256(unsigned_rlp.data(), unsigned_rlp.size());

    // 3. Sign with secp256k1
    uint8_t sig[65]{};
    if (!signer.sign_hash(tx_hash, sig)) {
        return {};
    }

    // 4. Extract r, s, and compute EIP-155 v
    //    sign_hash returns v = 27 + recid, so recid = sig[64] - 27
    uint8_t recid = sig[64] - 27;
    uint64_t v = params.chain_id * 2 + 35 + recid;
    uint8_t r[32], s[32];
    std::memcpy(r, sig, 32);
    std::memcpy(s, sig + 32, 32);

    // 5. RLP encode signed tx:
    //    [nonce, gasPrice, gasLimit, to, value, data, v, r, s]
    std::vector<uint8_t> signed_items;
    signed_items.reserve(512);
    encode_tx_fields(signed_items, params);
    rlp_encode_uint64(signed_items, v);
    rlp_encode_uint256(signed_items, r);
    rlp_encode_uint256(signed_items, s);

    std::vector<uint8_t> signed_rlp;
    signed_rlp.reserve(signed_items.size() + 4);
    rlp_wrap_list(signed_rlp, signed_items);

    return signed_rlp;
}

Bytes32 compute_tx_hash(const std::vector<uint8_t>& signed_tx) {
    return keccak256(signed_tx.data(), signed_tx.size());
}

}  // namespace lt
