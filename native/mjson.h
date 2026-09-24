/**
 * mjson.h - 极简 JSON 读写工具
 *
 * 只服务于本项目的 JSON-RPC 协议，不做通用 JSON 库。
 * 解析部分采用"按键扫描"策略：在顶层对象里查找 "key" 并解析其值，
 * 足够处理扁平结构的请求参数，体积远小于完整解析器。
 */
#ifndef MJSON_H
#define MJSON_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------- 写入 ---------------- */

/** 把字符串按 JSON 规则转义后追加到 buf (不含首尾引号)。返回新的写入位置 */
size_t mjson_escape(char *buf, size_t buf_size, size_t pos, const char *s);

/* ---------------- 解析 ---------------- */

/** 是否包含指定的键 (顶层) */
int mjson_has(const char *json, const char *key);

/** 取整数值。成功返回 1，键不存在或类型不符返回 0 */
int mjson_get_int(const char *json, const char *key, long *out);

/** 取布尔值。成功返回 1 */
int mjson_get_bool(const char *json, const char *key, int *out);

/**
 * 取字符串值并反转义, 写入 out。
 * 成功返回 1；键不存在返回 0；类型不符返回 -1。
 */
int mjson_get_str(const char *json, const char *key, char *out, size_t out_size);

/**
 * 取整数数组, 最多取 max_n 个。
 * 成功返回实际元素个数 (>=0)，键不存在或类型不符返回 -1。
 */
int mjson_get_int_array(const char *json, const char *key, long *out, int max_n);

/**
 * 取原始值片段 (不做解析), 用于需要原样转发/判断 null 的场景。
 * 返回指向 json 内部的指针，不是新分配的内存；找不到返回 NULL。
 */
const char *mjson_raw_value(const char *json, const char *key);

/** 判断某个键的值是否为 null */
int mjson_is_null(const char *json, const char *key);

#ifdef __cplusplus
}
#endif

#endif /* MJSON_H */
