/**
 * monitor_rpc.c - stdio JSON-RPC 服务
 *
 * 协议: 每行一个 JSON 请求, 每行一个 JSON 响应 (换行分隔的 JSON-RPC)。
 * 选择 stdio 而非 HTTP 的原因: DDC/CI 是慢速 I2C, 必须串行执行,
 * 而 stdin/stdout 天然就是单通道串行队列, 无需额外加锁, 也不占端口。
 *
 * 请求格式:
 *   {"id": 1, "method": "list_monitors"}
 *   {"id": 2, "method": "get_props", "index": 0}
 *   {"id": 3, "method": "set_prop", "index": 0, "prop": "brightness", "value": 60}
 *   {"id": 4, "method": "set_prop", "index": 0, "prop": "rgb_gain", "value": [100,100,80]}
 *
 * 响应格式:
 *   {"id": 1, "ok": true, "result": {...}}
 *   {"id": 3, "ok": false, "error": "value out of range"}
 *
 * 约定: stdout 只输出 JSON, 所有诊断信息走 stderr。
 */
#include "monitor_core.h"
#include "mjson.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#endif

#define RPC_LINE_MAX   8192
#define RPC_OUT_MAX    65536

/* ------------------------------------------------------------------ *
 *  全局状态
 * ------------------------------------------------------------------ */

static MonList   g_list;
static MonHandle *g_open = NULL;     /* 已打开的显示器 */
static size_t    g_open_count = 0;

/** 当前请求行, 供需要读取 value 的处理器使用 */
static char *g_req_line = NULL;

/* ------------------------------------------------------------------ *
 *  输出缓冲
 * ------------------------------------------------------------------ */

static char   g_out[RPC_OUT_MAX];
static size_t g_out_pos = 0;

static void out_reset(void)
{
    g_out_pos = 0;
    g_out[0] = '\0';
}

static void out_raw(const char *s)
{
    size_t n;
    if (!s) {
        return;
    }
    n = strlen(s);
    if (g_out_pos + n >= sizeof(g_out)) {
        n = sizeof(g_out) - 1 - g_out_pos;
    }
    memcpy(g_out + g_out_pos, s, n);
    g_out_pos += n;
    g_out[g_out_pos] = '\0';
}

static void out_fmt(const char *fmt, ...)
{
    va_list ap;
    int n;
    if (g_out_pos >= sizeof(g_out) - 1) {
        return;
    }
    va_start(ap, fmt);
    n = vsnprintf(g_out + g_out_pos, sizeof(g_out) - g_out_pos, fmt, ap);
    va_end(ap);
    if (n > 0) {
        g_out_pos += (size_t)n;
    }
    if (g_out_pos >= sizeof(g_out)) {
        g_out_pos = sizeof(g_out) - 1;
        g_out[g_out_pos] = '\0';
    }
}

/** 输出 JSON 字符串 (带引号与转义) */
static void out_json_str(const char *s)
{
    out_raw("\"");
    if (s) {
        g_out_pos = mjson_escape(g_out, sizeof(g_out), g_out_pos, s);
    }
    out_raw("\"");
}

/** 输出 "key": value 形式的字符串字段 */
static void out_field_str(const char *key, const char *value)
{
    out_fmt("\"%s\":", key);
    out_json_str(value);
}

/** 输出 "key": 数字 字段 */
static void out_field_int(const char *key, long value)
{
    out_fmt("\"%s\":%ld", key, value);
}

static void send_response(long id, int ok)
{
    out_reset();
    out_fmt("{\"id\":%ld,\"ok\":%s", id, ok ? "true" : "false");
}

static void send_ok(long id)
{
    send_response(id, 1);
}

static void send_err(long id, const char *msg)
{
    send_response(id, 0);
    out_raw(",\"error\":");
    out_json_str(msg);
    out_raw("}");
    out_raw("\n");
    fputs(g_out, stdout);
    fflush(stdout);
}

static void send_ok_result_begin(long id)
{
    send_ok(id);
    out_raw(",\"result\":");
}

static void send_ok_result_end(void)
{
    out_raw("}");
    out_raw("\n");
    fputs(g_out, stdout);
    fflush(stdout);
}

/** 直接发送不带 result 的成功响应 */
static void send_ok_bare(long id)
{
    send_ok(id);
    out_raw("}");
    out_raw("\n");
    fputs(g_out, stdout);
    fflush(stdout);
}

