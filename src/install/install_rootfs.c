/**
 * @file    src/install/install_rootfs.c
 * @brief   rootfs 真下载引擎实现（真下载 / 真校验 / 原子替换 / 失败回滚）
 * @version LN-0.9.0
 * @par     核心协议：诚实红线 + 容错编程（失败即明确失败，绝不留半成品/假成功）
 *
 * 全链（每一步都有真实系统调用与校验）：
 *   ① 镜像解析：内置固定版本清单（官方 digest 实测抓取）+ 用户清单覆盖
 *      <data_root>/system/config/rootfs_manifest.json
 *   ② 出口白名单：egress_check_url（src/net/egress.c）—— 未放行 = 明确报错（不绕过）
 *   ③ 下载：http_download（dlopen libcurl 支持 https/重定向；无 curl 走 socket）
 *      写入 <archive>.part.<pid>（半成品绝不落在目标路径）
 *   ④ 校验：字节数（清单提供时）+ SHA-256（sha256sum，与 src/update 同法）
 *      校验失败 → 删除半成品 + 旧归档原样保留（回滚 = 无变更）
 *   ⑤ 原子替换：rename（含旧归档备份/失败回滚）
 *   ⑥ 解压：tar 参数化执行（safe_exec_run，不经 shell）到 .staging-<distro>-<pid>
 *      解压后目录结构自检（bin/etc/usr 至少一项）
 *   ⑦ 原子切换：旧目录 → .bak，staging → 正式目录；失败自动恢复旧目录
 *   ⑧ 登记：<data_root>/system/config/rootfs_installed.json（原子写）
 *      + proot 二进制探测（找到即登记，未找到如实记录"待配置"）
 */

#include "install_rootfs.h"
#include "../common/safe_string.h"
#include "../common/lang.h"
#include "../common/data_path.h"
#include "../lib/log_extra.h"
#include "../lib/cJSON/cJSON.h"
#include "../net/http_client.h"
#include "../net/egress.h"
#include "../security/safe_exec.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <ftw.h>

#define ROOTFS_DL_TIMEOUT_S   1800      /* 整文件传输上限（30 分钟） */
#define ROOTFS_MAX_OVERRIDES  8
#define ROOTFS_PATH_SZ        512
#define ROOTFS_PATH_LONG      768

/* ============================================================
 * 内置镜像清单（**固定版本 + 官方 digest**）
 *   digest/size 采集时间：2026-10-16
 *     alpine: dl-cdn.alpinelinux.org/alpine/v3.20/releases/<arch>/<image>.sha256
 *     ubuntu: cdimage.ubuntu.com/ubuntu-base/releases/24.04/release/SHA256SUMS
 *   size = 官方 Content-Length；-1 表示官方未提供（诚实：跳过大小校验，
 *          SHA-256 仍是权威校验，二者不互相替代）
 * ============================================================ */
typedef struct {
    const char *distro;
    const char *arch;            /* 归一化架构（uname -m 归一） */
    const char *version;
    const char *url;
    const char *sha256;          /* 官方 digest（小写十六进制） */
    long long   size;            /* -1 = 未知 */
    int         strip_components;
    const char *format;
} rootfs_builtin_t;

static const rootfs_builtin_t g_builtin[] = {
    { "alpine", "aarch64", "3.20.9",
      "https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/aarch64/alpine-minirootfs-3.20.9-aarch64.tar.gz",
      "7e8b4adbf33b1363b90b50e614bbe7bf3d2c8863f9f1718cbc65a883b27c2f43",
      3949657, 0, "tar.gz" },
    { "alpine", "x86_64", "3.20.9",
      "https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/alpine-minirootfs-3.20.9-x86_64.tar.gz",
      "ad094d86f5ae5189d4236524e8ebe042c42b3d4d8555825664cea34e103886ff",
      3492459, 0, "tar.gz" },
    { "ubuntu", "aarch64", "24.04.5",
      "https://cdimage.ubuntu.com/ubuntu-base/releases/24.04/release/ubuntu-base-24.04.5-base-arm64.tar.gz",
      "04207713ece899c3740823d33690441ad3a7f0ded1101aca744e2b0f37ac7ff2",
      -1, 0, "tar.gz" },
    { "ubuntu", "x86_64", "24.04.5",
      "https://cdimage.ubuntu.com/ubuntu-base/releases/24.04/release/ubuntu-base-24.04.5-base-amd64.tar.gz",
      "a91d5a93010193712d346d761372b7c9db6dfcf093893161c64ca107f05914f2",
      -1, 0, "tar.gz" },
    { NULL, NULL, NULL, NULL, NULL, 0, 0, NULL }
};

/* 用户清单覆盖项（rootfs_manifest.json） */
typedef struct {
    char distro[32];
    char arch[16];
    char version[64];
    char url[512];
    char sha256[65];
    long long size;
    int strip_components;
    char format[16];
} rootfs_override_t;

static rootfs_override_t g_overrides[ROOTFS_MAX_OVERRIDES];
static int g_override_count = 0;
static int g_overrides_loaded = 0;

/* 解析结果（定长，避免 malloc 生命周期问题） */
typedef struct {
    char distro[32];
    char arch[16];
    char version[64];
    char url[512];
    char sha256[65];
    char filename[160];
    char format[16];
    long long size;
    int strip_components;
} rootfs_resolved_t;

static char g_last_error[512] = {0};
static char g_last_sha256[65] = {0};
static char g_arch[32] = {0};

/* ============================================================
 * 基础工具
 * ============================================================ */

static void set_error(const char *stage, const char *fmt, ...) {
    char detail[400];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(detail, sizeof(detail), fmt, ap);
    va_end(ap);
    safe_snprintf(g_last_error, sizeof(g_last_error), "[%s] %s", stage ? stage : "?", detail);
}

const char *install_rootfs_get_last_error(void) { return g_last_error; }
const char *install_rootfs_last_sha256(void)    { return g_last_sha256; }

