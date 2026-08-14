/**
 * @file    vision_train.h
 * @brief   模型训练接口头文件
 * @version LN-B-4.3.0.0
 */

#ifndef VISION_VISION_TRAIN_H
#define VISION_VISION_TRAIN_H

#include <stddef.h>

int vision_train_start(const char *dataset_path, const char *model_path, int epochs);
int vision_train_status(char *out, size_t out_len);

#endif /* VISION_VISION_TRAIN_H */