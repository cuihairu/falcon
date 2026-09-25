#pragma once

// ============================================================================
// SwarmKeyStore：节点身份私钥的加载与首次生成（load-or-create）
//
// 文件形态 = PEM（PKCS#8 未加密 PrivateKeyInfo）；内存形态 = 原始 32 字节
// 种子 + DER(SPKI) 公钥（SwarmCrypto 编码定案）。文件已存在但解析失败时
// 报错返回（绝不静默覆盖——私钥即节点身份，覆盖等于换身份）。
//
// 首次生成后 POSIX 侧收权 0600（owner-read/write）；Windows 侧依赖用户
// profile 目录的默认 ACL，不做额外处理。
// ============================================================================

#include <cstdint>
#include <string>
#include <vector>

namespace falcon::swarm {

struct SwarmKeyMaterial {
    std::vector<uint8_t> private_seed;    // 原始 32 字节种子
    std::vector<uint8_t> public_key_der;  // DER(SPKI)，恒 44 字节
    std::string node_id;                  // fingerprint(pubkey) 前 32 字符
    std::string pubkey_hex;               // DER(SPKI) 小写 hex（88 字符）

    bool valid() const {
        return private_seed.size() == 32 && !public_key_der.empty() &&
               !node_id.empty() && !pubkey_hex.empty();
    }
};

/// 目录不存在则创建（含父目录）；文件不存在则生成新身份并写入 PEM。
/// 失败（无目录写权限/PEM 解析失败/密码学错误）返回 invalid material，
/// error 置非空消息。
SwarmKeyMaterial load_or_create_swarm_key(const std::string& pem_path,
                                          std::string* error = nullptr);

}  // namespace falcon::swarm
