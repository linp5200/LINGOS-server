#ifndef COMMON_TYPES_H
#define COMMON_TYPES_H

/*
 * 在 Linux 用户态编译时，所有基本类型（uint64_t, size_t 等）
 * 均由系统标准头文件提供。我们手动定义的类型会与之冲突，
 * 因此直接跳过整个头文件。
 */
#ifndef __linux__
/* ===== 裸机环境下的基本类型定义 ===== */
typedef unsigned char      uint8_t;
typedef unsigned short     uint16_t;
typedef unsigned int       uint32_t;
typedef unsigned long long uint64_t;

typedef signed char        int8_t;
typedef signed short       int16_t;
typedef signed int         int32_t;
typedef signed long long   int64_t;

typedef uint64_t           uintptr_t;
typedef int64_t            intptr_t;
typedef long               ptrdiff_t;
typedef int64_t            intmax_t;
typedef uint64_t           uintmax_t;
typedef uint64_t           size_t;

typedef uint8_t bool;
#define true  1
#define false 0

typedef __builtin_va_list va_list;
#define va_start(v, l) __builtin_va_start(v, l)
#define va_end(v)       __builtin_va_end(v)
#define va_copy(d, s)   __builtin_va_copy(d, s)
#define va_arg(v, l)    __builtin_va_arg(v, l)

typedef uint8_t  uint_fast8_t;
typedef uint16_t uint_fast16_t;
typedef uint32_t uint_fast32_t;
typedef uint64_t uint_fast64_t;

typedef int8_t  int_fast8_t;
typedef int16_t int_fast16_t;
typedef int32_t int_fast32_t;
typedef int64_t int_fast64_t;

#define INT8_MIN   (-128)
#define INT8_MAX   127
#define INT16_MIN  (-32768)
#define INT16_MAX  32767
#define INT32_MIN  (-2147483647 - 1)
#define INT32_MAX  2147483647
#define INT64_MIN  (-9223372036854775807LL - 1LL)
#define INT64_MAX  9223372036854775807LL

#define UINT8_MAX  255U
#define UINT16_MAX 65535U
#define UINT32_MAX 4294967295U
#define UINT64_MAX 18446744073709551615ULL

#define CHAR_BIT 8
#define offsetof(type, member) __builtin_offsetof(type, member)

#define NULL ((void*)0)

#endif /* !__linux__ */
#endif/* COMMON_TYPES_H */