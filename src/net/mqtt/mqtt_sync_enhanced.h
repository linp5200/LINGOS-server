/**
 * @file    mqtt_sync_enhanced.h
 * @brief   多设备同步增强头文件
 * @version LN-B-4.3.0.0
 */

#ifndef NET_MQTT_SYNC_ENHANCED_H
#define NET_MQTT_SYNC_ENHANCED_H

int mqtt_sync_enhanced_init(const char *device_id, int is_server);
int mqtt_sync_enhanced_publish(const char *type, const char *data);
int mqtt_sync_is_server(void);
void mqtt_sync_set_role(int is_server);

#endif /* NET_MQTT_SYNC_ENHANCED_H */