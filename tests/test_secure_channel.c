/**
 * S1 加密通道测试（双向往返 + 方向隔离 + 防重放 + 篡改检测）
 * 编译：gcc -o /tmp/test_sc test_sc.c src/security/secure_channel.c \
 *        src/security/crypto/crypto_core.c src/lib/crypto/monocypher.c src/lib/log_extra.c ... 
 * 简化：直接链接已编译目标或用 -fsyntax 全编
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

/* 声明（避免拉全部依赖） */
typedef struct secure_channel secure_channel_t;
secure_channel_t *sc_create(void);
void sc_destroy(secure_channel_t *ch);
const uint8_t *sc_local_public(const secure_channel_t *ch);
int sc_handshake(secure_channel_t *ch, const uint8_t *peer_public, const uint8_t *salt, size_t salt_len);
int sc_is_ready(const secure_channel_t *ch);
void sc_set_direction(secure_channel_t *ch, uint32_t send_dir, uint32_t recv_dir);
int sc_encrypt(secure_channel_t *ch, const uint8_t *in, size_t in_len, uint8_t *out, size_t *out_len, size_t out_cap);
int sc_decrypt(secure_channel_t *ch, const uint8_t *in, size_t in_len, uint8_t *out, size_t *out_len, size_t out_cap);

static int fails = 0;
#define CHECK(cond, name) do { \
    if (cond) printf("  ✓ %s\n", name); \
    else { printf("  ✗ %s\n", name); fails++; } \
} while (0)

int main(void) {
    printf("=== S1 加密通道测试 ===\n\n");

    /* 双向模拟：客户端 / 服务端 */
    secure_channel_t *cli = sc_create();
    secure_channel_t *srv = sc_create();
    CHECK(cli && srv, "创建两个通道（X25519 密钥对生成）");
    if (!cli || !srv) return 1;

    /* 约定 salt = cli_salt || srv_salt（双方一致） */
    uint8_t cli_salt[16] = {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16};
    uint8_t srv_salt[16] = {16,15,14,13,12,11,10,9,8,7,6,5,4,3,2,1};
    uint8_t salt[32];
    memcpy(salt, cli_salt, 16);
    memcpy(salt + 16, srv_salt, 16);

    /* 握手（双向交换公钥） */
    int rc1 = sc_handshake(cli, sc_local_public(srv), salt, 32);
    int rc2 = sc_handshake(srv, sc_local_public(cli), salt, 32);
    CHECK(rc1 == 0 && rc2 == 0, "双向 X25519 握手");
    CHECK(sc_is_ready(cli) && sc_is_ready(srv), "通道就绪");

    /* 方向约定：客户端 send=1 recv=2；服务端 send=2 recv=1 */
    sc_set_direction(cli, 1, 2);
    sc_set_direction(srv, 2, 1);

    /* ① 客户端 → 服务端 */
    const char *msg1 = "加密测试消息-клиент→сервер-🎯";
    uint8_t enc1[512]; size_t elen1 = 0;
    int e1 = sc_encrypt(cli, (const uint8_t *)msg1, strlen(msg1), enc1, &elen1, sizeof(enc1));
    CHECK(e1 == 0 && elen1 == strlen(msg1) + 16, "客户端加密（密文=明文+16 标签）");

    uint8_t dec1[512]; size_t dlen1 = 0;
    int d1 = sc_decrypt(srv, enc1, elen1, dec1, &dlen1, sizeof(dec1));
    CHECK(d1 == 0 && dlen1 == strlen(msg1) && memcmp(dec1, msg1, dlen1) == 0,
          "服务端解密往返一致（UTF-8 中文测试）");

    /* ② 服务端 → 客户端（反方向——验证方向隔离） */
    const char *msg2 = "server-to-client-reply";
    uint8_t enc2[512]; size_t elen2 = 0;
    sc_encrypt(srv, (const uint8_t *)msg2, strlen(msg2), enc2, &elen2, sizeof(enc2));
    uint8_t dec2[512]; size_t dlen2 = 0;
    int d2 = sc_decrypt(cli, enc2, elen2, dec2, &dlen2, sizeof(dec2));
    CHECK(d2 == 0 && dlen2 == strlen(msg2) && memcmp(dec2, msg2, dlen2) == 0,
          "反向解密（客户端收服务端帧）");

    /* ③ 方向混淆测试（客户端密文冒充服务端帧——应被拒） */
    uint8_t enc3[512]; size_t elen3 = 0;
    sc_encrypt(cli, (const uint8_t *)"x", 1, enc3, &elen3, sizeof(enc3));
    uint8_t tmp[512]; size_t tlen = 0;
    int d3 = sc_decrypt(cli, enc3, elen3, tmp, &tlen, sizeof(tmp));
    CHECK(d3 != 0, "方向隔离（客户端密文不能在本端解密——nonce 不同）");

    /* ④ 篡改检测 */
    uint8_t enc4[512]; size_t elen4 = 0;
    sc_encrypt(cli, (const uint8_t *)"integrity-test", 14, enc4, &elen4, sizeof(enc4));
    enc4[5] ^= 0xFF;
    int d4 = sc_decrypt(srv, enc4, elen4, tmp, &tlen, sizeof(tmp));
    CHECK(d4 != 0, "篡改检测（翻转字节 → AEAD 认证失败）");

    /* ⑤ 重放测试（服务端已接受 seq=0）——重放初始帧应被拒 */
    uint8_t enc5[512]; size_t elen5 = 0;
    sc_encrypt(cli, (const uint8_t *)"replay-a", 8, enc5, &elen5, sizeof(enc5));
    sc_decrypt(srv, enc5, elen5, tmp, &tlen, sizeof(tmp));  /* 首次接受 */
    int d5 = sc_decrypt(srv, enc5, elen5, tmp, &tlen, sizeof(tmp));  /* 重放 */
    CHECK(d5 != 0, "防重放（同一帧第二次提交被拒）");

    sc_destroy(cli);
    sc_destroy(srv);

    printf("\n结果: %s（失败 %d 项）\n", fails == 0 ? "✓ 全部通过" : "✗ 有失败", fails);
    return fails;
}