const char *install_rootfs_arch(void) {
    if (g_arch[0]) return g_arch;

    struct utsname u;
    memset(&u, 0, sizeof(u));
    if (uname(&u) != 0 || !u.machine[0]) {
        safe_strncpy(g_arch, "unknown", sizeof(g_arch));
        return g_arch;
    }
    if (strcmp(u.machine, "arm64") == 0)        safe_strncpy(g_arch, "aarch64", sizeof(g_arch));
    else if (strcmp(u.machine, "amd64") == 0)   safe_strncpy(g_arch, "x86_64", sizeof(g_arch));
    else                                        safe_strncpy(g_arch, u.machine, sizeof(g_arch));
    return g_arch;
}

/* 标识符安全校验（防路径穿越/命令拼接）：[A-Za-z0-9._+-]，长度 1..maxlen */
static int safe_token(const char *s, size_t maxlen) {
    if (!s || !s[0]) return 0;
    size_t n = strlen(s);
    if (n > maxlen) return 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (!(isalnum(c) || c == '.' || c == '_' || c == '+' || c == '-')) return 0;
    }
    if (strstr(s, "..")) return 0;
    return 1;
}

static int mkdir_p(const char *path) {
    if (!path || !path[0]) return -1;
    char tmp[ROOTFS_PATH_LONG];
    safe_strncpy(tmp, path, sizeof(tmp));
    size_t len = strlen(tmp);
    while (len > 1 && tmp[len - 1] == '/') tmp[--len] = '\0';
    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
        *p = '/';
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
    return 0;
}

/* 递归删除（nftw；不依赖 rm —— safe_exec 白名单不含 rm，符合最小权限） */
static int rm_rf_cb(const char *p, const struct stat *sb, int flag, struct FTW *ftw) {
    (void)sb;
    (void)ftw;
    if (flag == FTW_DP) return rmdir(p);
    if (flag == FTW_F || flag == FTW_SL || flag == FTW_SLN) return unlink(p);
    return 0;
}

static void rm_rf(const char *path) {
    if (!path || !path[0]) return;
    if (access(path, F_OK) != 0) return;
    if (nftw(path, rm_rf_cb, 32, FTW_DEPTH | FTW_PHYS) != 0) {
        LOG_WARN_T("Rootfs", "RmRf", "Partial", "nftw could not fully remove %s: %s",
                   path, strerror(errno));
    }
}

static int file_size(const char *path, long long *out) {
    struct stat st;
    if (!path || stat(path, &st) != 0) return -1;
    if (!S_ISREG(st.st_mode)) return -1;
    if (out) *out = (long long)st.st_size;
    return 0;
}

/* 文件 SHA-256（系统 sha256sum —— 与 src/update/update_incremental_json.c:51 同法） */
static int file_sha256(const char *path, char *out, size_t out_sz) {
    if (!path || !out || out_sz < 65) return -1;
    if (strchr(path, '\'')) return -1;   /* 路径含引号 → 拒绝（不拼接不可信字符） */

    char cmd[ROOTFS_PATH_LONG + 64];
    int n = safe_snprintf(cmd, sizeof(cmd), "sha256sum '%s' 2>/dev/null", path);
    if (n <= 0 || (size_t)n >= sizeof(cmd)) return -1;

    FILE *p = popen(cmd, "r");
    if (!p) return -1;
    char buf[256];
    size_t got = fread(buf, 1, sizeof(buf) - 1, p);
    int status = pclose(p);
    buf[got] = '\0';
    if (status != 0 || got < 64) return -1;

    for (int i = 0; i < 64; i++) {
        unsigned char c = (unsigned char)buf[i];
        if (!isxdigit(c)) return -1;
        out[i] = (char)tolower(c);
    }
    out[64] = '\0';
    return 0;
}

/* ============================================================
 * 路径
 * ============================================================ */

static void rootfs_cache_dir(char *out, size_t sz) {
    safe_snprintf(out, sz, "%s/data/rootfs/cache", lingos_data_root());
}
static void rootfs_parent_dir(char *out, size_t sz) {
    safe_snprintf(out, sz, "%s/data/rootfs", lingos_data_root());
}
static void rootfs_registry_path(char *out, size_t sz) {
    safe_snprintf(out, sz, "%s/system/config/rootfs_installed.json", lingos_data_root());
}
static void rootfs_manifest_path(char *out, size_t sz) {
    safe_snprintf(out, sz, "%s/system/config/rootfs_manifest.json", lingos_data_root());
}
static void rootfs_egress_path(char *out, size_t sz) {
    safe_snprintf(out, sz, "%s/system/config/egress_allowlist.json", lingos_data_root());
}

/* ============================================================
 * 清单：内置 + 用户覆盖
 * ============================================================ */

