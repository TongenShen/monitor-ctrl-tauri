/**
 * vcp_code.h - VESA MCCS VCP 指令码表
 *
 * 移植自原项目 monitor_ctrl/vcp_code.py
 * 仅做数据表定义与查表，不含任何平台相关逻辑。
 */
#ifndef VCP_CODE_H
#define VCP_CODE_H

#include <stdint.h>
#include <stddef.h>

/* ------------------------------------------------------------------ *
 *  表项定义
 * ------------------------------------------------------------------ */

/** 功能名 -> VCP 码 */
typedef struct {
    const char *name;
    uint8_t     code;
} VcpCodeEntry;

/** 枚举名 <-> 枚举值 (双向查表) */
typedef struct {
    const char *name;
    uint8_t     value;
} VcpEnumEntry;

/** 带长度的枚举表，避免在头文件里暴露数组尺寸 */
typedef struct {
    const VcpEnumEntry *entries;
    size_t              count;
} VcpEnumTable;

typedef struct {
    const VcpCodeEntry *entries;
    size_t              count;
} VcpCodeTable;

/* ------------------------------------------------------------------ *
 *  全局表
 * ------------------------------------------------------------------ */

/** VCP 功能码总表 (对应 py 里的 VCP_CODE 字典) */
extern const VcpCodeTable VCP_CODES;

/** 0x14 Select Color Preset */
extern const VcpEnumTable COLOR_PRESET;

/** 0x1E Auto Setup */
extern const VcpEnumTable AUTO_SETUP;

/** 0xD6 Power Mode */
extern const VcpEnumTable POWER_MODE;

/** 0xCC OSD Language */
extern const VcpEnumTable OSD_LANG;

/** 0x60 Input Source */
extern const VcpEnumTable INPUT_SRC;

/** 0xB2 Flat Panel Sub-Pixel Layout (只用于 value -> 描述) */
extern const VcpEnumTable FLAT_PANEL_SUB_PIXEL_LAYOUT;

/* ------------------------------------------------------------------ *
 *  查表函数
 * ------------------------------------------------------------------ */

/** 按名字取 VCP 码，找不到返回 -1 */
int vcp_code_by_name(const char *name);

/** 按 VCP 码取名字，找不到返回 NULL */
const char *vcp_code_name(uint8_t code);

/** 按枚举名取值，找不到返回 -1 */
int vcp_enum_value(const VcpEnumTable *t, const char *name);

/** 按枚举值取名字，找不到返回 NULL */
const char *vcp_enum_name(const VcpEnumTable *t, uint8_t value);

#endif /* VCP_CODE_H */
