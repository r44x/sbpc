/* Minimal <stdint.h> for compiling SBPC's generated code with TinyCC, so the build needs no system
 * C headers (a Windows PC usually has none). Only what sh2_state.h / sh2_ops.h use. */
#ifndef SBPC_TCC_STDINT_H
#define SBPC_TCC_STDINT_H
#include <stddef.h>
typedef signed char int8_t;
typedef unsigned char uint8_t;
typedef short int16_t;
typedef unsigned short uint16_t;
typedef int int32_t;
typedef unsigned int uint32_t;
typedef long long int64_t;
typedef unsigned long long uint64_t;
typedef __SIZE_TYPE__ uintptr_t;
#define INT32_MIN (-2147483647 - 1)
#define INT32_MAX 2147483647
#define UINT32_MAX 0xFFFFFFFFu
#define INT64_MAX 0x7FFFFFFFFFFFFFFFLL
#define INT64_MIN (-INT64_MAX - 1)
#endif