static void load_overrides(void) {
    if (g_overrides_loaded) return;
    g_overrides_loaded = 1;
    g_override_count = 0;

    char path[ROOTFS_PATH_SZ];
    rootfs_manifest_path(path, sizeof(path));
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        LOG_DEBUG_T("Rootfs", "Manifest", "None", "no user manifest (%s) — builtin images only", path);
        return;
    }
    char text[64 * 1024];
    size_t got = fread(text, 1, sizeof(text) - 1, fp);
    fclose(fp);
    text[got] = '\0';
    if (got == 0) return;

    cJSON *root = cJSON_Parse(text);
    if (!root) {
        LOG_WARN_T("Rootfs", "Manifest", "ParseFail", "%s is not valid JSON — ignored (builtin only)", path);
        return;
    }
    const cJSON *images = cJSON_GetObjectItemCaseSensitive(root, "images");
    const cJSON *it = NULL;
    cJSON_ArrayForEach(it, images) {
        if (g_override_count >= ROOTFS_MAX_OVERRIDES) break;
        if (!cJSON_IsObject(it)) continue;
        const cJSON *jd = cJSON_GetObjectItemCaseSensitive(it, "distro");
        const cJSON *ju = cJSON_GetObjectItemCaseSensitive(it, "url");
        if (!cJSON_IsString(jd) || !cJSON_IsString(ju)) continue;
        if (!safe_token(jd->valuestring, 31)) continue;
        if (strncmp(ju->valuestring, "http", 4) != 0) continue;

        rootfs_override_t *o = &g_overrides[g_override_count];
        memset(o, 0, sizeof(*o));
        safe_strncpy(o->distro, jd->valuestring, sizeof(o->distro));
        safe_strncpy(o->url, ju->valuestring, sizeof(o->url));
        safe_strncpy(o->arch, "any", sizeof(o->arch));
        o->size = -1;
        safe_strncpy(o->format, "tar.gz", sizeof(o->format));

        const cJSON *ja = cJSON_GetObjectItemCaseSensitive(it, "arch");
        if (cJSON_IsString(ja) && ja->valuestring[0]) safe_strncpy(o->arch, ja->valuestring, sizeof(o->arch));
        const cJSON *jv = cJSON_GetObjectItemCaseSensitive(it, "version");
        if (cJSON_IsString(jv) && jv->valuestring[0]) safe_strncpy(o->version, jv->valuestring, sizeof(o->version));
        const cJSON *jh = cJSON_GetObjectItemCaseSensitive(it, "sha256");
        if (cJSON_IsString(jh) && jh->valuestring[0]) safe_strncpy(o->sha256, jh->valuestring, sizeof(o->sha256));
        const cJSON *js = cJSON_GetObjectItemCaseSensitive(it, "size");
        if (cJSON_IsNumber(js)) o->size = (long long)js->valuedouble;
        const cJSON *jsc = cJSON_GetObjectItemCaseSensitive(it, "strip_components");
        if (cJSON_IsNumber(jsc)) o->strip_components = jsc->valueint;
        const cJSON *jf = cJSON_GetObjectItemCaseSensitive(it, "format");
        if (cJSON_IsString(jf) && jf->valuestring[0]) safe_strncpy(o->format, jf->valuestring, sizeof(o->format));

        g_override_count++;
    }
    cJSON_Delete(root);
    LOG_INFO_T("Rootfs", "Manifest", "Loaded", "%d user image override(s) from %s", g_override_count, path);
}

/* URL → 文件名（严格字符校验） */
static int url_basename(const char *url, char *out, size_t out_sz) {
    if (!url || !out) return -1;
    const char *slash = strrchr(url, '/');
    if (!slash || !slash[1]) return -1;
    const char *name = slash + 1;
    if (!safe_token(name, 127)) return -1;
    if (strlen(name) >= out_sz) return -1;
    safe_strncpy(out, name, out_sz);
    return 0;
}

/* URL → host（出口白名单用） */
static int url_host(const char *url, char *out, size_t out_sz) {
    if (!url || !out) return -1;
    const char *p = strstr(url, "://");
    if (!p) return -1;
    p += 3;
    const char *end = p;
    while (*end && *end != '/' && *end != ':' && *end != '?') end++;
    size_t n = (size_t)(end - p);
    if (n == 0 || n >= out_sz) return -1;
    memcpy(out, p, n);
    out[n] = '\0';
    return 0;
}

/**
 * 解析发行版 → 镜像（用户清单优先，其次内置；架构需匹配）
 * @return ROOTFS_OK / ROOTFS_ERR_ARG / ROOTFS_ERR_UNKNOWN
 */
static int resolve_image(const char *distro, rootfs_resolved_t *out) {
    if (!distro || !out) return ROOTFS_ERR_ARG;
    if (!safe_token(distro, 31)) {
        set_error("arg", "invalid distro name '%s' (allowed: A-Za-z0-9._+-)", distro ? distro : "(null)");
        return ROOTFS_ERR_ARG;
    }
    memset(out, 0, sizeof(*out));
    load_overrides();

    const char *arch = install_rootfs_arch();

    /* ① 用户清单（arch 匹配或 "any"） */
    for (int i = 0; i < g_override_count; i++) {
        const rootfs_override_t *o = &g_overrides[i];
        if (strcmp(o->distro, distro) != 0) continue;
        if (strcmp(o->arch, "any") != 0 && strcmp(o->arch, arch) != 0) continue;
        safe_strncpy(out->distro, o->distro, sizeof(out->distro));
        safe_strncpy(out->arch, arch, sizeof(out->arch));
        safe_strncpy(out->version, o->version[0] ? o->version : "user-manifest", sizeof(out->version));
        safe_strncpy(out->url, o->url, sizeof(out->url));
        safe_strncpy(out->sha256, o->sha256, sizeof(out->sha256));
        safe_strncpy(out->format, o->format, sizeof(out->format));
        out->size = o->size;
        out->strip_components = o->strip_components;
        if (url_basename(out->url, out->filename, sizeof(out->filename)) != 0) {
            set_error("manifest", "cannot derive file name from url '%s'", out->url);
            return ROOTFS_ERR_ARG;
        }
        return ROOTFS_OK;
    }

    /* ② 内置清单 */
    for (int i = 0; g_builtin[i].distro; i++) {
        const rootfs_builtin_t *b = &g_builtin[i];
        if (strcmp(b->distro, distro) != 0 || strcmp(b->arch, arch) != 0) continue;
        safe_strncpy(out->distro, b->distro, sizeof(out->distro));
        safe_strncpy(out->arch, b->arch, sizeof(out->arch));
        safe_strncpy(out->version, b->version, sizeof(out->version));
        safe_strncpy(out->url, b->url, sizeof(out->url));
        safe_strncpy(out->sha256, b->sha256, sizeof(out->sha256));
        safe_strncpy(out->format, b->format, sizeof(out->format));
        out->size = b->size;
        out->strip_components = b->strip_components;
        if (url_basename(out->url, out->filename, sizeof(out->filename)) != 0) {
            set_error("builtin", "cannot derive file name from url '%s'", out->url);
            return ROOTFS_ERR_ARG;
        }
        return ROOTFS_OK;
    }

    set_error("resolve", "unknown distro '%s' for arch '%s' "
              "(builtin: alpine/ubuntu; add more via rootfs_manifest.json)",
              distro, arch);
    return ROOTFS_ERR_UNKNOWN;
}

/* ============================================================
 * 出口白名单（安全策略；不静默绕过）
 * ============================================================ */

