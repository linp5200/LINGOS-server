/**
 * @file    memory_vector.c
 * @brief   记忆向量检索实现（SQLite + sqlite-vec）
 * @version LN-B-5.0.0.0
 * @changes SQLite 线程安全增强；安全字符串替换；双文支持
 */

#include "memory_vector.h"
#include "../../common/data_path.h"
#include "../../common/safe_string.h"
#include "../../common/lang.h"
#include "../../lib/log_extra.h"
#include "../../lib/cJSON/cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sqlite3.h>
#include <math.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <pthread.h>

/* ============================================================
 * 常量定义
 * ============================================================ */

#define VECTOR_DB_PATH "/data/ai_memory/vectors.db"
#define EMBED_SOCKET_PATH "/LINGOS/run/embed.sock"

static sqlite3 *g_db = NULL;
static int g_initialized = 0;
static pthread_mutex_t g_db_lock = PTHREAD_MUTEX_INITIALIZER;  /* 新增：线程安全锁 */

/* ============================================================
 * 内部辅助：获取数据库路径
 * ============================================================ */

static const char* get_db_path(void) {
    static char path[512];
    if (path[0] == '\0') {
        const char *root = lingos_data_root();
        safe_snprintf(path, sizeof(path), "%s%s", root, VECTOR_DB_PATH);
    }
    return path;
}

/* ============================================================
 * 内部辅助：连接 Python 嵌入服务
 * ============================================================ */

static int connect_embed_service(void) {
    LOG_DEBUG_T("MemoryVector", "ConnectEmbed", "Enter", "connecting to %s", EMBED_SOCKET_PATH);

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        LOG_ERROR_T("MemoryVector", "ConnectEmbed", "SocketFail", "socket() error: %s (errno=%d)", strerror(errno), errno);
        return -1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    safe_strncpy(addr.sun_path, EMBED_SOCKET_PATH, sizeof(addr.sun_path));
    addr.sun_path[sizeof(addr.sun_path) - 1] = '\0';

    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        LOG_ERROR_T("MemoryVector", "ConnectEmbed", "ConnectFail", "connect to %s failed: %s", EMBED_SOCKET_PATH, strerror(errno));
        close(fd);
        return -1;
    }

    LOG_DEBUG_T("MemoryVector", "ConnectEmbed", "OK", "connected, fd=%d", fd);
    return fd;
}

/* ============================================================
 * 内部辅助：获取向量嵌入（通过 Unix Socket）
 * ============================================================ */

static int get_embedding(const char *text, float *vector, int dim) {
    LOG_DEBUG_T("MemoryVector", "GetEmbedding", "Enter", "text_len=%zu, dim=%d", text ? strlen(text) : 0, dim);

    if (!text || !vector || dim != VECTOR_DIM) {
        LOG_ERROR_T("MemoryVector", "GetEmbedding", "Invalid", "text=%p, vector=%p, dim=%d", (void*)text, (void*)vector, dim);
        return -1;
    }

    int fd = connect_embed_service();
    if (fd < 0) {
        LOG_WARN_T("MemoryVector", "GetEmbedding", "ConnectFail", "cannot connect to embed service, using fallback");
        srand(time(NULL) ^ (uintptr_t)text);
        for (int i = 0; i < dim; i++) {
            vector[i] = ((float)rand() / RAND_MAX) * 2.0 - 1.0;
        }
        float norm = 0.0f;
        for (int i = 0; i < dim; i++) {
            norm += vector[i] * vector[i];
        }
        norm = sqrtf(norm);
        if (norm > 0.0001f) {
            for (int i = 0; i < dim; i++) {
                vector[i] /= norm;
            }
        }
        return 0;
    }

    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "cmd", "embed");
    cJSON_AddStringToObject(req, "text", text);
    char *json_str = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);

    if (!json_str) {
        LOG_ERROR_T("MemoryVector", "GetEmbedding", "JSONFail", "cJSON_PrintUnformatted failed");
        close(fd);
        return -1;
    }

    LOG_DEBUG_T("MemoryVector", "GetEmbedding", "Request", "sending %s", json_str);

    if (write(fd, json_str, strlen(json_str)) < 0 || write(fd, "\n", 1) < 0) {
        LOG_ERROR_T("MemoryVector", "GetEmbedding", "WriteFail", "write failed: %s", strerror(errno));
        free(json_str);
        close(fd);
        return -1;
    }
    free(json_str);

    char buf[8192];
    int pos = 0;
    while (pos < (int)sizeof(buf) - 1) {
        ssize_t n = read(fd, buf + pos, 1);
        if (n <= 0) break;
        if (buf[pos] == '\n') {
            buf[pos] = '\0';
            break;
        }
        pos++;
    }
    close(fd);

    if (pos == 0) {
        LOG_ERROR_T("MemoryVector", "GetEmbedding", "ReadFail", "no response from embed service");
        return -1;
    }

    LOG_DEBUG_T("MemoryVector", "GetEmbedding", "Response", "raw='%s'", buf);

    cJSON *resp = cJSON_Parse(buf);
    if (!resp) {
        LOG_ERROR_T("MemoryVector", "GetEmbedding", "ParseFail", "invalid JSON response");
        return -1;
    }

    cJSON *status = cJSON_GetObjectItem(resp, "status");
    if (!status || strcmp(status->valuestring, "ok") != 0) {
        cJSON *err = cJSON_GetObjectItem(resp, "error");
        LOG_ERROR_T("MemoryVector", "GetEmbedding", "Error", "embed service error: %s", err ? err->valuestring : "unknown");
        cJSON_Delete(resp);
        return -1;
    }

    cJSON *embedding = cJSON_GetObjectItem(resp, "embedding");
    if (!embedding || !cJSON_IsArray(embedding)) {
        LOG_ERROR_T("MemoryVector", "GetEmbedding", "NoEmbedding", "no embedding in response");
        cJSON_Delete(resp);
        return -1;
    }

    int size = cJSON_GetArraySize(embedding);
    if (size != dim) {
        LOG_ERROR_T("MemoryVector", "GetEmbedding", "DimMismatch", "expected %d, got %d", dim, size);
        cJSON_Delete(resp);
        return -1;
    }

    for (int i = 0; i < dim; i++) {
        cJSON *item = cJSON_GetArrayItem(embedding, i);
        if (item && cJSON_IsNumber(item)) {
            vector[i] = (float)item->valuedouble;
        } else {
            vector[i] = 0.0f;
        }
    }

    cJSON_Delete(resp);
    LOG_DEBUG_T("MemoryVector", "GetEmbedding", "OK", "received %d-dim embedding", dim);
    return 0;
}

