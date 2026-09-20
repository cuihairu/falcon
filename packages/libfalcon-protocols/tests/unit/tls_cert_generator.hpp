/**
 * @file tls_cert_generator.hpp
 * @brief TLS 回环测试共享：运行时自签证书生成（http_commands_tls_test
 *        与 http_commands_proxy_test 共用）
 * @author Falcon Team
 * @date 2026-09-13
 */

#pragma once

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <openssl/pem.h>
#include <openssl/bio.h>

#include <string>

namespace falcon_test_tls {

/**
 * @brief 运行时生成自签证书（RSA 2048，v3：CA:TRUE + SAN
 *        DNS:localhost / IP:127.0.0.1），PEM 落盘
 *
 * CA:TRUE 使该证书可直接作为信任锚（SSL_CERT_FILE 指向即验签通过）；
 * SAN 双条目覆盖 DNS 与 IP 两种主机名校验路径。
 * 使用 1.1.0+ 且 3.x 不弃用的便携 API（EVP_PKEY_keygen 等），三平台
 * 零编译警告。
 */
inline bool generate_self_signed_cert(const std::string& key_path,
                                      const std::string& cert_path) {
    bool ok = false;
    EVP_PKEY* pkey = nullptr;
    X509* x509 = nullptr;
    EVP_PKEY_CTX* kctx = nullptr;

    do {
        kctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
        if (!kctx || EVP_PKEY_keygen_init(kctx) != 1 ||
            EVP_PKEY_CTX_set_rsa_keygen_bits(kctx, 2048) != 1) {
            break;
        }
        if (EVP_PKEY_keygen(kctx, &pkey) != 1) {
            break;
        }

        x509 = X509_new();
        if (!x509) {
            break;
        }
        ASN1_INTEGER_set(X509_get_serialNumber(x509), 1);
        X509_gmtime_adj(X509_getm_notBefore(x509), 0);
        X509_gmtime_adj(X509_getm_notAfter(x509), 24 * 3600);
        X509_set_version(x509, 2);  // v3：SAN/基本约束扩展需要
        X509_set_pubkey(x509, pkey);

        X509_NAME* name = X509_get_subject_name(x509);
        X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                                   reinterpret_cast<const unsigned char*>(
                                       "localhost"),
                                   -1, -1, 0);
        X509_set_issuer_name(x509, name);

        // basicConstraints CA:TRUE（自签证书可作信任锚）
        BASIC_CONSTRAINTS* bc = BASIC_CONSTRAINTS_new();
        if (!bc) {
            break;
        }
        bc->ca = 1;
        X509_EXTENSION* bc_ext = X509V3_EXT_i2d(NID_basic_constraints, 0, bc);
        BASIC_CONSTRAINTS_free(bc);
        if (!bc_ext || X509_add_ext(x509, bc_ext, -1) != 1) {
            X509_EXTENSION_free(bc_ext);
            break;
        }
        X509_EXTENSION_free(bc_ext);

        // subjectAltName：DNS localhost + IP 127.0.0.1
        GENERAL_NAMES* san = GENERAL_NAMES_new();
        if (!san) {
            break;
        }
        GENERAL_NAME* dns = GENERAL_NAME_new();
        ASN1_IA5STRING* dns_str = ASN1_IA5STRING_new();
        ASN1_STRING_set(dns_str, "localhost", -1);
        dns->type = GEN_DNS;
        dns->d.dNSName = dns_str;
        (void)sk_GENERAL_NAME_push(san, dns);

        GENERAL_NAME* ip = GENERAL_NAME_new();
        ASN1_OCTET_STRING* ip_str = ASN1_OCTET_STRING_new();
        static const unsigned char kLoopback[4] = {127, 0, 0, 1};
        ASN1_STRING_set(ip_str, kLoopback, sizeof(kLoopback));
        ip->type = GEN_IPADD;
        ip->d.iPAddress = ip_str;
        (void)sk_GENERAL_NAME_push(san, ip);

        // IPv6 回环（16 字节二进制形态）——v6 IP 直连的 IP SAN 匹配
        GENERAL_NAME* ip6 = GENERAL_NAME_new();
        ASN1_OCTET_STRING* ip6_str = ASN1_OCTET_STRING_new();
        unsigned char ip6_bytes[16] = {};
        ip6_bytes[15] = 1;
        ASN1_STRING_set(ip6_str, ip6_bytes, sizeof(ip6_bytes));
        ip6->type = GEN_IPADD;
        ip6->d.iPAddress = ip6_str;
        (void)sk_GENERAL_NAME_push(san, ip6);

        X509_EXTENSION* san_ext = X509V3_EXT_i2d(NID_subject_alt_name, 0, san);
        GENERAL_NAMES_free(san);
        if (!san_ext || X509_add_ext(x509, san_ext, -1) != 1) {
            X509_EXTENSION_free(san_ext);
            break;
        }
        X509_EXTENSION_free(san_ext);

        if (X509_sign(x509, pkey, EVP_sha256()) == 0) {
            break;
        }

        BIO* key_bio = BIO_new_file(key_path.c_str(), "w");
        if (!key_bio) {
            break;
        }
        const bool key_ok =
            PEM_write_bio_PrivateKey(key_bio, pkey, nullptr, nullptr, 0,
                                     nullptr, nullptr) == 1;
        BIO_free(key_bio);

        BIO* cert_bio = BIO_new_file(cert_path.c_str(), "w");
        if (!cert_bio) {
            break;
        }
        const bool cert_ok = PEM_write_bio_X509(cert_bio, x509) == 1;
        BIO_free(cert_bio);

        ok = key_ok && cert_ok;
    } while (false);

