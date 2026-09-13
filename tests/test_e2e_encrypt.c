/**
 * S1 TCP 端到端加密测试客户端
 *   流程：连接 → 读日志取验证码 → MSG_AUTH_CODE → 读日志取连接码 →
 *         MSG_CONNECTION_CODE → 收 token → MSG_KEY_EXCHANGE →
 *         加密心跳 → 解密 ACK
 * 用法: ./test_e2e <lingosd日志文件路径>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <ctype.h>
#include <time.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/* secure_channel 接口 */
typedef struct secure_channel secure_channel_t;
secure_channel_t *sc_create(void);
void sc_destroy(secure_channel_t *ch);
const uint8_t *sc_local_public(const secure_channel_t *ch);
int sc_handshake(secure_channel_t *ch, const uint8_t *peer_public, const uint8_t *salt, size_t salt_len);
int sc_is_ready(const secure_channel_t *ch);
void sc_set_direction(secure_channel_t *ch, uint32_t send_dir, uint32_t recv_dir);
int sc_encrypt(secure_channel_t *ch, const uint8_t *in, size_t in_len, uint8_t *out, size_t *out_len, size_t out_cap);
int sc_decrypt(secure_channel_t *ch, const uint8_t *in, size_t in_len, uint8_t *out, size_t *out_len, size_t out_cap);
int sc_negotiate(uint32_t local_caps, uint32_t peer_caps);
int sc_should_encrypt(uint32_t negotiated);

/* 随机源（链接 crypto_core） */
int crypto_random_bytes(uint8_t *buf, size_t len);

#define MAGIC 0x4C4E4753u
#define VER   0x0001

static int fails = 0;
#define CHECK(cond, name) do { \
    if (cond) printf("  ✓ %s\n", name); \
    else { printf("  ✗ %s\n", name); fails++; } \
} while (0)

/* ---------- TLV 帧 ---------- */
static int send_frame(int fd, uint16_t type, const uint8_t *payload, uint32_t plen) {
    uint8_t buf[70000];
    uint32_t magic = htonl(MAGIC);
    uint16_t ver = htons(VER), t = htons(type);
    uint32_t len = htonl(plen);
    memcpy(buf, &magic, 4);
    memcpy(buf + 4, &ver, 2);
    memcpy(buf + 6, &t, 2);
    memcpy(buf + 8, &len, 4);
    if (plen) memcpy(buf + 12, payload, plen);
    size_t total = 12 + plen;
    size_t sent = 0;
    while (sent < total) {
        ssize_t n = send(fd, buf + sent, total - sent, 0);
        if (n <= 0) return -1;
        sent += (size_t)n;
    }
    return 0;
}

