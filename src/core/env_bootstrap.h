#ifndef CORE_ENV_BOOTSTRAP_H
#define CORE_ENV_BOOTSTRAP_H

/**
 * @brief 确保运行时环境就绪：创建目录、复制Python脚本等
 * @return 0 成功，-1 失败
 */
int ensure_runtime_environment(void);

#endif