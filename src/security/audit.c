/**
 * @file    audit.c
 * @brief   审计日志（环形缓冲 + BLAKE2b 哈希链，信封加密存储）
 * @version LN-B-4.0.0.0
 * @changes 修复 audit_repair 重算逻辑；增加详细日志；头文件引用规范化
 */

#include "audit.h"
#include "../common/string_no_sys.h"
#include "../common/data_path.h"
#include "../common/safe_string.h"
#include "../common/lang.h"
#include "../drivers/timer.h"
#include "../drivers/uart.h"
#include "../lib/log_extra.h"
#include "../lib/crypto/monocypher.h"
#include "../lib/cJSON/cJSON.h"
#include "../security/crypto/crypto_core.h"
#include "../security/crypto/envelope.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>
#include <time.h>

#define AUDIT_BUF_SIZE 128

/* ============================================================
 * 全局状态
 * ============================================================ */
static audit_entry_t audit_ring[AUDIT_BUF_SIZE];
static int audit_head = 0;
static int audit_count_val = 0;
static uint8_t last_hash[AUDIT_HASH_SIZE] = {0};

/* ============================================================
 * 内部辅助函数
 * ============================================================ */

/**
 * @brief 计算单条审计条目的哈希值（包含 prev_hash）
 */
static void compute_entry_hash(const audit_entry_t *e, uint8_t hash_out[AUDIT_HASH_SIZE]) {
    LOG_DEBUG_T("Audit", "ComputeHash", "Enter", "e=%p, hash_out=%p", (void*)e, (void*)hash_out);
    if (!e || !hash_out) {
        LOG_ERROR_T("Audit", "ComputeHash", "Invalid", "e=%p, hash_out=%p", (void*)e, (void*)hash_out);
        return;
    }

    crypto_blake2b_ctx ctx;
    crypto_blake2b_init(&ctx, AUDIT_HASH_SIZE);

    crypto_blake2b_update(&ctx, (uint8_t*)&e->timestamp, sizeof(e->timestamp));
    crypto_blake2b_update(&ctx, (uint8_t*)e->uid, strlen(e->uid));
    crypto_blake2b_update(&ctx, (uint8_t*)e->source, strlen(e->source));
    crypto_blake2b_update(&ctx, (uint8_t*)e->skill_name, strlen(e->skill_name));
    crypto_blake2b_update(&ctx, (uint8_t*)e->args, strlen(e->args));
    crypto_blake2b_update(&ctx, (uint8_t*)e->result, strlen(e->result));
    crypto_blake2b_update(&ctx, (uint8_t*)&e->ret_code, sizeof(e->ret_code));
    crypto_blake2b_update(&ctx, (uint8_t*)e->risk_level, strlen(e->risk_level));
    crypto_blake2b_update(&ctx, (uint8_t*)&e->confirmed, sizeof(e->confirmed));
    crypto_blake2b_update(&ctx, (uint8_t*)e->prev_hash, AUDIT_HASH_SIZE);

    crypto_blake2b_final(&ctx, hash_out);
    LOG_DEBUG_T("Audit", "ComputeHash", "Exit", "hash computed");
}

/* ============================================================
 * 公共 API 实现
 * ============================================================ */

void audit_init(void) {
    LOG_INFO_T("Audit", "Init", "Enter", "Initializing audit system (buffer_size=%d)", AUDIT_BUF_SIZE);
    memset(audit_ring, 0, sizeof(audit_ring));
    audit_head = 0;
    audit_count_val = 0;
    memset(last_hash, 0, AUDIT_HASH_SIZE);
    LOG_INFO_T("Audit", "Init", "OK", "Audit system initialized with hash chain (buffer=%d entries)", AUDIT_BUF_SIZE);
}

