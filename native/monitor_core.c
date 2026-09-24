/**
 * monitor_core.c - DDC/CI 显示器控制核心库实现 (Windows)
 *
 * 依赖: Dxva2.lib (MinGW 下为 libdxva2.a)
 *   - EnumDisplayMonitors            (user32)  枚举 HMONITOR
 *   - GetNumberOfPhysicalMonitorsFromHMONITOR  (Dxva2)
 *   - GetPhysicalMonitorsFromHMONITOR          (Dxva2)
 *   - GetCapabilitiesStringLength              (Dxva2)
 *   - CapabilitiesRequestAndCapabilitiesReply  (Dxva2)
 *   - SetVCPFeature / GetVCPFeatureAndVCPFeatureReply (Dxva2)
 *   - DestroyPhysicalMonitor                   (Dxva2)
 */
#include "monitor_core.h"

#include <windows.h>
#include <physicalmonitorenumerationapi.h>
#include <lowlevelmonitorconfigurationapi.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ *
 *  内部工具
 * ------------------------------------------------------------------ */

/**
 * DDC/CI 是走 I2C 的慢速总线, 校验和错误是常态而非异常。
 * 实测原 Python 版连续读取 caps 的成功率约 7/8, 失败时会直接抛异常。
 * 这里对每次收发做有限重试, 显著提高可用性。
 */
#define MON_RETRY_TIMES 3
#define MON_RETRY_DELAY_MS 15

static void mon_retry_pause(void)
{
    Sleep(MON_RETRY_DELAY_MS);
}

