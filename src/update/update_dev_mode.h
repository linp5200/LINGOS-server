/**
 * @file    update_dev_mode.h
 * @brief   调试版本号头文件
 * @version LN-B-4.3.0.0
 */

#ifndef UPDATE_DEV_MODE_H
#define UPDATE_DEV_MODE_H

int update_dev_mode_is_enabled(void);
int update_dev_mode_set_version(const char *version);

#endif /* UPDATE_DEV_MODE_H */