void audit_log(const char *uid, const char *source, const char *skill_name,
               const char *args, const char *result, int ret_code,
               const char *risk_level, uint8_t confirmed) {
    LOG_DEBUG_T("Audit", "Log", "Enter", "uid='%s', source='%s', skill='%s', ret_code=%d, risk='%s', confirmed=%d",
                uid ? uid : "(null)", source ? source : "(null)",
                skill_name ? skill_name : "(null)", ret_code,
                risk_level ? risk_level : "(null)", confirmed);

    audit_entry_t *e = &audit_ring[audit_head];

    e->timestamp = timer_get_ticks();
    LOG_DEBUG_T("Audit", "Log", "Timestamp", "timestamp=%llu", (unsigned long long)e->timestamp);

    /* 安全复制字符串（使用 safe_strncpy） */
    safe_strncpy(e->uid, uid ? uid : "system", sizeof(e->uid));
    safe_strncpy(e->source, source ? source : "unknown", sizeof(e->source));
    safe_strncpy(e->skill_name, skill_name ? skill_name : "?", sizeof(e->skill_name));
    safe_strncpy(e->args, args ? args : "", sizeof(e->args));
    safe_strncpy(e->result, result ? result : "", sizeof(e->result));
    safe_strncpy(e->risk_level, risk_level ? risk_level : "low", sizeof(e->risk_level));

    e->ret_code = ret_code;
    e->confirmed = confirmed;

    /* 复制前一个哈希 */
    memcpy(e->prev_hash, last_hash, AUDIT_HASH_SIZE);
    LOG_DEBUG_T("Audit", "Log", "PrevHash", "prev_hash copied (first 8 bytes: %02x%02x%02x%02x%02x%02x%02x%02x)",
                e->prev_hash[0], e->prev_hash[1], e->prev_hash[2], e->prev_hash[3],
                e->prev_hash[4], e->prev_hash[5], e->prev_hash[6], e->prev_hash[7]);

    /* 计算当前条目哈希 */
    uint8_t current_hash[AUDIT_HASH_SIZE];
    compute_entry_hash(e, current_hash);
    memcpy(last_hash, current_hash, AUDIT_HASH_SIZE);
    LOG_DEBUG_T("Audit", "Log", "CurrentHash", "current_hash updated");

    /* 更新环形缓冲区指针 */
    audit_head = (audit_head + 1) % AUDIT_BUF_SIZE;
    if (audit_count_val < AUDIT_BUF_SIZE) {
        audit_count_val++;
        LOG_DEBUG_T("Audit", "Log", "Count", "audit_count_val=%d", audit_count_val);
    } else {
        LOG_DEBUG_T("Audit", "Log", "Overwrite", "Buffer full, overwriting oldest entry");
    }

    LOG_INFO_T("Audit", "Log", "OK", "audit entry logged: uid='%s', skill='%s', ret=%d",
               e->uid, e->skill_name, e->ret_code);
}

void audit_dump(char *buf, uint32_t buf_len) {
    LOG_DEBUG_T("Audit", "Dump", "Enter", "buf=%p, buf_len=%u", (void*)buf, buf_len);
    if (!buf || buf_len == 0) {
        LOG_ERROR_T("Audit", "Dump", "Invalid", "buf=%p, buf_len=%u", (void*)buf, buf_len);
        return;
    }

    uint32_t total = 0;
    total += safe_snprintf(buf + total, buf_len - total,
                           "========== AUDIT LOG (hash chained, %d entries) ==========\n",
                           audit_count_val);
    LOG_DEBUG_T("Audit", "Dump", "Header", "entries=%d, buf_len=%u", audit_count_val, buf_len);

    int start = (audit_head - audit_count_val + AUDIT_BUF_SIZE) % AUDIT_BUF_SIZE;
    LOG_DEBUG_T("Audit", "Dump", "Start", "start_idx=%d, head=%d", start, audit_head);

    for (int i = 0; i < audit_count_val; i++) {
        int idx = (start + i) % AUDIT_BUF_SIZE;
        audit_entry_t *e = &audit_ring[idx];

        total += safe_snprintf(buf + total, buf_len - total,
                               "[ts=%llu][uid=%s][src=%s][risk=%s][confirm=%d]\n"
                               "  skill: %s\n  args: %s\n  result(%d): %s\n  prev_hash: ",
                               (unsigned long long)e->timestamp,
                               e->uid, e->source, e->risk_level, e->confirmed,
                               e->skill_name, e->args, e->ret_code, e->result);

        for (int j = 0; j < 16 && j < AUDIT_HASH_SIZE; j++) {
            total += safe_snprintf(buf + total, buf_len - total, "%02x", e->prev_hash[j]);
        }
        total += safe_snprintf(buf + total, buf_len - total, "\n\n");

        LOG_DEBUG_T("Audit", "Dump", "Entry", "dumped entry %d (idx=%d)", i, idx);
    }

    total += safe_snprintf(buf + total, buf_len - total,
                           "========== END OF AUDIT LOG ==========\n");

    LOG_INFO_T("Audit", "Dump", "OK", "Dumped %d entries, total_len=%u", audit_count_val, total);
}