/** 安全拷贝字符串, 返回写入长度 (不含 '\0') */
static size_t copy_str(char *dst, size_t dst_size, const char *src)
{
    size_t n;
    if (!dst || dst_size == 0) {
        return 0;
    }
    if (!src) {
        dst[0] = '\0';
        return 0;
    }
    n = strlen(src);
    if (n >= dst_size) {
        n = dst_size - 1;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
    return n;
}

/**
 * 在 src 中查找 start_ 与其后最近的 end_ 之间的内容，写入 dst。
 * 对应 Python 版 PhyMonitor._get_model_info() 里的 find_()。
 */
static int find_between(const char *src, const char *start_, const char *end_,
                        char *dst, size_t dst_size)
{
    const char *p, *q;
    size_t len;

    if (!src || !start_ || !end_ || !dst || dst_size == 0) {
        return MON_ERR_ARG;
    }
    dst[0] = '\0';

    p = strstr(src, start_);
    if (!p) {
        return MON_ERR_UNSUPPORTED;
    }
    p += strlen(start_);

    q = strstr(p, end_);
    if (!q) {
        return MON_ERR_UNSUPPORTED;
    }

    len = (size_t)(q - p);
    if (len >= dst_size) {
        len = dst_size - 1;
    }
    memcpy(dst, p, len);
    dst[len] = '\0';
    return MON_OK;
}

/* ------------------------------------------------------------------ *
 *  全局生命周期
 * ------------------------------------------------------------------ */

int mon_global_init(void)
{
    /* Dxva2.dll 是 Windows Vista+ 的系统组件, 延迟绑定即可。
       这里只做一次显式探测, 便于尽早报错。 */
    HMODULE dxva2 = LoadLibraryA("dxva2.dll");
    if (!dxva2) {
        return MON_ERR_OPEN;
    }
    /* 保持引用不释放, 后续 API 直接使用 */
    return MON_OK;
}

void mon_global_shutdown(void)
{
    /* 目前无需释放, 保留接口以便未来扩展 */
}

/* ------------------------------------------------------------------ *
 *  枚举
 * ------------------------------------------------------------------ */

typedef struct {
    MonHandle *items;
    size_t     count;
    size_t     capacity;
} MonVec;

static int mon_vec_push(MonVec *v, const MonHandle *item)
{
    if (v->count == v->capacity) {
        size_t new_cap = v->capacity ? v->capacity * 2 : 4;
        MonHandle *p = (MonHandle *)realloc(v->items, new_cap * sizeof(MonHandle));
        if (!p) {
            return MON_ERR_NOMEM;
        }
        v->items = p;
        v->capacity = new_cap;
    }
    v->items[v->count] = *item;
    v->count++;
    return MON_OK;
}

/* EnumDisplayMonitors 回调: 收集 HMONITOR */
typedef struct {
    HMONITOR *items;
    size_t    count;
    size_t    capacity;
} HMonVec;

static BOOL CALLBACK enum_monitor_proc(HMONITOR hmon, HDC hdc, LPRECT rc, LPARAM data)
{
    HMonVec *v = (HMonVec *)data;
    (void)hdc;
    (void)rc;

    if (v->count == v->capacity) {
        size_t new_cap = v->capacity ? v->capacity * 2 : 4;
        HMONITOR *p = (HMONITOR *)realloc(v->items, new_cap * sizeof(HMONITOR));
        if (!p) {
            return FALSE; /* 中止枚举 */
        }
        v->items = p;
        v->capacity = new_cap;
    }
    v->items[v->count++] = hmon;
    return TRUE;
}

/** 取某个 HMONITOR 下的所有物理显示器, 追加到 out */
static int collect_physical_monitors(HMONITOR hmon, MonVec *out)
{
    DWORD count = 0;
    PHYSICAL_MONITOR *arr;
    DWORD i;
    int rc;

    if (!GetNumberOfPhysicalMonitorsFromHMONITOR(hmon, &count)) {
        return MON_ERR_OPEN;
    }
    if (count == 0) {
        return MON_OK;
    }

    arr = (PHYSICAL_MONITOR *)calloc(count, sizeof(PHYSICAL_MONITOR));
    if (!arr) {
        return MON_ERR_NOMEM;
    }

    if (!GetPhysicalMonitorsFromHMONITOR(hmon, count, arr)) {
        free(arr);
        return MON_ERR_OPEN;
    }

    for (i = 0; i < count; i++) {
        MonHandle item;
        memset(&item, 0, sizeof(item));
        item.handle = arr[i].hPhysicalMonitor;
        /* szPhysicalMonitorDescription 是 WCHAR, 转成 UTF-8 存 ASCII 即可 */
        {
            int n = WideCharToMultiByte(CP_UTF8, 0, arr[i].szPhysicalMonitorDescription, -1,
                                        item.description, (int)sizeof(item.description) - 1,
                                        NULL, NULL);
            if (n <= 0) {
                item.description[0] = '\0';
            }
        }
        rc = mon_vec_push(out, &item);
        if (rc != MON_OK) {
            /* 注意: 已 push 的句柄由 mon_list_free 释放; 未 push 的要单独销毁 */
            DestroyPhysicalMonitor(arr[i].hPhysicalMonitor);
            free(arr);
            return rc;
        }
    }

    free(arr);
    return MON_OK;
}

int mon_enumerate(MonList *list)
{
    HMonVec hmons;
    MonVec  vec;
    size_t  i;
    int     rc = MON_OK;

    if (!list) {
        return MON_ERR_ARG;
    }
    list->items = NULL;
    list->count = 0;

    memset(&hmons, 0, sizeof(hmons));
    memset(&vec, 0, sizeof(vec));

    if (!EnumDisplayMonitors(NULL, NULL, enum_monitor_proc, (LPARAM)&hmons)) {
        free(hmons.items);
        return MON_ERR_OPEN;
    }

    for (i = 0; i < hmons.count; i++) {
        rc = collect_physical_monitors(hmons.items[i], &vec);
        if (rc != MON_OK) {
            break;
        }
    }
    free(hmons.items);

    if (rc != MON_OK) {
        /* 清理已收集的句柄 */
        size_t k;
        for (k = 0; k < vec.count; k++) {
            DestroyPhysicalMonitor((HANDLE)vec.items[k].handle);
        }
        free(vec.items);
        return rc;
    }

    list->items = vec.items;
    list->count = vec.count;
    return vec.count ? MON_OK : MON_ERR_NO_MONITOR;
}

void mon_list_free(MonList *list)
{
    if (!list) {
        return;
    }
    free(list->items);
    list->items = NULL;
    list->count = 0;
}

/* ------------------------------------------------------------------ *
 *  打开 / 关闭
 * ------------------------------------------------------------------ */

int mon_open(MonHandle *out, const MonList *list, size_t index)
{
    DWORD caps_len = 0;
    char *caps;
    int attempt;
    int got_len = 0, got_caps = 0;

    if (!out || !list || index >= list->count) {
        return MON_ERR_ARG;
    }

    *out = list->items[index];   /* 拷贝 handle + description */
    out->caps = NULL;
    out->model[0] = '\0';
    out->display_type[0] = '\0';

    /* 读取 capabilities string 长度 (DDC/CI 易失败, 需重试) */
    for (attempt = 0; attempt < MON_RETRY_TIMES; attempt++) {
        if (GetCapabilitiesStringLength((HANDLE)out->handle, &caps_len) && caps_len > 0) {
            got_len = 1;
            break;
        }
        if (attempt + 1 < MON_RETRY_TIMES) {
            mon_retry_pause();
        }
    }
    if (!got_len) {
        return MON_ERR_CAPS;
    }

    caps = (char *)calloc(caps_len + 1, 1);
    if (!caps) {
        return MON_ERR_NOMEM;
    }

    for (attempt = 0; attempt < MON_RETRY_TIMES; attempt++) {
        if (CapabilitiesRequestAndCapabilitiesReply((HANDLE)out->handle, caps, caps_len)) {
            got_caps = 1;
            break;
        }
        if (attempt + 1 < MON_RETRY_TIMES) {
            mon_retry_pause();
        }
    }
    if (!got_caps) {
        free(caps);
        return MON_ERR_CAPS;
    }
    caps[caps_len] = '\0';
    out->caps = caps;

    /* 解析 model(...) 与 type(...) */
    find_between(caps, "model(", ")", out->model, sizeof(out->model));
    find_between(caps, "type(", ")", out->display_type, sizeof(out->display_type));

    return MON_OK;
}

void mon_close(MonHandle *m)
{
    if (!m) {
        return;
    }
    /* 句柄可能为 0 (见 mon_read_vcp 注释), 此时 DestroyPhysicalMonitor(0) 是安全空操作。
       但要避免重复释放 caps。 */
    DestroyPhysicalMonitor((HANDLE)m->handle);
    m->handle = NULL;
    if (m->caps) {
        free(m->caps);
        m->caps = NULL;
    }
}

/* ------------------------------------------------------------------ *
 *  显示器信息
 * ------------------------------------------------------------------ */

int mon_model(const MonHandle *m, char *buf, size_t buf_size)
{
    if (!m || !buf) {
        return MON_ERR_ARG;
    }
    copy_str(buf, buf_size, m->model);
    return MON_OK;
}

int mon_info_display_type(const MonHandle *m, char *buf, size_t buf_size)
{
    if (!m || !buf) {
        return MON_ERR_ARG;
    }
    copy_str(buf, buf_size, m->display_type);
    return MON_OK;
}

int mon_caps_string(const MonHandle *m, char *buf, size_t buf_size)
{
    if (!m || !buf) {
        return MON_ERR_ARG;
    }
    copy_str(buf, buf_size, m->caps);
    return MON_OK;
}

/* ------------------------------------------------------------------ *
 *  底层 VCP 收发
 * ------------------------------------------------------------------ */

int mon_send_vcp(const MonHandle *m, uint8_t code, uint32_t value)
{
    int attempt;

    if (!m) {
        return MON_ERR_ARG;
    }
    /* 注意: 不要检查 m->handle 是否为 NULL。
       部分驱动 (如 "Generic PnP Monitor") 返回的 hPhysicalMonitor 就是 0,
       但 Dxva2 仍能正常完成 DDC/CI 收发 —— 原 Python 版也正是这样工作的。 */
    for (attempt = 0; attempt < MON_RETRY_TIMES; attempt++) {
        if (SetVCPFeature((HANDLE)m->handle, (BYTE)code, (DWORD)value)) {
            return MON_OK;
        }
        if (attempt + 1 < MON_RETRY_TIMES) {
            mon_retry_pause();
        }
    }
    return MON_ERR_VCP;
}

int mon_read_vcp(const MonHandle *m, uint8_t code, uint32_t *current, uint32_t *maximum)
{
    int attempt;
    DWORD cur = 0, max = 0;

    if (!m) {
        return MON_ERR_ARG;
    }
    /* 同上: 允许句柄为 NULL */
    for (attempt = 0; attempt < MON_RETRY_TIMES; attempt++) {
        if (GetVCPFeatureAndVCPFeatureReply((HANDLE)m->handle, (BYTE)code, NULL, &cur, &max)) {
            if (current) {
                *current = (uint32_t)cur;
            }
            if (maximum) {
                *maximum = (uint32_t)max;
            }
            return MON_OK;
        }
        if (attempt + 1 < MON_RETRY_TIMES) {
            mon_retry_pause();
        }
    }
    return MON_ERR_VCP;
}

int mon_send_vcp_by_name(const MonHandle *m, const char *name, uint32_t value)
{
    int code = vcp_code_by_name(name);
    if (code < 0) {
        return MON_ERR_UNSUPPORTED;
    }
    return mon_send_vcp(m, (uint8_t)code, value);
}

int mon_read_vcp_by_name(const MonHandle *m, const char *name,
                         uint32_t *current, uint32_t *maximum)
{
    int code = vcp_code_by_name(name);
    if (code < 0) {
        return MON_ERR_UNSUPPORTED;
    }
    return mon_read_vcp(m, (uint8_t)code, current, maximum);
}

/* ------------------------------------------------------------------ *
 *  亮度 / 对比度
 * ------------------------------------------------------------------ */

int mon_brightness_max(const MonHandle *m, uint32_t *out)
{
    return mon_read_vcp_by_name(m, "Luminance", NULL, out);
}

int mon_brightness_get(const MonHandle *m, uint32_t *out)
{
    return mon_read_vcp_by_name(m, "Luminance", out, NULL);
}

int mon_brightness_set(const MonHandle *m, uint32_t value)
{
    uint32_t max = 0;
    int rc = mon_brightness_max(m, &max);
    if (rc != MON_OK) {
        return rc;
    }
    if (value > max) {
        return MON_ERR_RANGE;
    }
    return mon_send_vcp_by_name(m, "Luminance", value);
}

int mon_contrast_max(const MonHandle *m, uint32_t *out)
{
    return mon_read_vcp_by_name(m, "Contrast", NULL, out);
}

int mon_contrast_get(const MonHandle *m, uint32_t *out)
{
    return mon_read_vcp_by_name(m, "Contrast", out, NULL);
}

int mon_contrast_set(const MonHandle *m, uint32_t value)
{
    uint32_t max = 0;
    int rc = mon_contrast_max(m, &max);
    if (rc != MON_OK) {
        return rc;
    }
    if (value > max) {
        return MON_ERR_RANGE;
    }
    return mon_send_vcp_by_name(m, "Contrast", value);
}

/* ------------------------------------------------------------------ *
 *  色温
 * ------------------------------------------------------------------ */

int mon_color_temperature_get(const MonHandle *m, uint32_t *out)
{
    uint32_t increment = 0, current = 0;
    int rc;

    if (!out) {
        return MON_ERR_ARG;
    }
    rc = mon_read_vcp_by_name(m, "User Color Temperature Increment", &increment, NULL);
    if (rc != MON_OK) {
        return rc;
    }
    rc = mon_read_vcp_by_name(m, "User Color Temperature", &current, NULL);
    if (rc != MON_OK) {
        return rc;
    }
    *out = 3000u + current * increment;
    return MON_OK;
}

int mon_color_temperature_set(const MonHandle *m, uint32_t kelvin)
{
    uint32_t increment = 0;
    uint32_t new_value;
    int rc;

    rc = mon_read_vcp_by_name(m, "User Color Temperature Increment", &increment, NULL);
    if (rc != MON_OK) {
        return rc;
    }
    if (increment == 0 || kelvin < 3000u) {
        return MON_ERR_RANGE;
    }
    new_value = (kelvin - 3000u) / increment;
    return mon_send_vcp_by_name(m, "User Color Temperature", new_value);
}

/* ------------------------------------------------------------------ *
 *  颜色预设
 * ------------------------------------------------------------------ */

int mon_color_preset_get(const MonHandle *m, char *buf, size_t buf_size)
{
    uint32_t v = 0;
    const char *name;
    int rc = mon_read_vcp_by_name(m, "Select Color Preset", &v, NULL);
    if (rc != MON_OK) {
        return rc;
    }
    name = vcp_enum_name(&COLOR_PRESET, (uint8_t)v);
    if (!name) {
        if (buf && buf_size) {
            buf[0] = '\0';
        }
        return MON_OK;   /* 与 Python 版一致: 未知值返回空串而非报错 */
    }
    copy_str(buf, buf_size, name);
    return MON_OK;
}

int mon_color_preset_set(const MonHandle *m, const char *preset)
{
    int v = vcp_enum_value(&COLOR_PRESET, preset);
    if (v < 0) {
        return MON_ERR_UNSUPPORTED;
    }
    return mon_send_vcp_by_name(m, "Select Color Preset", (uint32_t)v);
}

/* ------------------------------------------------------------------ *
 *  RGB 增益
 * ------------------------------------------------------------------ */

int mon_rgb_gain_max(const MonHandle *m, uint32_t *out)
{
    return mon_read_vcp_by_name(m, "Video Gain Red", NULL, out);
}

int mon_rgb_gain_get(const MonHandle *m, uint32_t *r, uint32_t *g, uint32_t *b)
{
    uint32_t rv = 0, gv = 0, bv = 0;
    int rc;

    rc = mon_read_vcp_by_name(m, "Video Gain Red", &rv, NULL);
    if (rc != MON_OK) {
        return rc;
    }
    rc = mon_read_vcp_by_name(m, "Video Gain Green", &gv, NULL);
    if (rc != MON_OK) {
        return rc;
    }
    rc = mon_read_vcp_by_name(m, "Video Gain Blue", &bv, NULL);
    if (rc != MON_OK) {
        return rc;
    }
    if (r) *r = rv;
    if (g) *g = gv;
    if (b) *b = bv;
    return MON_OK;
}

int mon_rgb_gain_set(const MonHandle *m, uint32_t r, uint32_t g, uint32_t b)
{
    uint32_t max = 0;
    int rc = mon_rgb_gain_max(m, &max);
    if (rc != MON_OK) {
        return rc;
    }
    if (r > max || g > max || b > max) {
        return MON_ERR_RANGE;
    }

    rc = mon_send_vcp_by_name(m, "Video Gain Red", r);
    if (rc != MON_OK) {
        return rc;
    }
    rc = mon_send_vcp_by_name(m, "Video Gain Green", g);
    if (rc != MON_OK) {
        return rc;
    }
    return mon_send_vcp_by_name(m, "Video Gain Blue", b);
}

/* ------------------------------------------------------------------ *
 *  OSD 语言
 * ------------------------------------------------------------------ */

int mon_osd_language_get(const MonHandle *m, char *buf, size_t buf_size)
{
    uint32_t v = 0;
    const char *name;
    int rc = mon_read_vcp_by_name(m, "OSD Language", &v, NULL);
    if (rc != MON_OK) {
        return rc;
    }
    name = vcp_enum_name(&OSD_LANG, (uint8_t)v);
    copy_str(buf, buf_size, name ? name : "");
    return MON_OK;
}

int mon_osd_language_set(const MonHandle *m, const char *language)
{
    int v = vcp_enum_value(&OSD_LANG, language);
    if (v < 0) {
        return MON_ERR_UNSUPPORTED;
    }
    return mon_send_vcp_by_name(m, "OSD Language", (uint32_t)v);
}

/* ------------------------------------------------------------------ *
 *  电源状态
 * ------------------------------------------------------------------ */

int mon_power_mode_get(const MonHandle *m, char *buf, size_t buf_size)
{
    uint32_t v = 0;
    const char *name;
    int rc = mon_read_vcp_by_name(m, "Power Mode", &v, NULL);
    if (rc != MON_OK) {
        return rc;
    }
    name = vcp_enum_name(&POWER_MODE, (uint8_t)v);
    /* 与 Python 版一致: 关机时某些显示器回 0x02 等怪值, 统一报 'off' */
    copy_str(buf, buf_size, name ? name : "off");
    return MON_OK;
}

int mon_power_mode_set(const MonHandle *m, const char *mode)
{
    int v = vcp_enum_value(&POWER_MODE, mode);
    if (v < 0) {
        return MON_ERR_UNSUPPORTED;
    }
    return mon_send_vcp_by_name(m, "Power Mode", (uint32_t)v);
}

/* ------------------------------------------------------------------ *
 *  输入源
 * ------------------------------------------------------------------ */

int mon_input_src_get(const MonHandle *m, char *buf, size_t buf_size)
{
    uint32_t v = 0;
    const char *name;
    int rc = mon_read_vcp_by_name(m, "Input Source", &v, NULL);
    if (rc != MON_OK) {
        return rc;
    }
    name = vcp_enum_name(&INPUT_SRC, (uint8_t)v);
    copy_str(buf, buf_size, name ? name : "");
    return MON_OK;
}

int mon_input_src_set(const MonHandle *m, const char *src)
{
    int v = vcp_enum_value(&INPUT_SRC, src);
    if (v < 0) {
        return MON_ERR_UNSUPPORTED;
    }
    return mon_send_vcp_by_name(m, "Input Source", (uint32_t)v);
}

/* ------------------------------------------------------------------ *
 *  动作
 * ------------------------------------------------------------------ */

int mon_reset_factory(const MonHandle *m)
{
    return mon_send_vcp_by_name(m, "Restore Factory Defaults", 1);
}

int mon_auto_setup_perform(const MonHandle *m)
{
    int v = vcp_enum_value(&AUTO_SETUP, "Manual Perform");
    return mon_send_vcp_by_name(m, "Auto Setup", (uint32_t)v);
}

/* ------------------------------------------------------------------ *
 *  显示器信息 (需要 VCP 读取的)
 * ------------------------------------------------------------------ */

int mon_info_poweron_hours(const MonHandle *m, uint32_t *out)
{
    return mon_read_vcp_by_name(m, "Display Usage Time", out, NULL);
}

int mon_info_panel_type(const MonHandle *m, char *buf, size_t buf_size)
{
    uint32_t v = 0;
    const char *name;
    int rc = mon_read_vcp_by_name(m, "Flat Panel Sub-Pixel Layout", &v, NULL);
    if (rc != MON_OK) {
        return rc;
    }
    name = vcp_enum_name(&FLAT_PANEL_SUB_PIXEL_LAYOUT, (uint8_t)v);
    copy_str(buf, buf_size, name ? name : "");
    return MON_OK;
}

/* ------------------------------------------------------------------ *
 *  枚举表导出为 JSON
 * ------------------------------------------------------------------ */

/** 转义 JSON 字符串里的 " 和 \ */
static size_t json_escape_append(char *buf, size_t buf_size, size_t pos, const char *s)
{
    size_t i;
    for (i = 0; s[i] != '\0' && pos + 2 < buf_size; i++) {
        char c = s[i];
        if (c == '"' || c == '\\') {
            buf[pos++] = '\\';
        }
        buf[pos++] = c;
    }
    return pos;
}

int mon_enum_table_to_json(const VcpEnumTable *table, char *buf, size_t buf_size)
{
    size_t pos = 0;
    size_t i;

    if (!table || !buf || buf_size < 3) {
        return MON_ERR_ARG;
    }

    buf[pos++] = '[';
    for (i = 0; i < table->count; i++) {
        if (i > 0) {
            buf[pos++] = ',';
        }
        buf[pos++] = '"';
        pos = json_escape_append(buf, buf_size, pos, table->entries[i].name);
        buf[pos++] = '"';
        if (pos + 2 >= buf_size) {
            break;
        }
    }
    buf[pos++] = ']';
    buf[pos] = '\0';
    return MON_OK;
}

/**
 * 在 caps string 的 vcp(...) 段落里定位某个 VCP 码, 取出它的取值列表。
 *
 * caps 里每个条目形如 "60(01 03 0F 11)" 或裸的 "10" (支持但不限定取值)。
 * 这里的做法: 用 " XX" 前缀匹配避免误命中 (例如找 0x10 时不该匹配到
 * "10(..)" 里的 "10" 以外的东西, 也不该被 "110" 这类子串干扰)。
 */
static int caps_is_hex(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
}

static int caps_find_vcp_values(const char *caps, uint8_t code,
                                char *vals, size_t vals_size, int *has_parens)
{
    char token[3];
    const char *p;
    const char *end;
    int depth = 0;

    *has_parens = 0;
    vals[0] = '\0';
    if (!caps || !vals || vals_size < 2) {
        return 0;
    }

    /* 构造两位大写十六进制 token, 与 caps 的书写风格一致 */
    {
        static const char *hex = "0123456789ABCDEF";
        token[0] = hex[(code >> 4) & 0xF];
        token[1] = hex[code & 0xF];
        token[2] = '\0';
    }

    /* 只在 vcp(...) 段落里找, 避免命中 cmds(...) 或 mccs_ver(...) */
    p = strstr(caps, "vcp(");
    if (!p) {
        return 0;
    }
    p += 4;

    /* 找 vcp(...) 的配对右括号, 把搜索范围限制在这一段内。
     * 注意 vcp 段落内部还有一层括号 (枚举值列表), 所以要数括号深度。 */
    end = p;
    for (; *end; end++) {
        if (*end == '(') {
            depth++;
        } else if (*end == ')') {
            if (depth == 0) {
                break;
            }
            depth--;
        }
    }

    for (;;) {
        const char *hit = strstr(p, token);
        if (!hit || hit >= end) {
            return 0;
        }
        {
            /* 前后字符都不能是十六进制字符, 否则说明 token 只是
             * 某个更长数字/单词的一部分 (例如找 "1A" 时不该命中 "1AB")。
             * 合法的前导分隔符有 ' ', '(' 以及 ')'
             * (枚举表紧跟在前一个 VCP 的右括号后面时没有空格, 例如
             *  "D6(01 04 05)CC(02 0D)" 里的 CC)。 */
            char prev = (hit == caps) ? ' ' : hit[-1];
            char next = hit[2];
            int prev_ok = !caps_is_hex(prev);
            int next_ok = !caps_is_hex(next);

            if (prev_ok && next_ok) {
                if (next == '(') {
                    /* 有括号 → 收集括号内的两位十六进制值 */
                    const char *q = hit + 3;
                    size_t pos = 0;
                    *has_parens = 1;
                    while (*q && *q != ')' && pos + 3 < vals_size) {
                        if (*q == ' ') {
                            q++;
                            continue;
                        }
                        vals[pos++] = *q++;
                    }
                    vals[pos] = '\0';
                    return 1;
                }
                /* 裸条目 → 支持该 VCP 但不限定取值 */
                *has_parens = 0;
                return 1;
            }
        }
        p = hit + 2;
    }
}

int mon_enum_table_to_json_filtered(const MonHandle *m, uint8_t code,
                                    const VcpEnumTable *table,
                                    char *buf, size_t buf_size)
{
    char vals[512];
    int has_parens = 0;
    size_t pos = 0;
    size_t i;
    int emitted = 0;

    if (!table || !buf || buf_size < 3) {
        return MON_ERR_ARG;
    }

    /* 没有显示器 / 没有 caps 时退回全集, 保证前端至少能渲染 */
    if (!m || !m->caps || !caps_find_vcp_values(m->caps, code, vals, sizeof(vals), &has_parens)) {
        buf[0] = '[';
        buf[1] = ']';
        buf[2] = '\0';
        /* 显示器不支持该 VCP 码 → 空列表 (前端会禁用该控件) */
        if (m && m->caps) {
            return MON_OK;
        }
        return mon_enum_table_to_json(table, buf, buf_size);
    }

    buf[pos++] = '[';
    for (i = 0; i < table->count; i++) {
        int supported;
        if (!has_parens) {
            /* 裸条目: 显示器不限定取值, 全集都算支持 */
            supported = 1;
        } else {
            static const char *hex = "0123456789ABCDEF";
            char pair[3];
            const char *q;
            pair[0] = hex[(table->entries[i].value >> 4) & 0xF];
            pair[1] = hex[table->entries[i].value & 0xF];
            pair[2] = '\0';
            /* 必须按 2 字符 token 精确比较 —— 用 strstr 会误命中,
             * 例如 vals="020D" 里找 "20" 会匹配到 "020D" 的第 2 个字符。 */
            supported = 0;
            for (q = vals; q[0] && q[1]; q += 2) {
                if (q[0] == pair[0] && q[1] == pair[1]) {
                    supported = 1;
                    break;
                }
            }
        }
        if (!supported) {
            continue;
        }
        if (emitted > 0) {
            buf[pos++] = ',';
        }
        buf[pos++] = '"';
        pos = json_escape_append(buf, buf_size, pos, table->entries[i].name);
        buf[pos++] = '"';
        emitted++;
        if (pos + 2 >= buf_size) {
            break;
        }
    }
    buf[pos++] = ']';
    buf[pos] = '\0';
    return MON_OK;
}

int mon_color_preset_list_json(const MonHandle *m, char *buf, size_t buf_size)
{
    return mon_enum_table_to_json_filtered(m, 0x14, &COLOR_PRESET, buf, buf_size);
}

int mon_osd_languages_list_json(const MonHandle *m, char *buf, size_t buf_size)
{
    return mon_enum_table_to_json_filtered(m, 0xCC, &OSD_LANG, buf, buf_size);
}

int mon_power_mode_list_json(const MonHandle *m, char *buf, size_t buf_size)
{
    return mon_enum_table_to_json_filtered(m, 0xD6, &POWER_MODE, buf, buf_size);
}

int mon_input_src_list_json(const MonHandle *m, char *buf, size_t buf_size)
{
    return mon_enum_table_to_json_filtered(m, 0x60, &INPUT_SRC, buf, buf_size);
}

/* ------------------------------------------------------------------ *
 *  错误码
 * ------------------------------------------------------------------ */

const char *mon_strerror(int result)
{
    switch (result) {
    case MON_OK:              return "ok";
    case MON_ERR_ARG:         return "invalid argument";
    case MON_ERR_NO_MONITOR:  return "no usable monitor found";
    case MON_ERR_OPEN:        return "failed to open monitor";
    case MON_ERR_CAPS:        return "failed to read capabilities string";
    case MON_ERR_VCP:         return "VCP command failed";
    case MON_ERR_RANGE:       return "value out of range";
    case MON_ERR_UNSUPPORTED: return "unsupported feature or value";
    case MON_ERR_NOMEM:       return "out of memory";
    default:                  return "unknown error";
    }
}
