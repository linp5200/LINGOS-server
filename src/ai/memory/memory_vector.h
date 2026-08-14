/**
 * @file    memory_vector.h
 * @brief   记忆向量检索接口（语义检索）
 * @version LN-B-4.2.0.0
 */

#ifndef AI_MEMORY_MEMORY_VECTOR_H
#define AI_MEMORY_MEMORY_VECTOR_H

#include <stdint.h>
#include <stddef.h>

#define VECTOR_DIM 384           /* all-MiniLM-L6-v2 向量维度 */
#define MAX_VECTOR_RESULTS 10    /* 最大返回结果数 */

/**
 * @brief 向量检索结果条目
 */
typedef struct {
    char id[64];                /* 记忆 ID */
    char type[16];              /* short / medium / long */
    char summary[512];          /* 内容摘要 */
    float distance;             /* 余弦距离 (越小越相似) */
} vector_result_t;

/**
 * @brief 初始化向量检索系统
 * @return 0 成功，-1 失败
 */
int memory_vector_init(void);

/**
 * @brief 为记忆生成并存储向量
 * @param memory_id 记忆 ID
 * @param content 记忆内容
 * @param type 记忆类型 (short/medium/long)
 * @return 0 成功，-1 失败
 */
int memory_vector_store(const char *memory_id, const char *content, const char *type);

/**
 * @brief 语义检索记忆
 * @param query 查询文本
 * @param top_k 返回结果数量 (最多 MAX_VECTOR_RESULTS)
 * @param results 输出结果数组
 * @param max_results 结果数组大小
 * @return 实际返回结果数量，-1 失败
 */
int memory_vector_search(const char *query, int top_k, vector_result_t *results, int max_results);

/**
 * @brief 删除记忆向量
 * @param memory_id 记忆 ID
 * @return 0 成功，-1 失败
 */
int memory_vector_delete(const char *memory_id);

/**
 * @brief 清理向量检索系统
 */
void memory_vector_cleanup(void);

#endif /* AI_MEMORY_MEMORY_VECTOR_H */