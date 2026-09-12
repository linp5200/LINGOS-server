/**
 * @file    src/update/update_verify.h
 * @brief   更新包签名校验（先生 2026-09-12 裁决 S12）
 * @version LN-0.5.0
 *
 * ⚠️ 背景（审计发现）：
 *   原更新机制仅有 sha256 校验，但 **hash 与文件在同一 JSON 中声明**
 *   → 攻击者篡改文件同时改 hash 即可绕过。
 *   即「只能防传输损坏，不能防恶意篡改」。
 *
 * ✅ 现方案（Ed25519 数字签名，Monocypher 提供原语）：
 *   · 发布方用**私钥**对 manifest 签名 → 生成 manifest.json.sig
 *   · 客户端用**内置公钥**验签 → 通过才应用更新
 *   · 私钥不进入系统；公钥预置于 /LINGOS/system/config/update_pubkey
 *
 * 与 sha256 的关系：**两者都要**（签名保证来源可信 + hash 保证内容完整）
 */

#ifndef UPDATE_VERIFY_H
#define UPDATE_VERIFY_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UPDATE_SIG_SIZE      64   /* Ed25519 签名 */
#define UPDATE_PUBKEY_SIZE   32   /* Ed25519 公钥 */

/**
 * @brief 校验文件签名
 * @param file_path   被签名的文件（如 manifest.json）
 * @param sig_path    签名文件（如 manifest.json.sig，64 字节原始二进制）
 * @param pubkey_path 公钥文件（32 字节）；NULL → 用默认路径
 * @return 0 通过；-1 失败（签名无效/文件缺失/公钥缺失）
 */
int update_verify_file(const char *file_path, const char *sig_path,
                       const char *pubkey_path);

/**
 * @brief 校验内存缓冲签名（供内部调用）
 * @return 0 通过；-1 失败
 */
int update_verify_buffer(const uint8_t *data, size_t data_len,
                         const uint8_t *sig, size_t sig_len,
                         const uint8_t *pubkey, size_t pubkey_len);

/**
 * @brief 签发（**仅供构建/发布工具使用**，不进入运行时）
 * @param file_path   待签文件
 * @param privkey_path 私钥文件（32 字节 seed）
 * @param sig_out     输出签名文件路径
 * @return 0 成功；-1 失败
 */
int update_sign_file(const char *file_path, const char *privkey_path,
                     const char *sig_out);

/**
 * @brief 加载公钥（默认 /LINGOS/system/config/update_pubkey）
 * @return 0 成功；-1 缺失或长度错误
 */
int update_load_pubkey(uint8_t out[UPDATE_PUBKEY_SIZE]);

/**
 * @brief 检查更新包是否已签名且校验通过（用于更新流程前置门）
 * @param manifest_path manifest 路径
 * @return 1 通过；0 未通过（**拒绝应用更新**）
 * @note  签名开关受 option "sec.update_signature" 控制；
 *        未开启时记录警告但仍放行（兼容无签名更新包）。
 */
int update_signature_gate(const char *manifest_path);

#ifdef __cplusplus
}
#endif

#endif /* UPDATE_VERIFY_H */