static void log_err(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}

/* ------------------------------------------------------------------ *
 *  显示器管理
 * ------------------------------------------------------------------ */

static int monitors_init(void)
{
    size_t i, w = 0;
    int rc;

    rc = mon_enumerate(&g_list);
    if (rc != MON_OK) {
        log_err("enumerate failed: %s", mon_strerror(rc));
        return rc;
    }

    g_open = (MonHandle *)calloc(g_list.count ? g_list.count : 1, sizeof(MonHandle));
    if (!g_open) {
        return MON_ERR_NOMEM;
    }

    /* 直接紧凑写入 g_open, 使 index 与成功打开的显示器一一对应。
       注意: 不能用 handle != NULL 判断是否打开成功 ——
       部分驱动的 hPhysicalMonitor 本来就是 0 (见 monitor_core.c 注释)。 */
    for (i = 0; i < g_list.count; i++) {
        rc = mon_open(&g_open[w], &g_list, i);
        if (rc != MON_OK) {
            /* 与原项目一致: 打不开的显示器跳过, 不致命 */
            log_err("monitor %zu open failed: %s", i, mon_strerror(rc));
            memset(&g_open[w], 0, sizeof(MonHandle));
            continue;
        }
        w++;
    }
    g_open_count = w;

    log_err("monitors: %zu enumerated, %zu opened", g_list.count, g_open_count);
    return g_open_count ? MON_OK : MON_ERR_NO_MONITOR;
}

static void monitors_shutdown(void)
{
    size_t i;
    for (i = 0; i < g_open_count; i++) {
        mon_close(&g_open[i]);
    }
    free(g_open);
    g_open = NULL;
    g_open_count = 0;
    mon_list_free(&g_list);
}

/** 取显示器, 越界返回 NULL */
static const MonHandle *get_monitor(long index)
{
    if (index < 0 || (size_t)index >= g_open_count) {
        return NULL;
    }
    return &g_open[index];
}

/* ------------------------------------------------------------------ *
 *  RPC 方法
 * ------------------------------------------------------------------ */

static void rpc_ping(long id)
{
    send_ok_result_begin(id);
    out_raw("{");
    out_field_str("pong", "1");
    out_raw("}");
    send_ok_result_end();
}

static void rpc_list_monitors(long id)
{
    size_t i;
    send_ok_result_begin(id);
    out_raw("{");
    out_raw("\"monitors\":[");
    for (i = 0; i < g_open_count; i++) {
        if (i > 0) {
            out_raw(",");
        }
        out_raw("{");
        out_field_int("index", (long)i);
        out_raw(",");
        out_field_str("model", g_open[i].model);
        out_raw(",");
        out_field_str("description", g_open[i].description);
        out_raw("}");
    }
    out_raw("]");
    out_raw("}");
    send_ok_result_end();
}

