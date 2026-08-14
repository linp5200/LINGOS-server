/**
 * @file    snapshot_diff.c
 * @brief   快照差异比较（增强版）
 * @version LN-B-4.2.0.0
 */

#include "snapshot.h"
#include "log_extra.h"
#include "safe_string.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int snapshot_diff_detailed(const char *id, char *out, size_t out_len, int include_content) {
    LOG_INFO_T("SnapshotDiff", "Detailed", "Enter", "id='%s', include_content=%d",
               id ? id : "(null)", include_content);

    if (!id || !out || out_len == 0) {
        return -1;
    }

    const char *snap_dir = snapshot_get_dir();
    char snap_path[512];
    safe_snprintf(snap_path, sizeof(snap_path), "%s/%s", snap_dir, id);

    if (access(snap_path, F_OK) != 0) {
        safe_snprintf(out, out_len, "Snapshot not found");
        return -1;
    }

    FILE *fp;
    if (include_content) {
        fp = popen("diff -ur '/LINGOS/system/config' '/LINGOS/snapshots/snapshot_*/system/config' 2>/dev/null | head -200", "r");
    } else {
        fp = popen("diff -rq '/LINGOS/system/config' '/LINGOS/snapshots/snapshot_*/system/config' 2>/dev/null | head -100", "r");
    }

    if (!fp) {
        safe_snprintf(out, out_len, "Failed to run diff");
        return -1;
    }

    size_t pos = 0;
    char line[256];
    int line_count = 0;
    while (fgets(line, sizeof(line), fp) && pos < out_len - 1 && line_count < 100) {
        pos += safe_snprintf(out + pos, out_len - pos, "%s", line);
        line_count++;
    }
    pclose(fp);

    if (pos == 0) {
        safe_snprintf(out, out_len, "No differences found");
    }

    return 0;
}