/* ============================================================
 * 【修改】内部辅助：SQLite 数据库操作（线程安全）
 * ============================================================ */

static int open_db(void) {
    pthread_mutex_lock(&g_db_lock);

    if (g_db) {
        pthread_mutex_unlock(&g_db_lock);
        return 0;
    }

    const char *path = get_db_path();

    char dir[512];
    const char *root = lingos_data_root();
    safe_snprintf(dir, sizeof(dir), "%s/data/ai_memory", root);
    if (access(dir, F_OK) != 0) {
        if (mkdir(dir, 0755) != 0) {
            LOG_ERROR_T("MemoryVector", "OpenDB", "MkdirFail", "cannot create %s", dir);
            pthread_mutex_unlock(&g_db_lock);
            return -1;
        }
    }

    int rc = sqlite3_open(path, &g_db);
    if (rc != SQLITE_OK) {
        LOG_ERROR_T("MemoryVector", "OpenDB", "Fail", "sqlite3_open %s failed: %s", path, sqlite3_errmsg(g_db));
        sqlite3_close(g_db);
        g_db = NULL;
        pthread_mutex_unlock(&g_db_lock);
        return -1;
    }

    /* 启用 WAL 模式（提高并发性能） */
    sqlite3_exec(g_db, "PRAGMA journal_mode=WAL", NULL, NULL, NULL);
    sqlite3_exec(g_db, "PRAGMA synchronous=NORMAL", NULL, NULL, NULL);

    const char *create_sql =
        "CREATE TABLE IF NOT EXISTS memory_vectors ("
        "    id TEXT PRIMARY KEY,"
        "    memory_id TEXT NOT NULL,"
        "    type TEXT NOT NULL,"
        "    content TEXT,"
        "    embedding BLOB NOT NULL,"
        "    created_at INTEGER DEFAULT (strftime('%s','now'))"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_memory_id ON memory_vectors(memory_id);"
        "CREATE INDEX IF NOT EXISTS idx_type ON memory_vectors(type);";

    char *errmsg = NULL;
    rc = sqlite3_exec(g_db, create_sql, NULL, NULL, &errmsg);
    if (rc != SQLITE_OK) {
        LOG_ERROR_T("MemoryVector", "OpenDB", "CreateTableFail", "%s", errmsg);
        sqlite3_free(errmsg);
        sqlite3_close(g_db);
        g_db = NULL;
        pthread_mutex_unlock(&g_db_lock);
        return -1;
    }

    pthread_mutex_unlock(&g_db_lock);
    LOG_INFO_T("MemoryVector", "OpenDB", "OK", "database opened at %s (WAL mode enabled)", path);
    return 0;
}

/* ============================================================
 * 内部辅助：向量相似度计算（余弦相似度）
 * ============================================================ */

