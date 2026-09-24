/**
 * monitor_core.h - DDC/CI 显示器控制核心库 (Windows)
 *
 * 对应原 Python 项目的 vcp.py::PhyMonitor
 * 内部通过 Dxva2.dll 的 Low-Level Monitor Configuration API 收发 VCP 指令。
 *
 * 设计约定:
 *   - 所有 getter/setter 在失败时返回 0 (false)，不抛异常，与 Python 版行为一致。
 *   - 需要返回字符串的接口使用调用方提供的缓冲区，避免内存所有权问题。
 *   - 本库不做任何日志输出到 stdout，以免污染 JSON-RPC 通道；错误通过 stderr 或返回值反馈。
 */
#ifndef MONITOR_CORE_H
#define MONITOR_CORE_H

#include <stdint.h>
#include <stddef.h>

#include "vcp_code.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 显示器型号字符串最大长度 (含结尾 '\0') */
#define MON_MODEL_MAX      128
/** 通用短字符串缓冲区长度 */
#define MON_STR_MAX        256
/** 显示器描述最大长度 */
#define MON_DESC_MAX       128
/** 枚举表 JSON 输出时单行最大长度 */
#define MON_ENUM_JSON_MAX  2048
/** capabilities string 缓冲长度 (VESA 未规定上限, 4096 足够实际使用) */
#define MON_CAPS_MAX       4096

/** 返回值 / 错误码 */
typedef enum {
    MON_OK              = 0,
    MON_ERR_ARG         = -1,   /* 参数非法 */
    MON_ERR_NO_MONITOR  = -2,   /* 没有可用显示器 */
    MON_ERR_OPEN        = -3,   /* 打开显示器失败 */
    MON_ERR_CAPS        = -4,   /* 读取 capabilities string 失败 */
    MON_ERR_VCP         = -5,   /* 发送/读取 VCP 失败 */
    MON_ERR_RANGE       = -6,   /* 值超出范围 */
    MON_ERR_UNSUPPORTED = -7,   /* 不支持的功能 */
    MON_ERR_NOMEM       = -8,   /* 内存不足 */
} MonResult;

/**
 * 物理显示器句柄 (对应 Python 里的 _PhysicalMonitorStructure)。
 *
 * 由 mon_enumerate() 返回时只有 handle / description 有效；
 * mon_open() 之后 caps / model / display_type 才会被填充。
 */
typedef struct {
    void    *handle;                    /* HANDLE hPhysicalMonitor */
    char     description[MON_DESC_MAX]; /* szPhysicalMonitorDescription */

    /* ---- 以下字段由 mon_open() 填充 ---- */
    char    *caps;                      /* capabilities string (堆分配, mon_close 释放) */
    char     model[MON_MODEL_MAX];      /* caps 中的 model(...) */
    char     display_type[MON_STR_MAX]; /* caps 中的 type(...) */
} MonHandle;

/** 枚举结果列表 */
typedef struct {
    MonHandle *items;
    size_t     count;
} MonList;

/* ------------------------------------------------------------------ *
 *  全局生命周期
 * ------------------------------------------------------------------ */

/** 初始化全局状态 (Dxva2 可用性检查)。返回 MON_OK / 错误码 */
int  mon_global_init(void);

/** 释放全局状态 */
void mon_global_shutdown(void);

/* ------------------------------------------------------------------ *
 *  枚举 / 打开 / 关闭
 * ------------------------------------------------------------------ */

/**
 * 枚举系统中所有物理显示器。成功后 *list 由本库分配，需调用 mon_list_free() 释放。
 * 注意: 返回的是裸句柄，尚未建立 DDC/CI 通道。
 */
int  mon_enumerate(MonList *list);

/** 释放 mon_enumerate() 分配的列表 */
void mon_list_free(MonList *list);

/**
 * 打开第 index 个显示器，建立 DDC/CI 通道并读取 capabilities string。
 * 成功后可用 mon_model() 取型号。
 */
int  mon_open(MonHandle *out, const MonList *list, size_t index);

/** 关闭显示器，调用 DestroyPhysicalMonitor 释放句柄 */
void mon_close(MonHandle *m);

/* ------------------------------------------------------------------ *
 *  显示器信息
 * ------------------------------------------------------------------ */

/** 型号，来自 caps string 的 model(...) */
int mon_model(const MonHandle *m, char *buf, size_t buf_size);

/** 显示类型，来自 caps string 的 type(...) */
int mon_info_display_type(const MonHandle *m, char *buf, size_t buf_size);

/** 原始 capabilities string */
int mon_caps_string(const MonHandle *m, char *buf, size_t buf_size);

/** 开机小时数 (0xC0 Display Usage Time) */
int mon_info_poweron_hours(const MonHandle *m, uint32_t *out);

/** 面板子像素排列描述 (0xB2 Flat Panel Sub-Pixel Layout) */
int mon_info_panel_type(const MonHandle *m, char *buf, size_t buf_size);

/* ------------------------------------------------------------------ *
 *  底层 VCP 收发
 * ------------------------------------------------------------------ */

/** 发送 VCP 指令 */
int mon_send_vcp(const MonHandle *m, uint8_t code, uint32_t value);

/** 读取 VCP 当前值与最大值 */
int mon_read_vcp(const MonHandle *m, uint8_t code, uint32_t *current, uint32_t *maximum);