int install_rootfs_ensure_egress(const char *distro) {
    rootfs_resolved_t img;
    int rc = resolve_image(distro, &img);
    if (rc != ROOTFS_OK) return -1;

    char host[300];
    if (url_host(img.url, host, sizeof(host)) != 0) {
        set_error("egress", "cannot parse host from url '%s'", img.url);
        return -1;
    }
    if (egress_check(host) != 0) {
        set_error("egress", "host '%s' blocked by egress allowlist — add it to "
                  "<root>/system/config/egress_allowlist.json domains[] "
                  "(or call install_rootfs_allow_mirror(\"%s\") after user consent)",
                  host, distro);
        return -1;
    }
    return 0;
}

int install_rootfs_allow_mirror(const char *distro) {
    rootfs_resolved_t img;
    int rc = resolve_image(distro, &img);
    if (rc != ROOTFS_OK) return rc;

    char host[300];
    if (url_host(img.url, host, sizeof(host)) != 0) {
        set_error("allow", "cannot parse host from url '%s'", img.url);
        return ROOTFS_ERR_ARG;
    }

    char path[ROOTFS_PATH_SZ];
    rootfs_egress_path(path, sizeof(path));

    cJSON *root = NULL;
    FILE *fp = fopen(path, "rb");
    if (fp) {
        char text[64 * 1024];
        size_t got = fread(text, 1, sizeof(text) - 1, fp);
        fclose(fp);
        text[got] = '\0';
        if (got > 0) {
            root = cJSON_Parse(text);
            if (!root) {
                set_error("allow", "%s exists but is not valid JSON — refusing to overwrite "
                          "(fix or remove it manually)", path);
                return ROOTFS_ERR_REGISTRY;
            }
        }
    }
    if (!root) {
        root = cJSON_CreateObject();
        if (!root) return ROOTFS_ERR_REGISTRY;
        cJSON_AddBoolToObject(root, "enabled", 1);
        cJSON_AddItemToObject(root, "domains", cJSON_CreateArray());
    }

    cJSON *domains = cJSON_GetObjectItemCaseSensitive(root, "domains");
    if (!cJSON_IsArray(domains)) {
        cJSON_DeleteItemFromObjectCaseSensitive(root, "domains");
        domains = cJSON_CreateArray();
        cJSON_AddItemToObject(root, "domains", domains);
    }

    int present = 0;
    const cJSON *d = NULL;
    cJSON_ArrayForEach(d, domains) {
        if (cJSON_IsString(d) && strcmp(d->valuestring, host) == 0) { present = 1; break; }
    }
    if (!present) {
        cJSON_AddItemToArray(domains, cJSON_CreateString(host));
    }

    char *text = cJSON_Print(root);
    cJSON_Delete(root);
    if (!text) {
        set_error("allow", "cJSON_Print failed");
        return ROOTFS_ERR_REGISTRY;
    }

    char tmp[ROOTFS_PATH_LONG];
    safe_snprintf(tmp, sizeof(tmp), "%s.tmp.%d", path, (int)getpid());
    FILE *out = fopen(tmp, "wb");
    if (!out) {
        free(text);
        set_error("allow", "cannot write %s: %s", tmp, strerror(errno));
        return ROOTFS_ERR_REGISTRY;
    }
    size_t len = strlen(text);
    size_t wr = fwrite(text, 1, len, out);
    fclose(out);
    free(text);
    if (wr != len) {
        unlink(tmp);
        set_error("allow", "short write to %s", tmp);
        return ROOTFS_ERR_REGISTRY;
    }
    if (rename(tmp, path) != 0) {
        unlink(tmp);
        set_error("allow", "atomic rename failed: %s", strerror(errno));
        return ROOTFS_ERR_ATOMIC;
    }

    egress_reload();   /* 让 egress.c 重新加载（否则内存缓存仍旧） */
    LOG_INFO_T("Rootfs", "AllowMirror", "OK", "host '%s' added to %s (%s)",
               host, path, present ? "already present" : "newly added");
    return ROOTFS_OK;
}

/* ============================================================
 * 下载 + 校验 + 原子替换归档
 * ============================================================ */

/* 原子替换（目标存在则先备份；失败自动回滚） */
static int atomic_replace(const char *src, const char *dst) {
    if (access(dst, F_OK) != 0) {
        if (rename(src, dst) != 0) {
            set_error("atomic", "rename %s -> %s failed: %s", src, dst, strerror(errno));
            return -1;
        }
        return 0;
    }

    char bak[ROOTFS_PATH_LONG];
    safe_snprintf(bak, sizeof(bak), "%s.bak.%d", dst, (int)getpid());
    unlink(bak);
    if (rename(dst, bak) != 0) {
        set_error("atomic", "cannot back up existing %s: %s", dst, strerror(errno));
        return -1;
    }
    if (rename(src, dst) != 0) {
        set_error("atomic", "rename %s -> %s failed: %s", src, dst, strerror(errno));
        if (rename(bak, dst) == 0) {
            LOG_WARN_T("Rootfs", "Atomic", "Rollback", "previous archive restored at %s", dst);
        } else {
            LOG_ERROR_T("Rootfs", "Atomic", "RollbackFail",
                        "previous archive left at %s (rename back failed: %s)", bak, strerror(errno));
        }
        return -1;
    }
    unlink(bak);
    return 0;
}

/**
 * 校验已有归档（大小 + SHA-256）。
 * @return ROOTFS_OK 通过；ROOTFS_ERR_SIZE / ROOTFS_ERR_SHA256 / ROOTFS_ERR_ARG 失败
 */