static float cosine_distance(const float *a, const float *b, int dim) {
    float dot = 0.0f, norm_a = 0.0f, norm_b = 0.0f;
    for (int i = 0; i < dim; i++) {
        dot += a[i] * b[i];
        norm_a += a[i] * a[i];
        norm_b += b[i] * b[i];
    }
    if (norm_a < 1e-10f || norm_b < 1e-10f) return 1.0f;
    return 1.0f - (dot / (sqrtf(norm_a) * sqrtf(norm_b)));
}

/* ============================================================
 * 【修改】公共 API 实现（线程安全）
 * ============================================================ */

int memory_vector_init(void) {
    LOG_INFO_T("MemoryVector", "Init", "Enter", "initializing memory vector system");

    if (g_initialized) {
        LOG_DEBUG_T("MemoryVector", "Init", "Already", "already initialized");
        return 0;
    }

    if (open_db() != 0) {
        LOG_ERROR_T("MemoryVector", "Init", "OpenDBFail", "failed to open database");
        return -1;
    }

    g_initialized = 1;
    LOG_INFO_T("MemoryVector", "Init", "OK", "memory vector system ready");
    return 0;
}

int memory_vector_store(const char *memory_id, const char *content, const char *type) {
    LOG_INFO_T("MemoryVector", "Store", "Enter", "memory_id='%s', content_len=%zu, type='%s'",
               memory_id ? memory_id : "(null)", content ? strlen(content) : 0, type ? type : "(null)");

    if (!memory_id || !content || !type) {
        LOG_ERROR_T("MemoryVector", "Store", "Invalid", "memory_id=%p, content=%p, type=%p",
                    (void*)memory_id, (void*)content, (void*)type);
        return -1;
    }

    if (!g_initialized) {
        if (memory_vector_init() != 0) {
            LOG_ERROR_T("MemoryVector", "Store", "InitFail", "vector system not initialized");
            return -1;
        }
    }

    float embedding[VECTOR_DIM];
    if (get_embedding(content, embedding, VECTOR_DIM) != 0) {
        LOG_WARN_T("MemoryVector", "Store", "EmbedFail", "fallback to random vector for %s", memory_id);
    }

    pthread_mutex_lock(&g_db_lock);

    if (!g_db) {
        pthread_mutex_unlock(&g_db_lock);
        LOG_ERROR_T("MemoryVector", "Store", "NoDB", "database not open");
        return -1;
    }

    sqlite3_stmt *stmt;
    const char *sql = "INSERT OR REPLACE INTO memory_vectors (id, memory_id, type, content, embedding) VALUES (?, ?, ?, ?, ?)";
    int rc = sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        LOG_ERROR_T("MemoryVector", "Store", "PrepareFail", "sqlite3_prepare: %s", sqlite3_errmsg(g_db));
        pthread_mutex_unlock(&g_db_lock);
        return -1;
    }

    char id[64];
    safe_snprintf(id, sizeof(id), "vec_%s", memory_id);

    sqlite3_bind_text(stmt, 1, id, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, memory_id, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, type, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 4, content, -1, SQLITE_STATIC);
    sqlite3_bind_blob(stmt, 5, embedding, sizeof(embedding), SQLITE_STATIC);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    pthread_mutex_unlock(&g_db_lock);

    if (rc != SQLITE_DONE) {
        LOG_ERROR_T("MemoryVector", "Store", "StepFail", "sqlite3_step: %s", sqlite3_errmsg(g_db));
        return -1;
    }

    LOG_INFO_T("MemoryVector", "Store", "OK", "stored vector for memory_id=%s", memory_id);
    return 0;
}