int audit_count(void) {
    LOG_DEBUG_T("Audit", "Count", "value", "audit_count_val=%d", audit_count_val);
    return audit_count_val;
}

int audit_verify(char *error_msg, uint32_t msg_len) {
    LOG_DEBUG_T("Audit", "Verify", "Enter", "error_msg=%p, msg_len=%u", (void*)error_msg, msg_len);
    if (!error_msg || msg_len == 0) {
        LOG_ERROR_T("Audit", "Verify", "Invalid", "error_msg=%p, msg_len=%u", (void*)error_msg, msg_len);
        return -1;
    }

    if (audit_count_val == 0) {
        safe_strncpy(error_msg, "No audit entries", msg_len);
        LOG_INFO_T("Audit", "Verify", "Empty", "No entries to verify");
        return 0;
    }

    uint8_t computed_prev[AUDIT_HASH_SIZE] = {0};
    int start = (audit_head - audit_count_val + AUDIT_BUF_SIZE) % AUDIT_BUF_SIZE;
    LOG_DEBUG_T("Audit", "Verify", "Start", "start_idx=%d, count=%d", start, audit_count_val);

    for (int i = 0; i < audit_count_val; i++) {
        int idx = (start + i) % AUDIT_BUF_SIZE;
        audit_entry_t *e = &audit_ring[idx];

        LOG_DEBUG_T("Audit", "Verify", "Entry", "checking entry %d (idx=%d), prev_hash first 4 bytes: %02x%02x%02x%02x",
                    i, idx, e->prev_hash[0], e->prev_hash[1], e->prev_hash[2], e->prev_hash[3]);

        if (memcmp(e->prev_hash, computed_prev, AUDIT_HASH_SIZE) != 0) {
            safe_snprintf(error_msg, msg_len, "Hash chain broken at entry %d (idx=%d)", i, idx);
            LOG_WARN_T("Audit", "Verify", "Broken", "chain broken at entry %d (idx=%d)", i, idx);
            return -1;
        }

        uint8_t current_hash[AUDIT_HASH_SIZE];
        compute_entry_hash(e, current_hash);
        memcpy(computed_prev, current_hash, AUDIT_HASH_SIZE);
        LOG_DEBUG_T("Audit", "Verify", "EntryOK", "entry %d hash verified", i);
    }

    if (memcmp(computed_prev, last_hash, AUDIT_HASH_SIZE) != 0) {
        safe_strncpy(error_msg, "Final hash mismatch", msg_len);
        LOG_WARN_T("Audit", "Verify", "FinalMismatch", "final hash mismatch");
        return -1;
    }

    safe_strncpy(error_msg, "Audit chain verified OK", msg_len);
    LOG_INFO_T("Audit", "Verify", "OK", "audit chain verified successfully");
    return 0;
}