    if (x509) X509_free(x509);
    if (pkey) EVP_PKEY_free(pkey);
    if (kctx) EVP_PKEY_CTX_free(kctx);
    if (!ok) {
        ERR_clear_error();
    }
    return ok;
}

/**
 * @brief 运行时生成自签客户端证书（RSA 2048，CN=falcon-client，
 *        终端实体：无 CA 约束、无 SAN），PEM 落盘
 *
 * 供双向 TLS（mTLS）用例：服务器把该证书文件本身作为信任锚
 *（SSL_CTX_load_verify_locations——自签证书即自身锚）并要求
 * SSL_VERIFY_PEER，客户端出示后握手才成立。
 */
inline bool generate_client_cert(const std::string& key_path,
                                 const std::string& cert_path) {
    bool ok = false;
    EVP_PKEY* pkey = nullptr;
    X509* x509 = nullptr;
    EVP_PKEY_CTX* kctx = nullptr;

    do {
        kctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
        if (!kctx || EVP_PKEY_keygen_init(kctx) != 1 ||
            EVP_PKEY_CTX_set_rsa_keygen_bits(kctx, 2048) != 1) {
            break;
        }
        if (EVP_PKEY_keygen(kctx, &pkey) != 1) {
            break;
        }

        x509 = X509_new();
        if (!x509) {
            break;
        }
        ASN1_INTEGER_set(X509_get_serialNumber(x509), 1);
        X509_gmtime_adj(X509_getm_notBefore(x509), 0);
        X509_gmtime_adj(X509_getm_notAfter(x509), 24 * 3600);
        X509_set_version(x509, 2);
        X509_set_pubkey(x509, pkey);

        X509_NAME* name = X509_get_subject_name(x509);
        X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                                   reinterpret_cast<const unsigned char*>(
                                       "falcon-client"),
                                   -1, -1, 0);
        X509_set_issuer_name(x509, name);

        if (X509_sign(x509, pkey, EVP_sha256()) == 0) {
            break;
        }

        BIO* key_bio = BIO_new_file(key_path.c_str(), "w");
        if (!key_bio) {
            break;
        }
        const bool key_ok =
            PEM_write_bio_PrivateKey(key_bio, pkey, nullptr, nullptr, 0,
                                     nullptr, nullptr) == 1;
        BIO_free(key_bio);

        BIO* cert_bio = BIO_new_file(cert_path.c_str(), "w");
        if (!cert_bio) {
            break;
        }
        const bool cert_ok = PEM_write_bio_X509(cert_bio, x509) == 1;
        BIO_free(cert_bio);

        ok = key_ok && cert_ok;
    } while (false);

    if (x509) X509_free(x509);
    if (pkey) EVP_PKEY_free(pkey);
    if (kctx) EVP_PKEY_CTX_free(kctx);
    if (!ok) {
        ERR_clear_error();
    }
    return ok;
}

}  // namespace falcon_test_tls