int memory_vector_search(const char *query, int top_k, vector_result_t *results, int max_results) {
    LOG_INFO_T("MemoryVector", "Search", "Enter", "query='%s', top_k=%d, max_results=%d",
               query ? query : "(null)", top_k, max_results);

    if (!query || !results || max_results <= 0) {
        LOG_ERROR_T("MemoryVector", "Search", "Invalid", "query=%p, results=%p, max_results=%d",
                    (void*)query, (void*)results, max_results);
        return -1;
    }

    if (top_k > max_results) top_k = max_results;
    if (top_k > MAX_VECTOR_RESULTS) top_k = MAX_VECTOR_RESULTS;

    if (!g_initialized) {
        if (memory_vector_init() != 0) {
            LOG_ERROR_T("MemoryVector", "Search", "InitFail", "vector system not initialized");
            return -1;
        }
    }

    float query_vec[VECTOR_DIM];
    if (get_embedding(query, query_vec, VECTOR_DIM) != 0) {
        LOG_WARN_T("MemoryVector", "Search", "EmbedFail", "cannot generate query embedding, search may be inaccurate");
    }

    pthread_mutex_lock(&g_db_lock);

    if (!g_db) {
        pthread_mutex_unlock(&g_db_lock);
        LOG_ERROR_T("MemoryVector", "Search", "NoDB", "database not open");
        return -1;
    }

    sqlite3_stmt *stmt;
    const char *sql = "SELECT memory_id, type, content, embedding FROM memory_vectors";
    int rc = sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        LOG_ERROR_T("MemoryVector", "Search", "PrepareFail", "sqlite3_prepare: %s", sqlite3_errmsg(g_db));
        pthread_mutex_unlock(&g_db_lock);
        return -1;
    }

    typedef struct {
        char id[64];
        char type[16];
        char summary[512];
        float distance;
    } temp_result_t;

    temp_result_t temp[MAX_VECTOR_RESULTS];
    int count = 0;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *mem_id = (const char*)sqlite3_column_text(stmt, 0);
        const char *type = (const char*)sqlite3_column_text(stmt, 1);
        const char *content = (const char*)sqlite3_column_text(stmt, 2);
        const float *vec = (const float*)sqlite3_column_blob(stmt, 3);
        int blob_size = sqlite3_column_bytes(stmt, 3);

        if (!mem_id || !type || !content || !vec || blob_size != sizeof(float) * VECTOR_DIM) {
            LOG_WARN_T("MemoryVector", "Search", "Skip", "invalid row");
            continue;
        }

        float dist = cosine_distance(query_vec, vec, VECTOR_DIM);

        int pos = count;
        for (int i = 0; i < count && i < top_k; i++) {
            if (dist < temp[i].distance) {
                pos = i;
                break;
            }
        }

        if (pos < top_k) {
            for (int i = (count < top_k ? count : top_k - 1); i > pos; i--) {
                memcpy(&temp[i], &temp[i - 1], sizeof(temp_result_t));
            }
            safe_strncpy(temp[pos].id, mem_id, sizeof(temp[pos].id));
            safe_strncpy(temp[pos].type, type, sizeof(temp[pos].type));
            safe_strncpy(temp[pos].summary, content ? content : "", sizeof(temp[pos].summary));
            temp[pos].distance = dist;
            if (count < top_k) count++;
        }
    }

    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&g_db_lock);

    int result_count = count < top_k ? count : top_k;
    for (int i = 0; i < result_count; i++) {
        safe_strncpy(results[i].id, temp[i].id, sizeof(results[i].id));
        safe_strncpy(results[i].type, temp[i].type, sizeof(results[i].type));
        safe_strncpy(results[i].summary, temp[i].summary, sizeof(results[i].summary));
        results[i].distance = temp[i].distance;
    }

    LOG_INFO_T("MemoryVector", "Search", "OK", "found %d results for query", result_count);
    return result_count;
}

int memory_vector_delete(const char *memory_id) {
    LOG_INFO_T("MemoryVector", "Delete", "Enter", "memory_id='%s'", memory_id ? memory_id : "(null)");

    if (!memory_id) {
        LOG_ERROR_T("MemoryVector", "Delete", "Invalid", "memory_id is NULL");
        return -1;
    }

    if (!g_initialized) {
        LOG_WARN_T("MemoryVector", "Delete", "NotInit", "vector system not initialized");
        return 0;
    }

    pthread_mutex_lock(&g_db_lock);

    if (!g_db) {
        pthread_mutex_unlock(&g_db_lock);
        LOG_ERROR_T("MemoryVector", "Delete", "NoDB", "database not open");
        return -1;
    }

    sqlite3_stmt *stmt;
    const char *sql = "DELETE FROM memory_vectors WHERE memory_id = ?";
    int rc = sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        LOG_ERROR_T("MemoryVector", "Delete", "PrepareFail", "sqlite3_prepare: %s", sqlite3_errmsg(g_db));
        pthread_mutex_unlock(&g_db_lock);
        return -1;
    }

    sqlite3_bind_text(stmt, 1, memory_id, -1, SQLITE_STATIC);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    pthread_mutex_unlock(&g_db_lock);

    if (rc != SQLITE_DONE) {
        LOG_ERROR_T("MemoryVector", "Delete", "StepFail", "sqlite3_step: %s", sqlite3_errmsg(g_db));
        return -1;
    }

    LOG_INFO_T("MemoryVector", "Delete", "OK", "deleted vector for memory_id=%s", memory_id);
    return 0;
}

void memory_vector_cleanup(void) {
    LOG_INFO_T("MemoryVector", "Cleanup", "Enter", "cleaning up vector system");

    pthread_mutex_lock(&g_db_lock);

    if (g_db) {
        sqlite3_close(g_db);
        g_db = NULL;
    }
    g_initialized = 0;

    pthread_mutex_unlock(&g_db_lock);

    LOG_INFO_T("MemoryVector", "Cleanup", "OK", "vector system cleaned up");
}