int audit_repair(int strategy, char *error_msg, uint32_t msg_len) {
    LOG_INFO_T("Audit", "Repair", "Enter", "strategy=%d, error_msg=%p, msg_len=%u",
               strategy, (void*)error_msg, msg_len);
    if (!error_msg || msg_len == 0) {
        LOG_ERROR_T("Audit", "Repair", "Invalid", "error_msg=%p, msg_len=%u", (void*)error_msg, msg_len);
        return -1;
    }

    if (audit_count_val == 0) {
        safe_strncpy(error_msg, "No entries to repair", msg_len);
        LOG_INFO_T("Audit", "Repair", "Empty", "No entries to repair");
        return 0;
    }

    /* 查找断裂点 */
    int broken_idx = -1;
    uint8_t computed_prev[AUDIT_HASH_SIZE] = {0};
    int start = (audit_head - audit_count_val + AUDIT_BUF_SIZE) % AUDIT_BUF_SIZE;
    LOG_DEBUG_T("Audit", "Repair", "Start", "start_idx=%d, count=%d", start, audit_count_val);

    for (int i = 0; i < audit_count_val; i++) {
        int idx = (start + i) % AUDIT_BUF_SIZE;
        audit_entry_t *e = &audit_ring[idx];

        if (memcmp(e->prev_hash, computed_prev, AUDIT_HASH_SIZE) != 0) {
            broken_idx = i;
            LOG_WARN_T("Audit", "Repair", "FoundBreak", "chain broken at entry %d (idx=%d)", i, idx);
            break;
        }

        uint8_t current_hash[AUDIT_HASH_SIZE];
        compute_entry_hash(e, current_hash);
        memcpy(computed_prev, current_hash, AUDIT_HASH_SIZE);
        LOG_DEBUG_T("Audit", "Repair", "EntryOK", "entry %d (idx=%d) verified", i, idx);
    }

    /* 如果没有断裂点，但最终哈希不匹配，仍需修复 */
    if (broken_idx == -1 && memcmp(computed_prev, last_hash, AUDIT_HASH_SIZE) != 0) {
        /* 最后一个条目哈希与 last_hash 不匹配，需要修复最后一条 */
        broken_idx = audit_count_val - 1;
        LOG_WARN_T("Audit", "Repair", "FinalMismatch", "final hash mismatch, marking last entry as broken");
    }

    if (broken_idx == -1) {
        safe_strncpy(error_msg, "Chain already valid, no repair needed", msg_len);
        LOG_INFO_T("Audit", "Repair", "Valid", "chain already valid");
        return 0;
    }

    /* === 策略1：截断 === */
    if (strategy == 1) {
        LOG_INFO_T("Audit", "Repair", "Strategy", "Truncating after entry %d", broken_idx);
        audit_count_val = broken_idx;
        if (broken_idx == 0) {
            memset(last_hash, 0, AUDIT_HASH_SIZE);
            LOG_DEBUG_T("Audit", "Repair", "Truncate", "all entries removed, last_hash zeroed");
        } else {
            int last_idx = (start + broken_idx - 1) % AUDIT_BUF_SIZE;
            compute_entry_hash(&audit_ring[last_idx], last_hash);
            LOG_DEBUG_T("Audit", "Repair", "Truncate", "last_hash updated from entry %d (idx=%d)", broken_idx - 1, last_idx);
        }
        safe_snprintf(error_msg, msg_len, "Repaired by truncating after entry %d", broken_idx);
        LOG_INFO_T("Audit", "Repair", "OK", "truncated at entry %d", broken_idx);
        return 0;
    }

    /* === 策略2：重算 === */
    if (strategy == 2) {
        LOG_INFO_T("Audit", "Repair", "Strategy", "Recalculating hashes from entry %d", broken_idx);

        /* 确定断裂点处期望的前一个哈希值 */
        uint8_t prev_hash_snapshot[AUDIT_HASH_SIZE];
        if (broken_idx == 0) {
            /* 从第一个条目断裂，期望 prev_hash 为零 */
            memset(prev_hash_snapshot, 0, AUDIT_HASH_SIZE);
            LOG_DEBUG_T("Audit", "Repair", "Snapshot", "broken at first entry, prev_hash=0");
        } else {
            /* 从 broken_idx 处断裂，期望的 prev_hash 是上一个条目的正确哈希 */
            int prev_idx = (start + broken_idx - 1) % AUDIT_BUF_SIZE;
            compute_entry_hash(&audit_ring[prev_idx], prev_hash_snapshot);
            LOG_DEBUG_T("Audit", "Repair", "Snapshot", "prev_hash snapshot from entry %d (idx=%d)", broken_idx - 1, prev_idx);
        }

        /* 从断裂点开始重算所有后续哈希 */
        for (int i = broken_idx; i < audit_count_val; i++) {
            int cur = (start + i) % AUDIT_BUF_SIZE;
            audit_entry_t *e = &audit_ring[cur];

            LOG_DEBUG_T("Audit", "Repair", "Recalc", "recalculating entry %d (idx=%d)", i, cur);

            /* 将 prev_hash 替换为当前快照 */
            memcpy(e->prev_hash, prev_hash_snapshot, AUDIT_HASH_SIZE);

            /* 计算当前条目的新哈希 */
            uint8_t cur_hash[AUDIT_HASH_SIZE];
            compute_entry_hash(e, cur_hash);

            /* 更新快照为当前哈希（供下一条使用） */
            memcpy(prev_hash_snapshot, cur_hash, AUDIT_HASH_SIZE);
            LOG_DEBUG_T("Audit", "Repair", "RecalcOK", "entry %d (idx=%d) recalculated", i, cur);
        }

        /* 更新 last_hash */
        memcpy(last_hash, prev_hash_snapshot, AUDIT_HASH_SIZE);
        LOG_DEBUG_T("Audit", "Repair", "LastHash", "last_hash updated");

        safe_snprintf(error_msg, msg_len, "Repaired by recalculating hashes from entry %d", broken_idx);
        LOG_INFO_T("Audit", "Repair", "OK", "recalculated from entry %d", broken_idx);
        return 0;
    }

    safe_snprintf(error_msg, msg_len, "Invalid strategy (1=truncate, 2=recalc)");
    LOG_ERROR_T("Audit", "Repair", "InvalidStrategy", "strategy=%d", strategy);
    return -1;
}