static int verify_archive(const char *path, const rootfs_resolved_t *img) {
    long long sz = 0;
    if (file_size(path, &sz) != 0) {
        set_error("verify", "cannot stat %s", path);
        return ROOTFS_ERR_ARG;
    }
    if (img->size > 0 && sz != img->size) {
        set_error("verify", "size mismatch: %s is %lld bytes, manifest expects %lld",
                  path, sz, img->size);
        return ROOTFS_ERR_SIZE;
    }
    if (img->sha256[0]) {
        char got[65];
        if (file_sha256(path, got, sizeof(got)) != 0) {
            set_error("verify", "sha256sum failed for %s (is coreutils installed?)", path);
            return ROOTFS_ERR_SHA256;
        }
        if (strcasecmp(got, img->sha256) != 0) {
            set_error("verify", "SHA-256 mismatch: got %s, manifest expects %s", got, img->sha256);
            return ROOTFS_ERR_SHA256;
        }
        safe_strncpy(g_last_sha256, got, sizeof(g_last_sha256));
    } else {
        /* 诚实：无 digest 时明确"未校验"，绝不声称已校验 */
        LOG_WARN_T("Rootfs", "Verify", "NoDigest",
                   "%s: manifest has no sha256 — content NOT verified (size only)", path);
    }
    return ROOTFS_OK;
}

int install_rootfs_download(const char *distro) {
    rootfs_resolved_t img;
    int rc = resolve_image(distro, &img);
    if (rc != ROOTFS_OK) return rc;

    g_last_error[0] = '\0';
    g_last_sha256[0] = '\0';

    char cache_dir[ROOTFS_PATH_SZ];
    rootfs_cache_dir(cache_dir, sizeof(cache_dir));
    char archive[ROOTFS_PATH_LONG];
    safe_snprintf(archive, sizeof(archive), "%s/%s", cache_dir, img.filename);

    /* 已有归档：先校验，通过即复用（幂等，不重复下载） */
    if (access(archive, F_OK) == 0) {
        int vr = verify_archive(archive, &img);
        if (vr == ROOTFS_OK) {
            LOG_INFO_T("Rootfs", "Download", "Reuse",
                       "%s already present and verified — reuse (no re-download)", archive);
            return ROOTFS_OK;
        }
        LOG_WARN_T("Rootfs", "Download", "ReDownload",
                   "existing archive failed verification (%s) — re-downloading", g_last_error);
    }

    if (install_rootfs_ensure_egress(distro) != 0) {
        return ROOTFS_ERR_EGRESS;
    }

    if (mkdir_p(cache_dir) != 0) {
        set_error("download", "cannot create cache dir %s: %s", cache_dir, strerror(errno));
        return ROOTFS_ERR_TMP;
    }

    char tmp[ROOTFS_PATH_LONG + 32];
    safe_snprintf(tmp, sizeof(tmp), "%s.part.%d", archive, (int)getpid());
    unlink(tmp);

    LOG_INFO_T("Rootfs", "Download", "Start",
               "distro=%s version=%s arch=%s size=%s url=%s",
               img.distro, img.version, img.arch,
               img.size > 0 ? "(manifest)" : "unknown", img.url);

    int dl = http_download(img.url, tmp, ROOTFS_DL_TIMEOUT_S);
    if (dl != 0) {
        unlink(tmp);
        set_error("download", "http_download rc=%d for %s (libcurl=%s; https needs libcurl)",
                  dl, img.url, http_curl_available() ? "available" : "unavailable");
        return ROOTFS_ERR_DOWNLOAD;
    }

    /* 大小（清单提供时）→ SHA-256 → 原子替换 */
    rc = verify_archive(tmp, &img);
    if (rc != ROOTFS_OK) {
        unlink(tmp);   /* 半成品删除；旧归档保持不变（回滚 = 无变更） */
        LOG_ERROR_T("Rootfs", "Download", "Reject", "%s — partial file removed", g_last_error);
        return rc;
    }

    if (atomic_replace(tmp, archive) != 0) {
        unlink(tmp);
        return ROOTFS_ERR_ATOMIC;
    }

    long long sz = 0;
    file_size(archive, &sz);
    LOG_INFO_T("Rootfs", "Download", "OK",
               "%s (%lld bytes, sha256=%s) → %s",
               img.filename, sz, g_last_sha256[0] ? g_last_sha256 : "(unverified)",
               archive);
    return ROOTFS_OK;
}

/* ============================================================
 * 解压 + 原子目录切换
 * ============================================================ */

static int staging_has_rootfs(const char *staging) {
    static const char *probe[] = { "etc", "usr", "bin", "lib", NULL };
    for (int i = 0; probe[i]; i++) {
        char p[ROOTFS_PATH_LONG];
        safe_snprintf(p, sizeof(p), "%s/%s", staging, probe[i]);
        struct stat st;
        if (stat(p, &st) == 0 && S_ISDIR(st.st_mode)) return 1;
    }
    return 0;
}

/* tar 参数化执行（不经 shell；白名单命令 tar） */
static int extract_archive(const rootfs_resolved_t *img, const char *archive, const char *staging) {
    char out[2048];
    char strip[48];
    char *argv[12];
    int n = 0;

    argv[n++] = (char *)"tar";
    argv[n++] = (char *)"-xf";
    argv[n++] = (char *)archive;
    argv[n++] = (char *)"-C";
    argv[n++] = (char *)staging;
    argv[n++] = (char *)"--no-same-owner";
    if (img->strip_components > 0) {
        safe_snprintf(strip, sizeof(strip), "--strip-components=%d", img->strip_components);
        argv[n++] = strip;
    }
    argv[n] = NULL;

    memset(out, 0, sizeof(out));
    int rc = safe_exec_run(argv, out, sizeof(out), ROOTFS_DL_TIMEOUT_S);
    if (rc != 0) {
        set_error("extract", "tar rc=%d: %.300s", rc, out[0] ? out : "(no output)");
        return -1;
    }
    if (!staging_has_rootfs(staging)) {
        set_error("extract", "extracted tree at %s has no etc/usr/bin/lib — refusing to install",
                  staging);
        return -1;
    }
    return 0;
}