/** 一次性取回所有可读属性 (对应 Python 版一堆 property) */
static void rpc_get_props(long id, long index)
{
    const MonHandle *m = get_monitor(index);
    uint32_t v = 0, vmax = 0, r = 0, g = 0, b = 0;
    char sbuf[MON_STR_MAX];
    char jbuf[MON_ENUM_JSON_MAX];

    if (!m) {
        send_err(id, "invalid monitor index");
        return;
    }

    send_ok_result_begin(id);
    out_raw("{");

    /* 亮度 */
    if (mon_brightness_get(m, &v) == MON_OK) {
        out_field_int("brightness", (long)v);
    } else {
        out_raw("\"brightness\":null");
    }
    out_raw(",");
    if (mon_brightness_max(m, &vmax) == MON_OK) {
        out_field_int("brightness_max", (long)vmax);
    } else {
        out_raw("\"brightness_max\":null");
    }
    out_raw(",");

    /* 对比度 */
    if (mon_contrast_get(m, &v) == MON_OK) {
        out_field_int("contrast", (long)v);
    } else {
        out_raw("\"contrast\":null");
    }
    out_raw(",");
    if (mon_contrast_max(m, &vmax) == MON_OK) {
        out_field_int("contrast_max", (long)vmax);
    } else {
        out_raw("\"contrast_max\":null");
    }
    out_raw(",");

    /* RGB 增益 */
    if (mon_rgb_gain_get(m, &r, &g, &b) == MON_OK) {
        out_fmt("\"rgb_gain\":[%lu,%lu,%lu]", (unsigned long)r, (unsigned long)g, (unsigned long)b);
    } else {
        out_raw("\"rgb_gain\":null");
    }
    out_raw(",");
    if (mon_rgb_gain_max(m, &vmax) == MON_OK) {
        out_field_int("rgb_gain_max", (long)vmax);
    } else {
        out_raw("\"rgb_gain_max\":null");
    }
    out_raw(",");

    /* 色温 */
    if (mon_color_temperature_get(m, &v) == MON_OK) {
        out_field_int("color_temperature", (long)v);
    } else {
        out_raw("\"color_temperature\":null");
    }
    out_raw(",");

    /* 颜色预设 */
    if (mon_color_preset_get(m, sbuf, sizeof(sbuf)) == MON_OK) {
        out_field_str("color_preset", sbuf);
    } else {
        out_raw("\"color_preset\":null");
    }
    out_raw(",");
    if (mon_color_preset_list_json(m, jbuf, sizeof(jbuf)) == MON_OK) {
        out_fmt("\"color_preset_list\":%s", jbuf);
    } else {
        out_raw("\"color_preset_list\":[]");
    }
    out_raw(",");

    /* OSD 语言 */
    if (mon_osd_language_get(m, sbuf, sizeof(sbuf)) == MON_OK) {
        out_field_str("osd_language", sbuf);
    } else {
        out_raw("\"osd_language\":null");
    }
    out_raw(",");
    if (mon_osd_languages_list_json(m, jbuf, sizeof(jbuf)) == MON_OK) {
        out_fmt("\"osd_languages_list\":%s", jbuf);
    } else {
        out_raw("\"osd_languages_list\":[]");
    }
    out_raw(",");

    /* 电源状态 */
    if (mon_power_mode_get(m, sbuf, sizeof(sbuf)) == MON_OK) {
        out_field_str("power_mode", sbuf);
    } else {
        out_raw("\"power_mode\":null");
    }
    out_raw(",");
    if (mon_power_mode_list_json(m, jbuf, sizeof(jbuf)) == MON_OK) {
        out_fmt("\"power_mode_list\":%s", jbuf);
    } else {
        out_raw("\"power_mode_list\":[]");
    }
    out_raw(",");

    /* 输入源 */
    if (mon_input_src_get(m, sbuf, sizeof(sbuf)) == MON_OK) {
        out_field_str("input_src", sbuf);
    } else {
        out_raw("\"input_src\":null");
    }
    out_raw(",");
    if (mon_input_src_list_json(m, jbuf, sizeof(jbuf)) == MON_OK) {
        out_fmt("\"input_src_list\":%s", jbuf);
    } else {
        out_raw("\"input_src_list\":[]");
    }

    out_raw("}");
    send_ok_result_end();
}

/** 显示器静态信息 (型号/面板/开机小时/caps) */
static void rpc_get_info(long id, long index)
{
    const MonHandle *m = get_monitor(index);
    uint32_t hours = 0;
    char sbuf[MON_STR_MAX];
    char caps[MON_CAPS_MAX];

    if (!m) {
        send_err(id, "invalid monitor index");
        return;
    }

    send_ok_result_begin(id);
    out_raw("{");

    out_field_str("model", m->model);
    out_raw(",");
    out_field_str("description", m->description);
    out_raw(",");
    out_field_str("display_type", m->display_type);
    out_raw(",");

    if (mon_info_poweron_hours(m, &hours) == MON_OK) {
        out_field_int("poweron_hours", (long)hours);
    } else {
        out_raw("\"poweron_hours\":null");
    }
    out_raw(",");

    if (mon_info_panel_type(m, sbuf, sizeof(sbuf)) == MON_OK) {
        out_field_str("panel_type", sbuf);
    } else {
        out_raw("\"panel_type\":null");
    }
    out_raw(",");

    if (mon_caps_string(m, caps, sizeof(caps)) == MON_OK) {
        out_field_str("caps", caps);
    } else {
        out_raw("\"caps\":null");
    }

    out_raw("}");
    send_ok_result_end();
}