/* ====== export JSON (使用 cJSON) ====== */
char* audit_export_json(void) {
    LOG_DEBUG_T("Audit", "ExportJSON", "Enter", "exporting %d entries", audit_count_val);

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        LOG_ERROR_T("Audit", "ExportJSON", "CreateFail", "cJSON_CreateObject failed");
        return NULL;
    }

    cJSON_AddNumberToObject(root, "count", audit_count_val);
    cJSON *entries = cJSON_CreateArray();
    if (!entries) {
        LOG_ERROR_T("Audit", "ExportJSON", "CreateArrayFail", "cJSON_CreateArray failed");
        cJSON_Delete(root);
        return NULL;
    }

    int start = (audit_head - audit_count_val + AUDIT_BUF_SIZE) % AUDIT_BUF_SIZE;
    LOG_DEBUG_T("Audit", "ExportJSON", "Start", "start_idx=%d", start);

    for (int i = 0; i < audit_count_val; i++) {
        int idx = (start + i) % AUDIT_BUF_SIZE;
        audit_entry_t *e = &audit_ring[idx];

        cJSON *obj = cJSON_CreateObject();
        if (!obj) {
            LOG_WARN_T("Audit", "ExportJSON", "ObjCreateFail", "failed to create object for entry %d", i);
            continue;
        }

        cJSON_AddNumberToObject(obj, "timestamp", (double)e->timestamp);
        cJSON_AddStringToObject(obj, "uid", e->uid);
        cJSON_AddStringToObject(obj, "source", e->source);
        cJSON_AddStringToObject(obj, "skill", e->skill_name);
        cJSON_AddStringToObject(obj, "args", e->args);
        cJSON_AddStringToObject(obj, "result", e->result);
        cJSON_AddNumberToObject(obj, "ret_code", e->ret_code);
        cJSON_AddStringToObject(obj, "risk", e->risk_level);
        cJSON_AddBoolToObject(obj, "confirmed", e->confirmed);

        /* prev_hash as hex string */
        char hash_hex[129];
        for (int j = 0; j < AUDIT_HASH_SIZE; j++) {
            safe_snprintf(hash_hex + j*2, 3, "%02x", e->prev_hash[j]);
        }
        hash_hex[128] = '\0';
        cJSON_AddStringToObject(obj, "prev_hash", hash_hex);

        cJSON_AddItemToArray(entries, obj);
        LOG_DEBUG_T("Audit", "ExportJSON", "Entry", "exported entry %d (idx=%d)", i, idx);
    }

    cJSON_AddItemToObject(root, "entries", entries);
    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (json_str) {
        LOG_INFO_T("Audit", "ExportJSON", "OK", "exported %d entries", audit_count_val);
    } else {
        LOG_ERROR_T("Audit", "ExportJSON", "PrintFail", "cJSON_PrintUnformatted failed");
    }

    return json_str;
}

