/* 测试 https_client：拉取真实数据源（2026-09-18 接线调试用） */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "https_client.h"

static void try_fetch(const char *name, const char *url, int verify) {
    int code = -1;
    char *body = https_get_alloc(url, 512 * 1024, 12, verify, &code);
    if (!body) {
        printf("[%s] FAIL (no body, code=%d)\n", name, code);
        return;
    }
    size_t len = strlen(body);
    printf("[%s] OK http=%d len=%zu\n", name, code, len);
    /* 打印前 220 字符 */
    char prev[221];
    size_t n = len < 220 ? len : 220;
    memcpy(prev, body, n);
    prev[n] = '\0';
    for (size_t i = 0; i < n; i++) {
        if (prev[i] == '\n' || prev[i] == '\r') prev[i] = ' ';
    }
    printf("    %s\n", prev);
    free(body);
}

int main(void) {
    try_fetch("cenc_eew", "https://api.wolfx.jp/cenc_eew.json", 0);
    try_fetch("jma_eew", "https://api.wolfx.jp/jma_eew.json", 0);
    try_fetch("nmc_list", "http://typhoon.nmc.cn/weatherservice/typhoon/jsons/list_default", 0);
    try_fetch("usgs", "https://earthquake.usgs.gov/earthquakes/feed/v1.0/summary/2.5_day.geojson", 0);
    return 0;
}
