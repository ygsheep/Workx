/**
 * @file export.h
 * @brief WORKX_API 导出宏（Issue #21 M3：为未来 DLL/插件化预留）
 * @details v1 为静态库：宏为空操作（声明性标注）。
 *         未来转 DLL 时：库侧定义 WORKX_BUILDING_LIBRARY（dllexport），
 *         消费者定义 WORKX_USING_LIBRARY（dllimport）。
 *         非 Windows 使用 -fvisibility=hidden 时默认导出。
 */

#pragma once

#if defined(_WIN32)
#if defined(WORKX_BUILDING_LIBRARY)
#define WORKX_API __declspec(dllexport)
#elif defined(WORKX_USING_LIBRARY)
#define WORKX_API __declspec(dllimport)
#else
#define WORKX_API
#endif
#else
#define WORKX_API __attribute__((visibility("default")))
#endif