int audit_save_to_file(const char *path) {
    LOG_DEBUG_T("Audit", "Save", "Enter", "path='%s'", path ? path : "(null)");

    const char *root = lingos_data_root();
    char plain_path[1024];
    if (!path) {
        safe_snprintf(plain_path, sizeof(plain_path), "%s/Ensystem/audit_plain.json", root);
        LOG_DEBUG_T("Audit", "Save", "DefaultPath", "using default path: %s", plain_path);
    } else {
        safe_snprintf(plain_path, sizeof(plain_path), "%s", path);
    }

    /* 确保目录存在 */
    char dir[512];
    safe_snprintf(dir, sizeof(dir), "%s/Ensystem", root);
    if (access(dir, F_OK) != 0) {
        LOG_DEBUG_T("Audit", "Save", "Mkdir", "creating directory: %s", dir);
        if (mkdir(dir, 0755) != 0) {
            LOG_ERROR_T("Audit", "Save", "MkdirFail", "cannot create %s: %s (errno=%d)", dir, strerror(errno), errno);
            return -1;
        }
    }

    FILE *fp = fopen(plain_path, "w");
    if (!fp) {
        LOG_ERROR_T("Audit", "Save", "OpenFail", "cannot open %s: %s (errno=%d)", plain_path, strerror(errno), errno);
        return -1;
    }

    uint32_t version = 1;
    fwrite(&version, sizeof(version), 1, fp);
    fwrite(&audit_count_val, sizeof(audit_count_val), 1, fp);
    LOG_DEBUG_T("Audit", "Save", "Header", "version=%u, count=%d", version, audit_count_val);

    int start = (audit_head - audit_count_val + AUDIT_BUF_SIZE) % AUDIT_BUF_SIZE;
    for (int i = 0; i < audit_count_val; i++) {
        int idx = (start + i) % AUDIT_BUF_SIZE;
        fwrite(&audit_ring[idx], sizeof(audit_entry_t), 1, fp);
        LOG_DEBUG_T("Audit", "Save", "Entry", "saved entry %d (idx=%d)", i, idx);
    }
    fclose(fp);

    /* 信封加密 */
    char enc_path[1024];
    safe_snprintf(enc_path, sizeof(enc_path), "%s.enc", plain_path);
    LOG_DEBUG_T("Audit", "Save", "Encrypt", "encrypting to: %s", enc_path);

    if (envelope_encrypt_file(plain_path, enc_path, "LINGOS_AUDIT_KEY") != 0) {
        LOG_ERROR_T("Audit", "Save", "EncryptFail", "envelope_encrypt_file failed");
        unlink(plain_path);
        return -1;
    }

    if (rename(enc_path, plain_path) != 0) {
        LOG_ERROR_T("Audit", "Save", "RenameFail", "rename %s -> %s failed: %s", enc_path, plain_path, strerror(errno));
        unlink(enc_path);
        return -1;
    }

    LOG_INFO_T("Audit", "Save", "OK", "saved encrypted to %s", plain_path);
    return 0;
}