static int recv_frame(int fd, uint16_t *type, uint8_t *payload, uint32_t *plen, int timeout_s) {
    struct timeval tv = {timeout_s, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    uint8_t hdr[12];
    size_t got = 0;
    while (got < 12) {
        ssize_t n = recv(fd, hdr + got, 12 - got, 0);
        if (n <= 0) return -1;
        got += (size_t)n;
    }
    uint32_t magic;
    memcpy(&magic, hdr, 4);
    if (ntohl(magic) != MAGIC) return -1;
    uint16_t rawt;
    memcpy(&rawt, hdr + 6, 2);
    *type = ntohs(rawt);
    uint32_t len;
    memcpy(&len, hdr + 8, 4);
    len = ntohl(len);
    if (len > 65536) return -1;
    *plen = len;
    got = 0;
    while (got < len) {
        ssize_t n = recv(fd, payload + got, len - got, 0);
        if (n <= 0) return -1;
        got += (size_t)n;
    }
    return 0;
}

/* ---------- 从日志提取码（跳过 ANSI 转义；NUL 安全） ---------- */
/* 找最后一个 marker（memcmp 全缓冲扫描——日志可能含 NUL 字节，strstr 会提前停） */
static char *rfind_marker(char *hay, size_t hay_len, const char *needle) {
    size_t nl = strlen(needle);
    if (!nl || hay_len < nl) return NULL;
    char *found = NULL;
    for (size_t i = 0; i + nl <= hay_len; i++) {
        if (memcmp(hay + i, needle, nl) == 0) found = hay + i;
    }
    return found;
}

static int extract_code(const char *log_path, const char *marker, char *out, size_t out_len) {
    FILE *f = fopen(log_path, "rb");
    if (!f) return -1;
    static char buf[262144];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';

    char *last = rfind_marker(buf, n, marker);
    if (!last) return -1;

    char *p = last + strlen(marker);
    size_t oi = 0;
    size_t remain = (size_t)(buf + n - p);
    while (remain > 0 && oi < out_len - 1) {
        if ((unsigned char)*p == 0x1b) {
            /* 跳过 ESC 序列到字母终止符 */
            while (remain > 0 && !isalpha((unsigned char)*p)) { p++; remain--; }
            if (remain > 0) { p++; remain--; }   /* 跳过终止字母 */
            continue;
        }
        if (isalnum((unsigned char)*p) || *p == '-') {
            out[oi++] = *p++;
            remain--;
            continue;
        }
        if (oi > 0) break;   /* 已捕获后再遇非码字符 → 结束 */
        p++;
        remain--;
    }
    out[oi] = '\0';
    return oi > 0 ? 0 : -1;
}

static int wait_code(const char *log_path, const char *marker, char *out, size_t out_len, int timeout_s) {
    for (int i = 0; i < timeout_s * 4; i++) {
        if (extract_code(log_path, marker, out, out_len) == 0) return 0;
        usleep(250 * 1000);
    }
    return -1;
}

int main(int argc, char **argv) {
    if (argc < 2) { printf("用法: %s <lingosd日志>\n", argv[0]); return 1; }
    const char *log_path = argv[1];

    printf("=== S1 TCP 端到端加密测试 ===\n\n");

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(2937);
    inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr);
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        printf("✗ 无法连接 127.0.0.1:2937\n");
        return 1;
    }
    printf("  ✓ 已连接 TCP 2937\n\n");

    /* ① 读验证码（服务器 accept 时打印） */
    char auth_code[64] = {0};
    if (wait_code(log_path, "验证码：", auth_code, sizeof(auth_code), 5) != 0 &&
        wait_code(log_path, "Auth Code: ", auth_code, sizeof(auth_code), 2) != 0) {
        printf("  ✗ 未能从日志读取验证码\n");
        return 1;
    }
    printf("  ✓ 读到验证码: %s\n", auth_code);

    /* ② 发 MSG_AUTH_CODE */
    CHECK(send_frame(fd, 0x0001, (const uint8_t *)auth_code, (uint32_t)strlen(auth_code)) == 0,
          "发送验证码帧");

    /* ②b 收 MSG_AUTH_RESPONSE（服务器验证后应答——须在发连接码前读走） */
    uint16_t rt = 0;
    uint32_t rl = 0;
    static uint8_t rbuf[65536];
    if (recv_frame(fd, &rt, rbuf, &rl, 8) != 0 || rt != 0x0002) {
        printf("  ✗ 未收到验证应答（type=0x%04X）\n", rt);
        return 1;
    }
    rbuf[rl] = '\0';
    printf("  ✓ 验证应答: %.80s\n", (char *)rbuf);

    /* ③ 读连接码（服务器验证后打印） */
    char conn_code[64] = {0};
    if (wait_code(log_path, "连接码：", conn_code, sizeof(conn_code), 5) != 0 &&
        wait_code(log_path, "Connection Code: ", conn_code, sizeof(conn_code), 2) != 0) {
        printf("  ✗ 未能从日志读取连接码\n");
        return 1;
    }
    printf("  ✓ 读到连接码: %s\n", conn_code);

    /* ④ 发 MSG_CONNECTION_CODE */
    CHECK(send_frame(fd, 0x0003, (const uint8_t *)conn_code, (uint32_t)strlen(conn_code)) == 0,
          "发送连接码帧");

    /* ⑤ 收 MSG_CONNECTION_RESPONSE（token + encrypted 字段；跳过杂帧） */
    int got_conn = 0;
    for (int i = 0; i < 5; i++) {
        if (recv_frame(fd, &rt, rbuf, &rl, 8) != 0) break;
        if (rt == 0x0004) { got_conn = 1; break; }
    }
    if (!got_conn) {
        printf("  ✗ 未收到连接响应（type=0x%04X）\n", rt);
        return 1;
    }
    rbuf[rl] = '\0';
    printf("  ✓ 连接响应: %s\n", (char *)rbuf);
    CHECK(strstr((char *)rbuf, "\"status\":\"ok\"") != NULL, "会话建立（status=ok）");
    CHECK(strstr((char *)rbuf, "\"encrypted\":false") != NULL,
          "诚实上报（协商前 encrypted=false）");

    /* ⑥ 加密握手：MSG_KEY_EXCHANGE = caps(4BE) + pubkey(32) + salt(16) */
    secure_channel_t *ch = sc_create();
    if (!ch) { printf("  ✗ 创建加密通道失败\n"); return 1; }
    uint8_t kx[52];
    kx[0] = 0; kx[1] = 0; kx[2] = 0; kx[3] = 0x01;   /* caps = SC_CAP_ENCRYPT */
    memcpy(kx + 4, sc_local_public(ch), 32);
    uint8_t cli_salt[16];
    crypto_random_bytes(cli_salt, 16);
    memcpy(kx + 36, cli_salt, 16);
    CHECK(send_frame(fd, 0x000B, kx, 52) == 0, "发送密钥交换（X25519 公钥）");

    /* ⑦ 收服务端密钥交换响应 */
    if (recv_frame(fd, &rt, rbuf, &rl, 8) != 0 || rt != 0x000B || rl < 52) {
        printf("  ✗ 密钥交换响应异常（type=0x%04X len=%u）\n", rt, rl);
        return 1;
    }
    uint32_t srv_caps = ((uint32_t)rbuf[0] << 24) | ((uint32_t)rbuf[1] << 16) |
                        ((uint32_t)rbuf[2] << 8) | (uint32_t)rbuf[3];
    printf("  ✓ 服务端响应 caps=0x%02X\n", srv_caps);
    const uint8_t *srv_pub = rbuf + 4;
    const uint8_t *srv_salt = rbuf + 36;

    /* 双方一致 salt = client_salt || server_salt */
    uint8_t salt[32];
    memcpy(salt, cli_salt, 16);
    memcpy(salt + 16, srv_salt, 16);
    int hs = sc_handshake(ch, srv_pub, salt, sizeof(salt));
    CHECK(hs == 0 && sc_is_ready(ch), "X25519 握手完成（会话密钥派生）");
    sc_set_direction(ch, 1, 2);   /* 客户端发送=1；接收（服务端发送）=2 */

    uint32_t neg = sc_negotiate(0x01, srv_caps);
    CHECK(sc_should_encrypt(neg), "能力协商：加密启用");

    /* ⑧ 加密心跳（带载荷 → 走 AEAD） */
    const char *hb = "ping-encrypted-测试";
    uint8_t enc[512];
    size_t elen = 0;
    int er = sc_encrypt(ch, (const uint8_t *)hb, strlen(hb), enc, &elen, sizeof(enc));
    CHECK(er == 0, "客户端加密心跳");
    CHECK(send_frame(fd, 0x0008, enc, (uint32_t)elen) == 0, "发送加密心跳帧");

    /* ⑨ 收加密 ACK 并解密 */
    if (recv_frame(fd, &rt, rbuf, &rl, 8) != 0 || rt != 0x0009) {
        printf("  ✗ 未收到心跳 ACK（type=0x%04X）\n", rt);
        /* 可能是错误帧：打印 */
        if (rt == 0x000A) { rbuf[rl] = '\0'; printf("    错误帧: %s\n", (char *)rbuf); }
        return 1;
    }
    printf("  ✓ 收到加密 ACK（密文 %u 字节）\n", rl);
    uint8_t dec[1024];
    size_t dlen = 0;
    int dr = sc_decrypt(ch, rbuf, rl, dec, &dlen, sizeof(dec));
    CHECK(dr == 0, "解密服务端 ACK（服务端发送方向=2）");
    if (dr == 0) {
        dec[dlen] = '\0';
        printf("    解密内容: %s\n", (char *)dec);
        CHECK(strstr((char *)dec, "\"status\":\"ok\"") != NULL, "ACK 内容正确");
    }

    /* ⑩ 负向：明文帧（未加密）应被服务端拒绝 */
    const char *plain = "plaintext-should-fail";
    (void)send_frame(fd, 0x0002, (const uint8_t *)plain, (uint32_t)strlen(plain));
    if (recv_frame(fd, &rt, rbuf, &rl, 5) == 0) {
        CHECK(rt == 0x000A, "明文帧被拒（收到错误帧——AEAD 防线生效）");
    } else {
        printf("  （注：未收到错误帧——连接可能已关闭）\n");
    }

    sc_destroy(ch);
    close(fd);
    printf("\n结果: %s（失败 %d 项）\n", fails == 0 ? "✓ 端到端全部通过" : "✗ 有失败", fails);
    return fails;
}
