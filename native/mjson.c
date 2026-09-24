/**
 * mjson.c - 极简 JSON 读写工具实现
 *
 * 解析策略: 单遍扫描, 只在"键位置"(紧跟 '{' 或 ',' 之后) 识别字符串作为键,
 * 因此不会把字符串**值**误认成键, 嵌套对象也能正确定位。
 */
#include "mjson.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ *
 *  内部工具
 * ------------------------------------------------------------------ */

static int is_ws(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

/** 跳过空白 */
static const char *skip_ws(const char *p)
{
    while (p && *p && is_ws(*p)) {
        p++;
    }
    return p;
}

/**
 * 从 p (指向开引号) 开始读取一个 JSON 字符串, 反转义后写入 out。
 * 返回指向闭引号之后的位置; 失败返回 NULL。
 */
static const char *read_string(const char *p, char *out, size_t out_size)
{
    size_t pos = 0;

    if (!p || *p != '"') {
        return NULL;
    }
    p++; /* 跳过开引号 */

    while (*p && *p != '"') {
        char c = *p;

        if (c == '\\') {
            p++;
            switch (*p) {
            case '"':  c = '"';  p++; break;
            case '\\': c = '\\'; p++; break;
            case '/':  c = '/';  p++; break;
            case 'b':  c = '\b'; p++; break;
            case 'f':  c = '\f'; p++; break;
            case 'n':  c = '\n'; p++; break;
            case 'r':  c = '\r'; p++; break;
            case 't':  c = '\t'; p++; break;
            case 'u': {
                /* 解析 \uXXXX, 按 UTF-8 编码输出 (BMP 范围) */
                unsigned int cp = 0;
                int k;
                p++;
                for (k = 0; k < 4 && p[k]; k++) {
                    char h = p[k];
                    unsigned int d;
                    if (h >= '0' && h <= '9')      d = (unsigned int)(h - '0');
                    else if (h >= 'a' && h <= 'f') d = (unsigned int)(h - 'a' + 10);
                    else if (h >= 'A' && h <= 'F') d = (unsigned int)(h - 'A' + 10);
                    else break;
                    cp = cp * 16u + d;
                }
                p += k;
                if (cp < 0x80u) {
                    if (out && pos + 1 < out_size) out[pos] = (char)cp;
                    pos++;
                } else if (cp < 0x800u) {
                    if (out && pos + 2 < out_size) {
                        out[pos]     = (char)(0xC0u | (cp >> 6));
                        out[pos + 1] = (char)(0x80u | (cp & 0x3Fu));
                    }
                    pos += 2;
                } else {
                    if (out && pos + 3 < out_size) {
                        out[pos]     = (char)(0xE0u | (cp >> 12));
                        out[pos + 1] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
                        out[pos + 2] = (char)(0x80u | (cp & 0x3Fu));
                    }
                    pos += 3;
                }
                continue;
            }
            default:  c = *p; p++; break;
            }
        } else {
            p++;
        }

        if (out && pos + 1 < out_size) {
            out[pos] = c;
        }
        pos++;
    }

    if (*p != '"') {
        return NULL; /* 未闭合 */
    }
    p++;

    if (out) {
        size_t end = (pos < out_size) ? pos : (out_size ? out_size - 1 : 0);
        out[end] = '\0';
    }
    return p;
}

/** 跳过任意 JSON 值, 返回其后的位置 */
static const char *skip_value(const char *p)
{
    int depth = 0;

    p = skip_ws(p);
    if (!p || !*p) {
        return p;
    }

    if (*p == '"') {
        return read_string(p, NULL, 0);
    }

    /* 对象 / 数组: 用括号配对跳过, 同时跳过内部字符串 */
    if (*p == '{' || *p == '[') {
        for (; *p; p++) {
            if (*p == '"') {
                p = read_string(p, NULL, 0);
                if (!p) return NULL;
                p--;
                continue;
            }
            if (*p == '{' || *p == '[') {
                depth++;
            } else if (*p == '}' || *p == ']') {
                depth--;
                if (depth == 0) {
                    return p + 1;
                }
            }
        }
        return NULL;
    }

    /* 数字 / true / false / null: 读到分隔符为止 */
    while (*p && *p != ',' && *p != '}' && *p != ']' && !is_ws(*p)) {
        p++;
    }
    return p;
}

/**
 * 在顶层对象 json 中查找键 key, 返回指向其值起始处的指针 (冒号之后, 已跳过空白)。
 *
 * 采用逐个 "键: 值" 配对的方式推进, 因此不会把字符串**值**误认成键,
 * 嵌套的对象/数组也会被 skip_value() 整体跳过。
 */
static const char *find_value(const char *json, const char *key)
{
    const char *p;

    if (!json || !key) {
        return NULL;
    }

    p = skip_ws(json);
    if (*p != '{') {
        return NULL;
    }
    p = skip_ws(p + 1);
    if (*p == '}') {
        return NULL;   /* 空对象 */
    }

    for (;;) {
        char kbuf[128];

        p = skip_ws(p);
        if (*p != '"') {
            return NULL;
        }
        p = read_string(p, kbuf, sizeof(kbuf));
        if (!p) {
            return NULL;
        }

        p = skip_ws(p);
        if (*p != ':') {
            return NULL;
        }
        p = skip_ws(p + 1);

        if (strcmp(kbuf, key) == 0) {
            return p;
        }

        p = skip_value(p);
        if (!p) {
            return NULL;
        }
        p = skip_ws(p);

        if (*p == ',') {
            p++;
            continue;
        }
        return NULL;   /* '}' 或格式错误: 没找到 */
    }
}

/* ------------------------------------------------------------------ *
 *  写入
 * ------------------------------------------------------------------ */

size_t mjson_escape(char *buf, size_t buf_size, size_t pos, const char *s)
{
    size_t i;
    if (!buf || !s) {
        return pos;
    }
    for (i = 0; s[i] != '\0'; i++) {
        unsigned char c = (unsigned char)s[i];
        const char *esc = NULL;
        switch (c) {
        case '"':  esc = "\\\""; break;
        case '\\': esc = "\\\\"; break;
        case '\n': esc = "\\n";  break;
        case '\r': esc = "\\r";  break;
        case '\t': esc = "\\t";  break;
        default: break;
        }
        if (esc) {
            if (pos + 2 < buf_size) {
                buf[pos] = esc[0];
                buf[pos + 1] = esc[1];
            }
            pos += 2;
        } else if (c < 0x20) {
            if (pos + 6 < buf_size) {
                sprintf(buf + pos, "\\u%04x", c);
            }
            pos += 6;
        } else {
            if (pos + 1 < buf_size) {
                buf[pos] = (char)c;
            }
            pos++;
        }
    }
    if (pos < buf_size) {
        buf[pos] = '\0';
    }
    return pos;
}

/* ------------------------------------------------------------------ *
 *  解析 API
 * ------------------------------------------------------------------ */

int mjson_has(const char *json, const char *key)
{
    return find_value(json, key) != NULL;
}

const char *mjson_raw_value(const char *json, const char *key)
{
    return find_value(json, key);
}

int mjson_is_null(const char *json, const char *key)
{
    const char *v = find_value(json, key);
    return v && strncmp(v, "null", 4) == 0;
}

int mjson_get_int(const char *json, const char *key, long *out)
{
    const char *v = find_value(json, key);
    char *end = NULL;
    long n;

    if (!v) {
        return 0;
    }
    if (*v == '"') {
        /* 容忍字符串形式的数字: "123" */
        char tmp[32];
        const char *r = read_string(v, tmp, sizeof(tmp));
        if (!r) {
            return 0;
        }
        n = strtol(tmp, &end, 10);
        if (end == tmp) {
            return 0;
        }
    } else {
        n = strtol(v, &end, 10);
        if (end == v) {
            return 0;
        }
    }
    if (out) {
        *out = n;
    }
    return 1;
}

int mjson_get_bool(const char *json, const char *key, int *out)
{
    const char *v = find_value(json, key);
    if (!v) {
        return 0;
    }
    if (strncmp(v, "true", 4) == 0) {
        if (out) *out = 1;
        return 1;
    }
    if (strncmp(v, "false", 5) == 0) {
        if (out) *out = 0;
        return 1;
    }
    /* 容忍 0/1 */
    if (*v == '0' || *v == '1') {
        if (out) *out = (*v == '1');
        return 1;
    }
    return 0;
}

int mjson_get_str(const char *json, const char *key, char *out, size_t out_size)
{
    const char *v = find_value(json, key);
    if (!v) {
        return 0;
    }
    if (strncmp(v, "null", 4) == 0) {
        if (out && out_size) {
            out[0] = '\0';
        }
        return 1;
    }
    if (*v != '"') {
        return -1;
    }
    if (!read_string(v, out, out_size)) {
        return -1;
    }
    return 1;
}

int mjson_get_int_array(const char *json, const char *key, long *out, int max_n)
{
    const char *v = find_value(json, key);
    int n = 0;

    if (!v) {
        return -1;
    }
    v = skip_ws(v);
    if (*v != '[') {
        return -1;
    }
    v++;

    for (;;) {
        v = skip_ws(v);
        if (*v == ']') {
            break;
        }
        if (*v == '\0') {
            return -1;
        }
        {
            char *end = NULL;
            long num = strtol(v, &end, 10);
            if (end == v) {
                return -1;
            }
            if (n < max_n && out) {
                out[n] = num;
            }
            n++;
            v = end;
        }
        v = skip_ws(v);
        if (*v == ',') {
            v++;
            continue;
        }
        if (*v == ']') {
            break;
        }
        return -1;
    }

    return n;
}
