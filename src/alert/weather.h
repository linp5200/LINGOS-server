#ifndef ALERT_WEATHER_H
#define ALERT_WEATHER_H

/* 获取天气预警信息（返回字符串，需 free）*/
char *weather_get_alert(void);

/* 设置城市（存储在配置文件中）*/
void weather_set_city(const char *city);

/* 获取当前城市 */
const char *weather_get_city(void);

#endif