/* 目录原子切换（旧目录备份；失败回滚） */
static int swap_dir(const char *staging, const char *final_dir) {
    char bak[ROOTFS_PATH_LONG];
    safe_snprintf(bak, sizeof(bak), "%s.bak.%d", final_dir, (int)getpid());
    rm_rf(bak);

    int had_old = (access(final_dir, F_OK) == 0);
    if (had_old && rename(final_dir, bak) != 0) {
        set_error("swap", "cannot move old rootfs %s aside: %s", final_dir, strerror(errno));
        return -1;
    }
    if (rename(staging, final_dir) != 0) {
        set_error("swap", "cannot move new rootfs into place: %s", strerror(errno));
        if (had_old && rename(bak, final_dir) == 0) {
            LOG_WARN_T("Rootfs", "Swap", "Rollback", "previous rootfs restored at %s", final_dir);
        } else if (had_old) {
            LOG_ERROR_T("Rootfs", "Swap", "RollbackFail",
                        "previous rootfs left at %s — manual recovery needed", bak);
        }
        return -1;
    }
    if (had_old) rm_rf(bak);
    return 0;
}

/* ============================================================
 * 登记表（rootfs_installed.json，原子写）
 * ============================================================ */

static cJSON *registry_load(int create_if_missing) {
    char path[ROOTFS_PATH_SZ];
    rootfs_registry_path(path, sizeof(path));

    FILE *fp = fopen(path, "rb");
    if (!fp) {
        if (!create_if_missing) return NULL;
        cJSON *root = cJSON_CreateObject();
        if (!root) return NULL;
        cJSON_AddNumberToObject(root, "version", 1);
        cJSON_AddItemToObject(root, "sandboxes", cJSON_CreateArray());
        return root;
    }

    char text[256 * 1024];
    size_t got = fread(text, 1, sizeof(text) - 1, fp);
    fclose(fp);
    text[got] = '\0';
    if (got == 0) {
        if (!create_if_missing) return NULL;
        cJSON *root = cJSON_CreateObject();
        if (!root) return NULL;
        cJSON_AddNumberToObject(root, "version", 1);
        cJSON_AddItemToObject(root, "sandboxes", cJSON_CreateArray());
        return root;
    }
    cJSON *root = cJSON_Parse(text);
    if (!root) {
        set_error("registry", "%s is not valid JSON — refusing to overwrite", path);
        return NULL;
    }
    return root;
}

static int registry_save(cJSON *root) {
    if (!root) return -1;
    char path[ROOTFS_PATH_SZ];
    rootfs_registry_path(path, sizeof(path));

    char *text = cJSON_Print(root);
    if (!text) {
        set_error("registry", "cJSON_Print failed");
        return -1;
    }
    char tmp[ROOTFS_PATH_LONG];
    safe_snprintf(tmp, sizeof(tmp), "%s.tmp.%d", path, (int)getpid());
    FILE *fp = fopen(tmp, "wb");
    if (!fp) {
        free(text);
        set_error("registry", "cannot write %s: %s", tmp, strerror(errno));
        return -1;
    }
    size_t len = strlen(text);
    size_t wr = fwrite(text, 1, len, fp);
    fclose(fp);
    free(text);
    if (wr != len) {
        unlink(tmp);
        set_error("registry", "short write to %s", tmp);
        return -1;
    }
    if (rename(tmp, path) != 0) {
        unlink(tmp);
        set_error("registry", "atomic rename failed: %s", strerror(errno));
        return -1;
    }
    return 0;
}

static cJSON *registry_find(cJSON *root, const char *name) {
    cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, "sandboxes");
    if (!cJSON_IsArray(arr)) return NULL;
    cJSON *it = NULL;
    cJSON_ArrayForEach(it, arr) {
        cJSON *jn = cJSON_GetObjectItemCaseSensitive(it, "name");
        if (cJSON_IsString(jn) && strcmp(jn->valuestring, name) == 0) return it;
    }
    return NULL;
}

static int registry_upsert(const char *name, const char *version, const char *arch,
                           const char *rootfs_dir, const char *archive, const char *sha256,
                           long long size, const char *proot_path, const char *source) {
    cJSON *root = registry_load(1);
    if (!root) {
        if (!g_last_error[0]) set_error("registry", "cannot load registry");
        return -1;
    }

    cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, "sandboxes");
    if (!cJSON_IsArray(arr)) {
        cJSON_DeleteItemFromObjectCaseSensitive(root, "sandboxes");
        arr = cJSON_CreateArray();
        cJSON_AddItemToObject(root, "sandboxes", arr);
    }

    cJSON *entry = registry_find(root, name);
    if (!entry) {
        entry = cJSON_CreateObject();
        if (!entry) { cJSON_Delete(root); return -1; }
        cJSON_AddItemToArray(arr, entry);
    }

    cJSON_DeleteItemFromObjectCaseSensitive(entry, "name");
    cJSON_DeleteItemFromObjectCaseSensitive(entry, "version");
    cJSON_DeleteItemFromObjectCaseSensitive(entry, "arch");
    cJSON_DeleteItemFromObjectCaseSensitive(entry, "rootfs_dir");
    cJSON_DeleteItemFromObjectCaseSensitive(entry, "archive");
    cJSON_DeleteItemFromObjectCaseSensitive(entry, "sha256");
    cJSON_DeleteItemFromObjectCaseSensitive(entry, "size");
    cJSON_DeleteItemFromObjectCaseSensitive(entry, "proot_path");
    cJSON_DeleteItemFromObjectCaseSensitive(entry, "installed_at");
    cJSON_DeleteItemFromObjectCaseSensitive(entry, "source");

    cJSON_AddStringToObject(entry, "name", name ? name : "");
    cJSON_AddStringToObject(entry, "version", version ? version : "");
    cJSON_AddStringToObject(entry, "arch", arch ? arch : "");
    cJSON_AddStringToObject(entry, "rootfs_dir", rootfs_dir ? rootfs_dir : "");
    cJSON_AddStringToObject(entry, "archive", archive ? archive : "");
    cJSON_AddStringToObject(entry, "sha256", sha256 ? sha256 : "");
    cJSON_AddNumberToObject(entry, "size", (double)size);
    cJSON_AddStringToObject(entry, "proot_path", proot_path ? proot_path : "");
    cJSON_AddNumberToObject(entry, "installed_at", (double)time(NULL));
    cJSON_AddStringToObject(entry, "source", source ? source : "");

    int rc = registry_save(root);
    cJSON_Delete(root);
    if (rc != 0) return -1;
    LOG_INFO_T("Rootfs", "Registry", "Upsert", "%s registered (%s)", name, rootfs_dir);
    return 0;
}

