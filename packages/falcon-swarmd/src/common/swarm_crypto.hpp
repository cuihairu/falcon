#pragma once

// ============================================================================
// SwarmCrypto：Ed25519 密钥/签名与 SHA-256 摘要
// （docs/p2sp_network_design.md §8.1 线协议密码学）
//
// 全部走 OpenSSL EVP 便携 API（3.0 弃用的低层接口一律不触碰）；
// Ed25519 签名用一步式 EVP_DigestSign/EVP_DigestVerify（md=nullptr，
// Ed25519 无摘要语义，RFC 8032 向量以原始种子为私钥形态）。
//
// 编码定案（§8.1）：
//   私钥   = 原始 32 字节种子（内存形态；文件持久化由 swarm_key_store 负责）
//   公钥   = DER(SPKI) 小写 hex，恒 88 字符
//   签名   = 小写 hex，恒 128 字符（Ed25519 恒 64 字节）
//   指纹   = sha256_hex(DER(SPKI)) 前 32 字符（RFC 8032 TEST1 向量的
//            指纹为 06e3fd8fda29bb60ab59557de61edb0a，测试钉死）
// ============================================================================

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace falcon::swarm {

class SwarmCrypto {
public:
    struct KeyPair {
        std::vector<uint8_t> private_seed;    // 原始种子，恒 32 字节
        std::vector<uint8_t> public_key_der;  // DER(SPKI)，恒 44 字节
    };

    // 生成 Ed25519 密钥对；任一步失败返回空种子（调用方判 private_seed
    // 是否为空）
    static KeyPair generate_keypair();

    // hex 编解码（小写输出；decode 对奇数长度或非 hex 字符返回空）
    static std::string bytes_to_hex(const uint8_t* data, std::size_t len);
    static std::vector<uint8_t> hex_to_bytes(const std::string& hex);

    // SHA-256 → 小写 hex（64 字符）；EVP_Digest 失败返回空串
    static std::string sha256_hex(const uint8_t* data, std::size_t len);
    static std::string sha256_hex(const std::string& data);

    // Ed25519 签名：message 任意字节 → hex(128)；种子非 32 字节或
    // 任一 EVP 步骤失败返回空串
    static std::string sign(const std::vector<uint8_t>& private_seed,
                            const std::string& message);

    // Ed25519 验签（公钥 DER(SPKI) 形态）；任何失败——DER 解析、签名
    // 非 64 字节、非 hex、密码学不匹配——一律返回 false
    static bool verify(const std::vector<uint8_t>& public_key_der,
                       const std::string& message,
                       const std::string& signature_hex);

    // 指纹：sha256_hex(DER(SPKI)) 前 32 字符
    static std::string fingerprint(const std::vector<uint8_t>& public_key_der);

    // 密码学随机字节（challenge/session/nonce 生成）；失败返回空
    static std::vector<uint8_t> random_bytes(std::size_t n);
};

}  // namespace falcon::swarm
