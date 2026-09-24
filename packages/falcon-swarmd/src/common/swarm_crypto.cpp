// ============================================================================
// SwarmCrypto 实现：OpenSSL EVP 便携 API
//
// 注入点（falcon::detail::InjectPoint，测试构建生效）：
//   SwarmSignCtxNew / SwarmSign     —— 签名链 ctx 创建 / 签名动作失败
//   SwarmVerifyCtxNew / SwarmVerify —— 验签链 ctx 创建 / 验签动作失败
// 创建类注入短路真实创建防泄漏（批次 Z 形态）；动作类注入跳过真实
// 调用直接走失败收口。
// ============================================================================

#include "swarm_crypto.hpp"

#include <falcon/detail/injection.hpp>

#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/x509.h>

#include <limits>

namespace falcon::swarm {

namespace {

constexpr char kHexDigits[] = "0123456789abcdef";

int hex_nibble(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

}  // namespace

SwarmCrypto::KeyPair SwarmCrypto::generate_keypair() {
    KeyPair pair;

    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, nullptr);
    if (!ctx) {
        return pair;
    }
    EVP_PKEY* pkey = nullptr;
    if (EVP_PKEY_keygen_init(ctx) != 1 || EVP_PKEY_keygen(ctx, &pkey) != 1) {
        EVP_PKEY_CTX_free(ctx);
        return pair;
    }
    EVP_PKEY_CTX_free(ctx);

    // 私钥：原始 32 字节种子（RFC 8032 向量形态）
    std::size_t seed_len = 0;
    if (EVP_PKEY_get_raw_private_key(pkey, nullptr, &seed_len) != 1 ||
        seed_len != 32) {
        EVP_PKEY_free(pkey);
        return pair;
    }
    pair.private_seed.resize(seed_len);
    if (EVP_PKEY_get_raw_private_key(pkey, pair.private_seed.data(),
                                     &seed_len) != 1) {
        EVP_PKEY_free(pkey);
        pair.private_seed.clear();
        return pair;
    }

    // 公钥：DER(SPKI)
    const int der_len = i2d_PUBKEY(pkey, nullptr);
    if (der_len <= 0) {
        EVP_PKEY_free(pkey);
        pair.private_seed.clear();
        return pair;
    }
    pair.public_key_der.resize(static_cast<std::size_t>(der_len));
    unsigned char* der_ptr = pair.public_key_der.data();
    if (i2d_PUBKEY(pkey, &der_ptr) != der_len) {
        EVP_PKEY_free(pkey);
        pair.private_seed.clear();
        pair.public_key_der.clear();
        return pair;
    }

    EVP_PKEY_free(pkey);
    return pair;
}

std::string SwarmCrypto::bytes_to_hex(const uint8_t* data, std::size_t len) {
    if (!data) {
        return {};
    }
    std::string out;
    out.reserve(len * 2);
    for (std::size_t i = 0; i < len; ++i) {
        out.push_back(kHexDigits[data[i] >> 4]);
        out.push_back(kHexDigits[data[i] & 0x0F]);
    }
    return out;
}

std::vector<uint8_t> SwarmCrypto::hex_to_bytes(const std::string& hex) {
    if (hex.size() % 2 != 0) {
        return {};
    }
    std::vector<uint8_t> out;
    out.reserve(hex.size() / 2);
    for (std::size_t i = 0; i < hex.size(); i += 2) {
        const int hi = hex_nibble(hex[i]);
        const int lo = hex_nibble(hex[i + 1]);
        if (hi < 0 || lo < 0) {
            return {};
        }
        out.push_back(static_cast<uint8_t>((hi << 4) | lo));
    }
    return out;
}

std::string SwarmCrypto::sha256_hex(const uint8_t* data, std::size_t len) {
    if (!data && len > 0) {
        return {};
    }
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int md_len = 0;
    if (EVP_Digest(data, len, md, &md_len, EVP_sha256(), nullptr) != 1) {
        return {};
    }
    return bytes_to_hex(md, md_len);
}

std::string SwarmCrypto::sha256_hex(const std::string& data) {
    return sha256_hex(reinterpret_cast<const uint8_t*>(data.data()),
                      data.size());
}

std::string SwarmCrypto::sign(const std::vector<uint8_t>& private_seed,
                              const std::string& message) {
    if (private_seed.size() != 32) {
        return {};
    }

    EVP_PKEY* pkey = EVP_PKEY_new_raw_private_key(
        EVP_PKEY_ED25519, nullptr, private_seed.data(), private_seed.size());
    if (!pkey) {
        return {};
    }

    EVP_MD_CTX* ctx = inject_failure(
                          detail::InjectPoint::SwarmSignCtxNew)
                          ? nullptr
                          : EVP_MD_CTX_new();
    if (!ctx) {
        EVP_PKEY_free(pkey);
        return {};
    }

    std::string result;
    // 一步式签名（md=nullptr）：首调取签名长度，二调产出
    std::size_t sig_len = 0;
    if (EVP_DigestSignInit(ctx, nullptr, nullptr, nullptr, pkey) == 1 &&
        EVP_DigestSign(ctx, nullptr, &sig_len,
                       reinterpret_cast<const unsigned char*>(message.data()),
                       message.size()) == 1 &&
        !inject_failure(detail::InjectPoint::SwarmSign)) {
        std::vector<unsigned char> sig(sig_len);
        std::size_t out_len = sig_len;
        if (EVP_DigestSign(ctx, sig.data(), &out_len,
                           reinterpret_cast<const unsigned char*>(
                               message.data()),
                           message.size()) == 1) {
            result = bytes_to_hex(sig.data(), out_len);
        }
    }

    EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(pkey);
    return result;
}

bool SwarmCrypto::verify(const std::vector<uint8_t>& public_key_der,
                         const std::string& message,
                         const std::string& signature_hex) {
    if (public_key_der.empty()) {
        return false;
    }

    const unsigned char* der_ptr = public_key_der.data();
    EVP_PKEY* pkey =
        d2i_PUBKEY(nullptr, &der_ptr,
                   static_cast<long>(public_key_der.size()));
    if (!pkey) {
        return false;
    }

    EVP_MD_CTX* ctx = inject_failure(
                          detail::InjectPoint::SwarmVerifyCtxNew)
                          ? nullptr
                          : EVP_MD_CTX_new();
    if (!ctx) {
        EVP_PKEY_free(pkey);
        return false;
    }

    bool ok = false;
    const std::vector<uint8_t> sig = hex_to_bytes(signature_hex);
    if (sig.size() == 64 &&
        EVP_DigestVerifyInit(ctx, nullptr, nullptr, nullptr, pkey) == 1 &&
        !inject_failure(detail::InjectPoint::SwarmVerify)) {
        ok = EVP_DigestVerify(ctx, sig.data(), sig.size(),
                              reinterpret_cast<const unsigned char*>(
                                  message.data()),
                              message.size()) == 1;
    }

    EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(pkey);
    return ok;
}

std::string SwarmCrypto::fingerprint(
    const std::vector<uint8_t>& public_key_der) {
    const std::string hash = sha256_hex(public_key_der.data(),
                                        public_key_der.size());
    return hash.substr(0, 32);
}

std::vector<uint8_t> SwarmCrypto::random_bytes(std::size_t n) {
    if (n > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return {};
    }
    std::vector<uint8_t> out(n);
    if (n > 0 && RAND_bytes(out.data(), static_cast<int>(n)) != 1) {
        return {};
    }
    return out;
}

}  // namespace falcon::swarm