static int registry_remove(const char *name) {
    cJSON *root = registry_load(0);
    if (!root) {
        set_error("registry", "no registry (%s)", install_rootfs_get_last_error());
        return -1;
    }
    cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, "sandboxes");
    if (!cJSON_IsArray(arr)) { cJSON_Delete(root); return -1; }

    int idx = 0, found = -1;
    cJSON *it = NULL;
    cJSON_ArrayForEach(it, arr) {
        cJSON *jn = cJSON_GetObjectItemCaseSensitive(it, "name");
        if (cJSON_IsString(jn) && strcmp(jn->valuestring, name) == 0) { found = idx; break; }
        idx++;
    }
    if (found < 0) { cJSON_Delete(root); return -1; }

    cJSON_DeleteItemFromArray(arr, found);
    int rc = registry_save(root);
    cJSON_Delete(root);
    return rc;
}

/* ============================================================
 * proot 探测（找到即登记；未找到如实记录"待配置"）
 * ============================================================ */

static int detect_proot(char *out, size_t out_sz) {
    const char *cands[] = {
        "/usr/bin/proot",
        "/usr/local/bin/proot",
        "/data/data/com.termux/files/usr/bin/proot",
        NULL
    };
    for (int i = 0; cands[i]; i++) {
        if (access(cands[i], X_OK) == 0) {
            safe_strncpy(out, cands[i], out_sz);
            return 1;
        }
    }
    char p[ROOTFS_PATH_SZ];
    safe_snprintf(p, sizeof(p), "%s/bin/proot", lingos_data_root());
    if (access(p, X_OK) == 0) {
        safe_strncpy(out, p, out_sz);
        return 1;
    }
    out[0] = '\0';
    return 0;
}

/* ============================================================
 * 对外：安装 / 状态 / 卸载 / 登记
 * ============================================================ */

int install_rootfs_install(const char *distro) {
    rootfs_resolved_t img;
    int rc = resolve_image(distro, &img);
    if (rc != ROOTFS_OK) return rc;

    /* ① 下载 + 校验 + 原子替换归档 */
    rc = install_rootfs_download(distro);
    if (rc != ROOTFS_OK) return rc;

    char cache_dir[ROOTFS_PATH_SZ];
    rootfs_cache_dir(cache_dir, sizeof(cache_dir));
    char archive[ROOTFS_PATH_LONG];
    safe_snprintf(archive, sizeof(archive), "%s/%s", cache_dir, img.filename);

    char parent[ROOTFS_PATH_SZ];
    rootfs_parent_dir(parent, sizeof(parent));
    if (mkdir_p(parent) != 0) {
        set_error("install", "cannot create %s: %s", parent, strerror(errno));
        return ROOTFS_ERR_TMP;
    }

    char final_dir[ROOTFS_PATH_LONG];
    safe_snprintf(final_dir, sizeof(final_dir), "%s/%s", parent, img.distro);
    char staging[ROOTFS_PATH_LONG];
    safe_snprintf(staging, sizeof(staging), "%s/.staging-%s-%d", parent, img.distro, (int)getpid());

    /* ② 解压到 staging（失败不会触碰已装目录） */
    rm_rf(staging);
    if (mkdir_p(staging) != 0) {
        set_error("install", "cannot create staging dir %s: %s", staging, strerror(errno));
        return ROOTFS_ERR_TMP;
    }
    LOG_INFO_T("Rootfs", "Install", "Extract", "%s → %s (strip=%d)",
               archive, staging, img.strip_components);
    if (extract_archive(&img, archive, staging) != 0) {
        rm_rf(staging);
        LOG_ERROR_T("Rootfs", "Install", "ExtractFail", "%s", g_last_error);
        return ROOTFS_ERR_EXTRACT;
    }

    /* ③ 原子切换（旧目录备份/失败回滚） */
    if (swap_dir(staging, final_dir) != 0) {
        rm_rf(staging);
        LOG_ERROR_T("Rootfs", "Install", "SwapFail", "%s", g_last_error);
        return ROOTFS_ERR_ATOMIC;
    }

    /* ④ proot 探测 + 登记 */
    char proot[ROOTFS_PATH_SZ];
    if (!detect_proot(proot, sizeof(proot))) {
        LOG_WARN_T("Rootfs", "Install", "NoProot",
                   "proot binary not found (checked /usr/bin, /usr/local/bin, Termux, <root>/bin) — "
                   "rootfs registered honestly with empty proot_path (configure it in App/settings)");
    }

    if (registry_upsert(img.distro, img.version, img.arch, final_dir, archive,
                        g_last_sha256[0] ? g_last_sha256 : img.sha256,
                        img.size, proot, "download") != 0) {
        LOG_ERROR_T("Rootfs", "Install", "RegistryFail", "%s", g_last_error);
        return ROOTFS_ERR_REGISTRY;
    }

    LOG_INFO_T("Rootfs", "Install", "OK",
               "%s %s installed at %s (sha256=%s, proot=%s)",
               img.distro, img.version, final_dir,
               g_last_sha256[0] ? g_last_sha256 : "(unverified)",
               proot[0] ? proot : "(not found)");
    return ROOTFS_OK;
}

int install_rootfs_is_ready(const char *distro) {
    if (!distro || !safe_token(distro, 31)) return 0;

    cJSON *root = registry_load(0);
    if (!root) return 0;
    cJSON *entry = registry_find(root, distro);
    int ready = 0;
    if (entry) {
        cJSON *jd = cJSON_GetObjectItemCaseSensitive(entry, "rootfs_dir");
        if (cJSON_IsString(jd) && jd->valuestring[0]) {
            struct stat st;
            if (stat(jd->valuestring, &st) == 0 && S_ISDIR(st.st_mode)) ready = 1;
        }
    }
    cJSON_Delete(root);
    return ready;
}