/** 按功能名发送 */
int mon_send_vcp_by_name(const MonHandle *m, const char *name, uint32_t value);

/** 按功能名读取 */
int mon_read_vcp_by_name(const MonHandle *m, const char *name,
                         uint32_t *current, uint32_t *maximum);

/* ------------------------------------------------------------------ *
 *  亮度 / 对比度
 * ------------------------------------------------------------------ */

int mon_brightness_max(const MonHandle *m, uint32_t *out);
int mon_brightness_get(const MonHandle *m, uint32_t *out);
int mon_brightness_set(const MonHandle *m, uint32_t value);

int mon_contrast_max(const MonHandle *m, uint32_t *out);
int mon_contrast_get(const MonHandle *m, uint32_t *out);
int mon_contrast_set(const MonHandle *m, uint32_t value);

/* ------------------------------------------------------------------ *
 *  色温 (K)
 * ------------------------------------------------------------------ */

int mon_color_temperature_get(const MonHandle *m, uint32_t *out);
int mon_color_temperature_set(const MonHandle *m, uint32_t kelvin);

/* ------------------------------------------------------------------ *
 *  颜色预设 (0x14)
 * ------------------------------------------------------------------ */

/** 取当前预设名，写入 buf */
int mon_color_preset_get(const MonHandle *m, char *buf, size_t buf_size);
/** 按预设名设置 */
int mon_color_preset_set(const MonHandle *m, const char *preset);

/* ------------------------------------------------------------------ *
 *  RGB 增益 (0x16 / 0x18 / 0x1A)
 * ------------------------------------------------------------------ */

int mon_rgb_gain_max(const MonHandle *m, uint32_t *out);
int mon_rgb_gain_get(const MonHandle *m, uint32_t *r, uint32_t *g, uint32_t *b);
int mon_rgb_gain_set(const MonHandle *m, uint32_t r, uint32_t g, uint32_t b);

/* ------------------------------------------------------------------ *
 *  OSD 语言 (0xCC)
 * ------------------------------------------------------------------ */

int mon_osd_language_get(const MonHandle *m, char *buf, size_t buf_size);
int mon_osd_language_set(const MonHandle *m, const char *language);

/* ------------------------------------------------------------------ *
 *  电源状态 (0xD6)
 * ------------------------------------------------------------------ */

int mon_power_mode_get(const MonHandle *m, char *buf, size_t buf_size);
int mon_power_mode_set(const MonHandle *m, const char *mode);

/* ------------------------------------------------------------------ *
 *  输入源 (0x60)
 * ------------------------------------------------------------------ */

int mon_input_src_get(const MonHandle *m, char *buf, size_t buf_size);
int mon_input_src_set(const MonHandle *m, const char *src);

/* ------------------------------------------------------------------ *
 *  动作
 * ------------------------------------------------------------------ */

/** 恢复出厂设置 (0x04 = 1) */
int mon_reset_factory(const MonHandle *m);

/** VGA 自动调整 (0x1E = Manual Perform) */
int mon_auto_setup_perform(const MonHandle *m);

/* ------------------------------------------------------------------ *
 *  枚举表导出 (供前端生成下拉框)
 * ------------------------------------------------------------------ */

/** 把枚举表序列化成 JSON 数组字符串: ["a","b",...] */
int mon_enum_table_to_json(const VcpEnumTable *table, char *buf, size_t buf_size);

/**
 * 只保留显示器 caps string 里声明支持的那些枚举值, 再序列化成 JSON 数组。
 *
 * 为什么需要这个:
 *   本项目的枚举表来自 VESA MCCS 规范 (全集), 而实际显示器只实现其中一小部分。
 *   例如本机只接了 DisplayPort 2, caps 里是 vcp(60(0F 11)), 但码表有 18 个输入源。
 *   不过滤的话前端会列出 HDMI 1 / Tuner / Composite 等根本不存在或未接入的选项,
 *   用户一选就必然报错。
 *
 * 解析规则 (对应 caps 里 `vcp(...)` 段落中的条目, 例如 `60(01 03 0F 11)`):
 *   - 枚举值以**两位大写十六进制**列出
 *   - 若该 VCP 在 caps 中**没有**括号, 表示显示器支持这个 VCP 码但不限定取值 → 返回全集
 *   - 若 caps 里找不到该 VCP 码 → 返回空数组 (显示器不支持)
 *
 * @param m       已打开的显示器 (用它的 caps)
 * @param code    目标 VCP 码, 如 0x60
 * @param table   候选枚举表
 */
int mon_enum_table_to_json_filtered(const MonHandle *m, uint8_t code,
                                    const VcpEnumTable *table,
                                    char *buf, size_t buf_size);

/** 便捷函数: 取各枚举表的 JSON 形式 (已按 caps 过滤) */
int mon_color_preset_list_json(const MonHandle *m, char *buf, size_t buf_size);
int mon_osd_languages_list_json(const MonHandle *m, char *buf, size_t buf_size);
int mon_power_mode_list_json(const MonHandle *m, char *buf, size_t buf_size);
int mon_input_src_list_json(const MonHandle *m, char *buf, size_t buf_size);

/** 把错误码转成可读字符串 */
const char *mon_strerror(int result);

#ifdef __cplusplus
}
#endif

#endif /* MONITOR_CORE_H */