/** 设置单个属性, 值类型按属性名分派 */
static void rpc_set_prop(long id, long index, const char *prop)
{
    const MonHandle *m = get_monitor(index);
    const char *raw;
    int rc = MON_ERR_ARG;

    if (!m) {
        send_err(id, "invalid monitor index");
        return;
    }
    if (!prop) {
        send_err(id, "missing 'prop'");
        return;
    }

    raw = mjson_raw_value(g_req_line, "value");
    if (!raw) {
        send_err(id, "missing 'value'");
        return;
    }

    /* ---- RGB 增益: 数组 ---- */
    if (strcmp(prop, "rgb_gain") == 0) {
        long rgb[3];
        int n = mjson_get_int_array(g_req_line, "value", rgb, 3);
        if (n < 3) {
            send_err(id, "rgb_gain requires an array of 3 integers");
            return;
        }
        rc = mon_rgb_gain_set(m, (uint32_t)rgb[0], (uint32_t)rgb[1], (uint32_t)rgb[2]);
    }
    /* ---- 字符串类属性 ---- */
    else if (strcmp(prop, "color_preset") == 0 ||
             strcmp(prop, "osd_language") == 0 ||
             strcmp(prop, "power_mode") == 0 ||
             strcmp(prop, "input_src") == 0) {
        char val[MON_STR_MAX];
        if (mjson_get_str(g_req_line, "value", val, sizeof(val)) != 1) {
            send_err(id, "value must be a string");
            return;
        }
        if (strcmp(prop, "color_preset") == 0) {
            rc = mon_color_preset_set(m, val);
        } else if (strcmp(prop, "osd_language") == 0) {
            rc = mon_osd_language_set(m, val);
        } else if (strcmp(prop, "power_mode") == 0) {
            rc = mon_power_mode_set(m, val);
        } else {
            rc = mon_input_src_set(m, val);
        }
    }
    /* ---- 整数类属性 ---- */
    else if (strcmp(prop, "brightness") == 0 ||
             strcmp(prop, "contrast") == 0 ||
             strcmp(prop, "color_temperature") == 0) {
        long val;
        if (!mjson_get_int(g_req_line, "value", &val)) {
            send_err(id, "value must be an integer");
            return;
        }
        if (strcmp(prop, "brightness") == 0) {
            rc = mon_brightness_set(m, (uint32_t)val);
        } else if (strcmp(prop, "contrast") == 0) {
            rc = mon_contrast_set(m, (uint32_t)val);
        } else {
            rc = mon_color_temperature_set(m, (uint32_t)val);
        }
    }
    /* ---- 通用逃生口: 直接用 VCP 功能名发送 ---- */
    else if (strncmp(prop, "vcp:", 4) == 0) {
        long val;
        if (!mjson_get_int(g_req_line, "value", &val)) {
            send_err(id, "value must be an integer");
            return;
        }
        rc = mon_send_vcp_by_name(m, prop + 4, (uint32_t)val);
    } else {
        send_err(id, "unknown property");
        return;
    }

    if (rc != MON_OK) {
        send_err(id, mon_strerror(rc));
        return;
    }

    send_ok_result_begin(id);
    out_raw("{");
    out_field_str("prop", prop);
    out_raw(",");
    out_field_str("status", "ok");
    out_raw("}");
    send_ok_result_end();
}

/** 动作: 恢复出厂 / 自动调整 */
static void rpc_action(long id, long index, const char *action)
{
    const MonHandle *m = get_monitor(index);
    int rc;

    if (!m) {
        send_err(id, "invalid monitor index");
        return;
    }
    if (!action) {
        send_err(id, "missing 'action'");
        return;
    }

    if (strcmp(action, "reset_factory") == 0) {
        rc = mon_reset_factory(m);
    } else if (strcmp(action, "auto_setup") == 0) {
        rc = mon_auto_setup_perform(m);
    } else {
        send_err(id, "unknown action");
        return;
    }

    if (rc != MON_OK) {
        send_err(id, mon_strerror(rc));
        return;
    }

    send_ok_result_begin(id);
    out_raw("{");
    out_field_str("action", action);
    out_raw(",");
    out_field_str("status", "ok");
    out_raw("}");
    send_ok_result_end();
}

/**
 * 导出枚举表, 供前端生成下拉框。
 * 已按该显示器的 caps string 过滤 —— 只返回它真正声明支持的取值,
 * 否则前端会列出 HDMI/Tuner 等本机根本没接的输入源。
 * index 缺省 (或越界) 时退回第一个显示器。
 */
