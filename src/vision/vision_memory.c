/**
 * @file    vision_memory.c
 * @brief   视觉位置记忆存储（SQLite）
 * @version LN-B-5.0.0.0
 * @changes Schema 版本校验；安全字符串替换
 */

#include "vision_memory.h"
#include "../common/safe_string.h"
#include "../common/data_path.h"
#include "../common/lang.h"
#include "../lib/log_extra.h"
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sqlite3.h>
#include <time.h>

#define DB_PATH "/data/vision/vision.db"
#define SCHEMA_VERSION 2

static sqlite3 *g_db = NULL;
static int g_initialized = 0;

/* ============================================================
 * 获取数据库路径
 * ============================================================ */

static const char* get_db_path(void) {
    static char path[512];
    if (path[0] == '\0') {
        const char *root = lingos_data_root();
        safe_snprintf(path, sizeof(path), "%s%s", root, DB_PATH);
    }
    return path;
}

/* ============================================================
 * 【修改】初始化（含 Schema 版本校验）
 * ============================================================ */

int vision_memory_init(void) {
    LOG_DEBUG_T("VisionMemory", "Init", "Enter", "vision_memory_init run");

    const char *path = get_db_path();
    char dir[512];
    const char *root = lingos_data_root();
    safe_snprintf(dir, sizeof(dir), "%s/data/vision", root);
    mkdir(dir, 0755);

    if (sqlite3_open(path, &g_db) != SQLITE_OK) {
        LOG_ERROR_T("VisionMemory", "Init", "OpenFail", "sqlite3_open failed");
        return -1;
    }

    const char *create_sql =
        "CREATE TABLE IF NOT EXISTS objects ("
        "    id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "    label TEXT NOT NULL,"
        "    world_x REAL,"
        "    world_y REAL,"
        "    confidence REAL,"
        "    timestamp INTEGER,"
        "    track_id INTEGER"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_label ON objects(label);"
        "CREATE INDEX IF NOT EXISTS idx_timestamp ON objects(timestamp);";

    char *errmsg = NULL;
    if (sqlite3_exec(g_db, create_sql, NULL, NULL, &errmsg) != SQLITE_OK) {
        LOG_ERROR_T("VisionMemory", "Init", "CreateTableFail", "%s", errmsg);
        sqlite3_free(errmsg);
        sqlite3_close(g_db);
        g_db = NULL;
        return -1;
    }

    /* Schema 版本校验 */
    int user_version = 0;
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(g_db, "PRAGMA user_version", -1, &stmt, NULL) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            user_version = sqlite3_column_int(stmt, 0);
        }
        sqlite3_finalize(stmt);
    }

    if (user_version < SCHEMA_VERSION) {
        LOG_INFO_T("VisionMemory", "Init", "SchemaUpgrade", "upgrading schema from %d to %d",
                   user_version, SCHEMA_VERSION);
        char upgrade_sql[256];
        safe_snprintf(upgrade_sql, sizeof(upgrade_sql),
                      "PRAGMA user_version = %d", SCHEMA_VERSION);
        sqlite3_exec(g_db, upgrade_sql, NULL, NULL, NULL);
    }

    g_initialized = 1;
    LOG_INFO_T("VisionMemory", "Init", "OK", "database initialized, schema v%d", SCHEMA_VERSION);
    return 0;
}

int vision_memory_save(const detection_result_t *result) {
    if (!g_initialized || !result) return -1;

    sqlite3_stmt *stmt;
    const char *sql = "INSERT INTO objects (label, world_x, world_y, confidence, timestamp, track_id) "
                      "VALUES (?, ?, ?, ?, ?, ?)";

    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        LOG_ERROR_T("VisionMemory", "Save", "PrepareFail", "%s", sqlite3_errmsg(g_db));
        return -1;
    }

    sqlite3_bind_text(stmt, 1, result->label, -1, SQLITE_STATIC);
    sqlite3_bind_double(stmt, 2, result->world_x);
    sqlite3_bind_double(stmt, 3, result->world_y);
    sqlite3_bind_double(stmt, 4, result->confidence);
    sqlite3_bind_int64(stmt, 5, (sqlite3_int64)time(NULL));
    sqlite3_bind_int(stmt, 6, result->track_id);

    int ret = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (ret != SQLITE_DONE) {
        LOG_WARN_T("VisionMemory", "Save", "StepFail", "%s", sqlite3_errmsg(g_db));
        return -1;
    }

    LOG_DEBUG_T("VisionMemory", "Save", "OK", "saved %s at (%.1f, %.1f)", result->label, result->world_x, result->world_y);
    return 0;
}

int vision_memory_locate(const char *label, vision_location_t *out, int max_count) {
    if (!g_initialized || !label || !out || max_count <= 0) return 0;

    sqlite3_stmt *stmt;
    const char *sql = "SELECT label, world_x, world_y, timestamp, track_id FROM objects "
                      "WHERE label = ? ORDER BY timestamp DESC LIMIT ?";

    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        LOG_ERROR_T("VisionMemory", "Locate", "PrepareFail", "%s", sqlite3_errmsg(g_db));
        return 0;
    }

    sqlite3_bind_text(stmt, 1, label, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 2, max_count);

    int count = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW && count < max_count) {
        const char *lbl = (const char*)sqlite3_column_text(stmt, 0);
        double wx = sqlite3_column_double(stmt, 1);
        double wy = sqlite3_column_double(stmt, 2);
        time_t ts = (time_t)sqlite3_column_int64(stmt, 3);
        int tid = sqlite3_column_int(stmt, 4);

        safe_strncpy(out[count].label, lbl ? lbl : "", sizeof(out[count].label));
        out[count].world_x = wx;
        out[count].world_y = wy;
        out[count].timestamp = ts;
        out[count].track_id = tid;
        count++;
    }

    sqlite3_finalize(stmt);
    LOG_DEBUG_T("VisionMemory", "Locate", "OK", "found %d records for %s", count, label);
    return count;
}

void vision_memory_cleanup(void) {
    if (g_db) {
        sqlite3_close(g_db);
        g_db = NULL;
    }
    g_initialized = 0;
    LOG_DEBUG_T("VisionMemory", "Cleanup", "OK", "vision memory cleaned up");
}