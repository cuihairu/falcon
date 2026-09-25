// ============================================================================
// SwarmKeyStore 实现（见 swarm_key_store.hpp 头注释）
// ============================================================================

#include "swarm_key_store.hpp"

#include "../common/swarm_crypto.hpp"

#include <openssl/evp.h>
#include <openssl/pem.h>

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace falcon::swarm {

namespace {

SwarmKeyMaterial invalid_material(std::string* error, std::string message) {
    if (error != nullptr) *error = std::move(message);
    return SwarmKeyMaterial{};
}

/// 从 EVP_PKEY（Ed25519）提取完整身份（种子 + DER(SPKI) + 指纹 + hex）。
SwarmKeyMaterial material_from_evp_pkey(EVP_PKEY* pkey,
                                        std::string* error) {
    SwarmKeyMaterial out;

    std::vector<uint8_t> seed(32);
    std::size_t seed_len = seed.size();
    if (EVP_PKEY_get_raw_private_key(pkey, seed.data(), &seed_len) != 1 ||
        seed_len != seed.size()) {
        return invalid_material(error, "failed to extract raw private key");
    }
    out.private_seed = std::move(seed);

    unsigned char* der_buf = nullptr;
    const int der_len = i2d_PUBKEY(pkey, &der_buf);
    if (der_len <= 0 || der_buf == nullptr) {
        return invalid_material(error, "failed to encode public key (SPKI)");
    }
    out.public_key_der.assign(der_buf, der_buf + der_len);
    OPENSSL_free(der_buf);

    out.node_id = SwarmCrypto::fingerprint(out.public_key_der);
    out.pubkey_hex =
        SwarmCrypto::bytes_to_hex(out.public_key_der.data(),
                                  out.public_key_der.size());
    if (out.node_id.empty() || out.pubkey_hex.empty()) {
        return invalid_material(error, "failed to derive node identity");
    }
    return out;
}

}  // namespace

SwarmKeyMaterial load_or_create_swarm_key(const std::string& pem_path,
                                          std::string* error) {
    std::error_code ec;
    if (error != nullptr) error->clear();
    if (pem_path.empty()) {
        return invalid_material(error, "key file path is empty");
    }

    if (fs::exists(pem_path, ec)) {
        std::ifstream in(pem_path, std::ios::binary);
        if (!in) {
            return invalid_material(error, "failed to open key file: " + pem_path);
        }
        const std::string pem((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());
        if (pem.empty()) {
            return invalid_material(error, "key file is empty: " + pem_path);
        }

        BIO* bio = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
        if (bio == nullptr) {
            return invalid_material(error, "BIO allocation failed");
        }
        EVP_PKEY* pkey =
            PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
        BIO_free(bio);
        if (pkey == nullptr) {
            return invalid_material(error,
                                    "key file is not a valid PEM private key: " +
                                        pem_path);
        }
        SwarmKeyMaterial out = material_from_evp_pkey(pkey, error);
        EVP_PKEY_free(pkey);
        return out;
    }

    // ------------------------------------------------------------------
    // 首次生成：目录逐级创建 + 新密钥对 + PEM 落盘 + POSIX 0600
    // ------------------------------------------------------------------
    const fs::path parent = fs::path(pem_path).parent_path();
    if (!parent.empty()) {
        fs::create_directories(parent, ec);
        if (ec) {
            return invalid_material(error, "failed to create key directory: " +
                                               parent.string());
        }
    }

    const SwarmCrypto::KeyPair kp = SwarmCrypto::generate_keypair();
    if (kp.private_seed.empty()) {
        return invalid_material(error, "Ed25519 keypair generation failed");
    }

    EVP_PKEY* pkey = EVP_PKEY_new_raw_private_key(
        EVP_PKEY_ED25519, nullptr, kp.private_seed.data(),
        kp.private_seed.size());
    if (pkey == nullptr) {
        return invalid_material(error, "failed to reassemble Ed25519 key");
    }

    BIO* out_bio = BIO_new_file(pem_path.c_str(), "wb");
    if (out_bio == nullptr) {
        EVP_PKEY_free(pkey);
        return invalid_material(error, "failed to open key file for writing: " +
                                           pem_path);
    }
    const bool written =
        PEM_write_bio_PrivateKey(out_bio, pkey, nullptr, nullptr, 0, nullptr,
                                 nullptr) == 1;
    BIO_free(out_bio);
    EVP_PKEY_free(pkey);
    if (!written) {
        fs::remove(pem_path, ec);
        return invalid_material(error, "failed to write PEM key: " + pem_path);
    }

#ifndef _WIN32
    fs::permissions(pem_path,
                    fs::perms::owner_read | fs::perms::owner_write,
                    fs::perm_options::replace, ec);
    // 收权失败不致命（只影响多用户机器上的保密面），但记录
    if (ec) {
        if (error != nullptr) {
            *error = "warning: failed to chmod 0600 key file";
        }
        ec.clear();
    }
#endif

    SwarmKeyMaterial out;
    out.private_seed = kp.private_seed;
    out.public_key_der = kp.public_key_der;
    out.node_id = SwarmCrypto::fingerprint(kp.public_key_der);
    out.pubkey_hex = SwarmCrypto::bytes_to_hex(kp.public_key_der.data(),
                                               kp.public_key_der.size());
    if (!out.valid()) {
        return invalid_material(error, "failed to derive node identity");
    }
    return out;
}

}  // namespace falcon::swarm