int audit_load_from_file(const char *path) {
    LOG_DEBUG_T("Audit", "Load", "Enter", "path='%s'", path ? path : "(null)");

    const char *root = lingos_data_root();
    char plain_path[1024];
    if (!path) {
        safe_snprintf(plain_path, sizeof(plain_path), "%s/Ensystem/audit_plain.json", root);
        LOG_DEBUG_T("Audit", "Load", "DefaultPath", "using default path: %s", plain_path);
    } else {
        safe_snprintf(plain_path, sizeof(plain_path), "%s", path);
    }

    char dec_path[1024];
    safe_snprintf(dec_path, sizeof(dec_path), "%s.dec", plain_path);
    LOG_DEBUG_T("Audit", "Load", "Decrypt", "decrypting to: %s", dec_path);

    int need_cleanup = 0;
    if (envelope_decrypt_file(plain_path, dec_path, "LINGOS_AUDIT_KEY") != 0) {
        LOG_WARN_T("Audit", "Load", "DecryptFail", "envelope_decrypt_file failed, trying plain file");
        if (access(plain_path, F_OK) == 0) {
            LOG_DEBUG_T("Audit", "Load", "PlainFallback", "using plain file: %s", plain_path);
            safe_strncpy(dec_path, plain_path, sizeof(dec_path));
        } else {
            LOG_ERROR_T("Audit", "Load", "NoFile", "neither encrypted nor plain file found: %s", plain_path);
            return -1;
        }
    } else {
        need_cleanup = 1;
        LOG_DEBUG_T("Audit", "Load", "DecryptOK", "decrypted successfully");
    }

    FILE *fp = fopen(dec_path, "rb");
    if (!fp) {
        LOG_ERROR_T("Audit", "Load", "OpenFail", "cannot open %s: %s (errno=%d)", dec_path, strerror(errno), errno);
        if (need_cleanup) unlink(dec_path);
        return -1;
    }

    uint32_t version, count;
    if (fread(&version, sizeof(version), 1, fp) != 1) {
        LOG_ERROR_T("Audit", "Load", "ReadVersionFail", "failed to read version");
        fclose(fp);
        if (need_cleanup) unlink(dec_path);
        return -1;
    }
    LOG_DEBUG_T("Audit", "Load", "Version", "version=%u", version);

    if (version != 1) {
        LOG_ERROR_T("Audit", "Load", "VersionMismatch", "unsupported version %u", version);
        fclose(fp);
        if (need_cleanup) unlink(dec_path);
        return -1;
    }

    if (fread(&count, sizeof(count), 1, fp) != 1) {
        LOG_ERROR_T("Audit", "Load", "ReadCountFail", "failed to read count");
        fclose(fp);
        if (need_cleanup) unlink(dec_path);
        return -1;
    }
    LOG_DEBUG_T("Audit", "Load", "Count", "count=%u", count);

    if (count > AUDIT_BUF_SIZE) {
        LOG_WARN_T("Audit", "Load", "CountTooLarge", "count=%u > AUDIT_BUF_SIZE=%d, truncating", count, AUDIT_BUF_SIZE);
        count = AUDIT_BUF_SIZE;
    }

    audit_count_val = count;
    audit_head = count;

    for (uint32_t i = 0; i < count; i++) {
        if (fread(&audit_ring[i], sizeof(audit_entry_t), 1, fp) != 1) {
            LOG_ERROR_T("Audit", "Load", "ReadEntryFail", "failed to read entry %u", i);
            fclose(fp);
            if (need_cleanup) unlink(dec_path);
            return -1;
        }
        LOG_DEBUG_T("Audit", "Load", "Entry", "loaded entry %u (idx=%d)", i, i);
    }
    fclose(fp);

    if (need_cleanup) {
        unlink(dec_path);
        LOG_DEBUG_T("Audit", "Load", "Cleanup", "removed decrypted temp file");
    }

    /* 更新 last_hash */
    if (count > 0) {
        uint8_t hash[AUDIT_HASH_SIZE];
        compute_entry_hash(&audit_ring[count-1], hash);
        memcpy(last_hash, hash, AUDIT_HASH_SIZE);
        LOG_DEBUG_T("Audit", "Load", "LastHash", "last_hash updated from last entry");
    } else {
        memset(last_hash, 0, AUDIT_HASH_SIZE);
        LOG_DEBUG_T("Audit", "Load", "LastHash", "last_hash zeroed (no entries)");
    }

    LOG_INFO_T("Audit", "Load", "OK", "loaded %d entries from %s", count, plain_path);
    return 0;
}