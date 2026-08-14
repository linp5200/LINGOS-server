/**
 * @file    vision_train.c
 * @brief   模型训练接口（调用Python训练脚本）
 * @version LN-B-4.3.0.0
 */

#include "vision_train.h"
#include "../common/error_report.h"
#include "../common/safe_string.h"
#include "../lib/log_extra.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

#define TRAIN_SCRIPT "/LINGOS/bin/vision_train.py"

/* ============================================================
 * 训练模型
 * ============================================================ */

int vision_train_start(const char *dataset_path, const char *model_path, int epochs) {
    LOG_INFO_T("VisionTrain", "Start", "Enter", "dataset=%s, epochs=%d", dataset_path, epochs);

    if (!dataset_path || !model_path) {
        LOG_ERROR_T("VisionTrain", "Start", "Invalid", "dataset or model path is NULL");
        return -1;
    }

    /* 检查训练脚本是否存在 */
    if (access(TRAIN_SCRIPT, X_OK) != 0) {
        LOG_ERROR_T("VisionTrain", "Start", "ScriptNotFound", "%s not found", TRAIN_SCRIPT);
        return -1;
    }

    pid_t pid = fork();
    if (pid == -1) {
        LOG_ERROR_T("VisionTrain", "Start", "ForkFail", "fork failed");
        return -1;
    }

    if (pid == 0) {
        /* 子进程：执行训练 */
        char cmd[1024];
        safe_snprintf(cmd, sizeof(cmd), "%s --dataset %s --model %s --epochs %d", TRAIN_SCRIPT, dataset_path, model_path, epochs);
        execlp("python3", "python3", TRAIN_SCRIPT,
               "--dataset", dataset_path,
               "--model", model_path,
               "--epochs", (char[]){'0' + epochs/100, '0' + (epochs/10)%10, '0' + epochs%10, '\0'},
               (char*)NULL);
        _exit(1);
    }

    LOG_INFO_T("VisionTrain", "Start", "OK", "training started, PID=%d", pid);
    return 0;
}

/* ============================================================
 * 获取训练状态
 * ============================================================ */

int vision_train_status(char *out, size_t out_len) {
    if (!out || out_len == 0) return -1;

    /* 简化：检查是否存在进度文件 */
    const char *progress_file = "/tmp/vision_train_progress.txt";
    FILE *fp = fopen(progress_file, "r");
    if (fp) {
        char line[256];
        if (fgets(line, sizeof(line), fp)) {
            safe_strncpy(out, line, out_len);
            fclose(fp);
            return 0;
        }
        fclose(fp);
    }

    safe_strncpy(out, "Training not running", out_len);
    return 0;
}