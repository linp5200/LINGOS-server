/* alert_sources 真实数据源集成测试（2026-09-18 接线验证） */
#include <stdio.h>
#include <string.h>
#include "alert_sources.h"

const char *tr(const char *en, const char *zh) { return zh ? zh : en; }

int main(void) {
    alert_event_t evs[16];
    memset(evs, 0, sizeof(evs));
    printf("=== alert_sources_fetch_all ===\n");
    int n = alert_sources_fetch_all(evs, 16);
    printf("total events: %d\n\n", n);
    const char *types[] = {"UNKNOWN", "TYPHOON", "EARTHQUAKE", "RAIN", "HIGH_TEMP", "STORM", "FIRE", "HEALTH", "SECURITY"};
    for (int i = 0; i < n && i < 16; i++) {
        alert_event_t *e = &evs[i];
        printf("--- #%d type=%s level=%d source=%s\n    loc=%s\n    desc=%s\n"
               "    mag=%.1f lat=%.3f lon=%.3f dist=%dkm\n",
               i, types[e->type <= 8 ? e->type : 0], e->level, e->source,
               e->location, e->description,
               e->magnitude, e->latitude, e->longitude, e->distance_km);
    }
    return 0;
}
