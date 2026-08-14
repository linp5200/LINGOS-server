#ifndef PLATFORM_H
#define PLATFORM_H

#include "lingos_config.h"

/* ==================== 1. Linux 用户态 ==================== */
#ifdef LINGOS_PLATFORM_LINUX

/* 引入系统标准头文件（所有 .c 文件只需要包含 platform.h 即可获得这些） */
#include <string.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>

/* 消除系统定义的 true / false / NULL 并统一为我们项目要求的样式 */
#ifdef true
#undef true
#endif
#define true  1

#ifdef false
#undef false
#endif
#define false 0

#ifdef NULL
#undef NULL
#endif
#define NULL ((void*)0)

#endif /* LINGOS_PLATFORM_LINUX */

/* ==================== 2. 裸机环境 ==================== */
#ifdef LINGOS_PLATFORM_BAREMETAL

#include "../common/types.h"              /* 基本类型、bool、NULL 等 */
#include "../common/string_no_sys.h"   /* 新：只包含头文件，不再直接声明 */

#endif /* LINGOS_PLATFORM_BAREMETAL */

/* ==================== 3. 通用类型别名 ==================== */
typedef uint8_t   u8_t;
typedef int8_t    s8_t;
typedef uint16_t  u16_t;
typedef int16_t   s16_t;
typedef uint32_t  u32_t;
typedef int32_t   s32_t;
typedef uint64_t  u64_t;
typedef int64_t   s64_t;

#endif /* PLATFORM_H */