int install_rootfs_uninstall(const char *distro) {
    if (!distro || !safe_token(distro, 31)) {
        set_error("uninstall", "invalid distro name");
        return ROOTFS_ERR_ARG;
    }

    cJSON *root = registry_load(0);
    if (!root) {
        set_error("uninstall", "no registry entry for '%s' (%s)", distro,
                  g_last_error[0] ? g_last_error : "registry missing");
        return ROOTFS_ERR_UNKNOWN;
    }
    cJSON *entry = registry_find(root, distro);
    if (!entry) {
        cJSON_Delete(root);
        set_error("uninstall", "'%s' is not registered", distro);
        return ROOTFS_ERR_UNKNOWN;
    }
    cJSON *jd = cJSON_GetObjectItemCaseSensitive(entry, "rootfs_dir");
    char dir[ROOTFS_PATH_LONG];
    dir[0] = '\0';
    if (cJSON_IsString(jd)) safe_strncpy(dir, jd->valuestring, sizeof(dir));
    cJSON_Delete(root);

    if (dir[0]) {
        /* 安全守卫：只删除 <data_root>/data/rootfs/ 下的目录 */
        char parent[ROOTFS_PATH_SZ];
        rootfs_parent_dir(parent, sizeof(parent));
        if (strncmp(dir, parent, strlen(parent)) != 0 || strstr(dir, "..")) {
            set_error("uninstall", "refusing to delete '%s' (outside %s)", dir, parent);
            return ROOTFS_ERR_ARG;
        }
        rm_rf(dir);
        LOG_INFO_T("Rootfs", "Uninstall", "Removed", "rootfs dir %s removed (archive cache kept)", dir);
    }
    if (registry_remove(distro) != 0) {
        set_error("uninstall", "rootfs removed but registry update failed");
        return ROOTFS_ERR_REGISTRY;
    }
    return ROOTFS_OK;
}

int install_rootfs_register_manual(const char *name, const char *rootfs_dir, const char *proot_path) {
    if (!name || !rootfs_dir || !safe_token(name, 31)) {
        set_error("register", "invalid name/params");
        return ROOTFS_ERR_ARG;
    }
    struct stat st;
    if (stat(rootfs_dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
        set_error("register", "rootfs dir '%s' does not exist (manual registration needs a real dir)",
                  rootfs_dir);
        return ROOTFS_ERR_ARG;
    }
    if (proot_path && proot_path[0] && access(proot_path, X_OK) != 0) {
        set_error("register", "proot '%s' not executable — pass an empty path to leave unset", proot_path);
        return ROOTFS_ERR_ARG;
    }

    if (registry_upsert(name, "manual", install_rootfs_arch(), rootfs_dir, "", "",
                        0, proot_path ? proot_path : "", "manual") != 0) {
        return ROOTFS_ERR_REGISTRY;
    }
    LOG_INFO_T("Rootfs", "Register", "OK", "manual sandbox '%s' → %s (proot=%s)",
               name, rootfs_dir, (proot_path && proot_path[0]) ? proot_path : "(unset)");
    return ROOTFS_OK;
}

int install_rootfs_list(char *out, size_t out_sz) {
    if (!out || out_sz == 0) return -1;
    out[0] = '\0';

    cJSON *root = registry_load(0);
    if (!root) {
        safe_strncpy(out, "[]", out_sz);
        return 2;
    }
    cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, "sandboxes");
    char *text = cJSON_IsArray(arr) ? cJSON_PrintUnformatted(arr) : NULL;
    cJSON_Delete(root);
    if (!text) {
        safe_strncpy(out, "[]", out_sz);
        return 2;
    }
    size_t len = strlen(text);
    if (len + 1 > out_sz) { free(text); return -1; }
    memcpy(out, text, len + 1);
    free(text);
    return (int)len;
}

int install_rootfs_available(char *out, size_t out_sz) {
    if (!out || out_sz == 0) return -1;
    out[0] = '\0';

    const char *arch = install_rootfs_arch();
    load_overrides();

    cJSON *arr = cJSON_CreateArray();
    if (!arr) return -1;

    for (int i = 0; g_builtin[i].distro; i++) {
        if (strcmp(g_builtin[i].arch, arch) != 0) continue;
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "distro", g_builtin[i].distro);
        cJSON_AddStringToObject(o, "version", g_builtin[i].version);
        cJSON_AddStringToObject(o, "arch", g_builtin[i].arch);
        cJSON_AddStringToObject(o, "url", g_builtin[i].url);
        cJSON_AddStringToObject(o, "format", g_builtin[i].format);
        cJSON_AddBoolToObject(o, "sha256_pinned", g_builtin[i].sha256[0] ? 1 : 0);
        cJSON_AddNumberToObject(o, "size", (double)g_builtin[i].size);
        cJSON_AddStringToObject(o, "source", "builtin");
        cJSON_AddItemToArray(arr, o);
    }
    for (int i = 0; i < g_override_count; i++) {
        if (strcmp(g_overrides[i].arch, "any") != 0 && strcmp(g_overrides[i].arch, arch) != 0) continue;
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "distro", g_overrides[i].distro);
        cJSON_AddStringToObject(o, "version", g_overrides[i].version);
        cJSON_AddStringToObject(o, "arch", g_overrides[i].arch);
        cJSON_AddStringToObject(o, "url", g_overrides[i].url);
        cJSON_AddStringToObject(o, "format", g_overrides[i].format);
        cJSON_AddBoolToObject(o, "sha256_pinned", g_overrides[i].sha256[0] ? 1 : 0);
        cJSON_AddNumberToObject(o, "size", (double)g_overrides[i].size);
        cJSON_AddStringToObject(o, "source", "user-manifest");
        cJSON_AddItemToArray(arr, o);
    }

    char *text = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    if (!text) return -1;
    size_t len = strlen(text);
    if (len + 1 > out_sz) { free(text); return -1; }
    memcpy(out, text, len + 1);
    free(text);
    return (int)len;
}

void install_rootfs_cleanup(void) {
    g_last_error[0] = '\0';
}