static void rpc_enum_tables(long id, long index)
{
    const MonHandle *m = get_monitor(index);
    char jbuf[MON_ENUM_JSON_MAX];

    if (!m) {
        m = get_monitor(0);
    }

    send_ok_result_begin(id);
    out_raw("{");
    if (mon_color_preset_list_json(m, jbuf, sizeof(jbuf)) == MON_OK) {
        out_fmt("\"color_preset_list\":%s", jbuf);
    }
    out_raw(",");
    if (mon_osd_languages_list_json(m, jbuf, sizeof(jbuf)) == MON_OK) {
        out_fmt("\"osd_languages_list\":%s", jbuf);
    }
    out_raw(",");
    if (mon_power_mode_list_json(m, jbuf, sizeof(jbuf)) == MON_OK) {
        out_fmt("\"power_mode_list\":%s", jbuf);
    }
    out_raw(",");
    if (mon_input_src_list_json(m, jbuf, sizeof(jbuf)) == MON_OK) {
        out_fmt("\"input_src_list\":%s", jbuf);
    }
    out_raw("}");
    send_ok_result_end();
}

/* ------------------------------------------------------------------ *
 *  分发
 * ------------------------------------------------------------------ */

static void dispatch(char *line)
{
    long id = 0;
    char method[MON_STR_MAX];
    char prop[MON_STR_MAX];
    char action[MON_STR_MAX];
    long index = 0;

    mjson_get_int(line, "id", &id);

    if (mjson_get_str(line, "method", method, sizeof(method)) != 1) {
        send_err(id, "missing 'method'");
        return;
    }

    g_req_line = line;

    if (strcmp(method, "ping") == 0) {
        rpc_ping(id);
    } else if (strcmp(method, "list_monitors") == 0) {
        rpc_list_monitors(id);
    } else if (strcmp(method, "enum_tables") == 0) {
        mjson_get_int(line, "index", &index);
        rpc_enum_tables(id, index);
    } else if (strcmp(method, "get_props") == 0) {
        mjson_get_int(line, "index", &index);
        rpc_get_props(id, index);
    } else if (strcmp(method, "get_info") == 0) {
        mjson_get_int(line, "index", &index);
        rpc_get_info(id, index);
    } else if (strcmp(method, "set_prop") == 0) {
        mjson_get_int(line, "index", &index);
        if (mjson_get_str(line, "prop", prop, sizeof(prop)) != 1) {
            send_err(id, "missing 'prop'");
            return;
        }
        rpc_set_prop(id, index, prop);
    } else if (strcmp(method, "action") == 0) {
        mjson_get_int(line, "index", &index);
        if (mjson_get_str(line, "action", action, sizeof(action)) != 1) {
            send_err(id, "missing 'action'");
            return;
        }
        rpc_action(id, index, action);
    } else if (strcmp(method, "shutdown") == 0) {
        send_ok_bare(id);
        exit(0);
    } else {
        send_err(id, "unknown method");
    }
}

/* ------------------------------------------------------------------ *
 *  入口
 * ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    static char line[RPC_LINE_MAX];
    int rc;
    int i;

    (void)argc;
    (void)argv;

    /* Windows 下确保 stdout/stdin 为二进制模式, 避免 \n 被转换 */
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif

    rc = mon_global_init();
    if (rc != MON_OK) {
        log_err("global init failed: %s", mon_strerror(rc));
        return 1;
    }

    rc = monitors_init();
    if (rc != MON_OK) {
        log_err("no usable monitor: %s", mon_strerror(rc));
        /* 仍然启动服务, 让前端能拿到空列表并显示错误, 而不是直接崩溃 */
    }

    /* 主循环: 逐行读取请求并响应 */
    for (;;) {
        if (!fgets(line, sizeof(line), stdin)) {
            break;   /* EOF: 父进程关闭了管道 */
        }

        /* 去掉行尾换行 */
        for (i = (int)strlen(line) - 1; i >= 0; i--) {
            if (line[i] == '\n' || line[i] == '\r') {
                line[i] = '\0';
            } else {
                break;
            }
        }
        if (line[0] == '\0') {
            continue;
        }

        dispatch(line);
    }

    monitors_shutdown();
    mon_global_shutdown();
    return 0;
}
