/**
 * vcp_code.c - VCP 码表数据与查表实现
 *
 * 数据完整移植自 monitor_ctrl/vcp_code.py
 */
#include "vcp_code.h"
#include <string.h>

/* ------------------------------------------------------------------ *
 *  VCP 功能码总表  (原 VCP_CODE 字典)
 * ------------------------------------------------------------------ */
static const VcpCodeEntry vcp_code_entries[] = {
    /* ---- Preset Operation ---- */
    { "Restore Factory Color Defaults",                  0x08 },
    { "Restore Factory Defaults",                        0x04 },
    { "Restore Factory Geometry Defaults",               0x06 },
    { "Restore Factory Luminance / Contrast Defaults",   0x05 },
    { "Restore Factory TV Defaults",                     0x0A },
    { "Save / Restore Settings",                         0xB0 },
    { "VCP Code Page",                                   0x00 },

    /* ---- Image Adjustment ---- */
    { "User Color Temperature Increment",                0x0B },
    { "User Color Temperature",                          0x0C },
    { "Luminance",                                       0x10 },
    { "Clock",                                           0x0E },
    { "Flesh Tone Enhancement",                          0x11 },
    { "Contrast",                                        0x12 },
    { "Select Color Preset",                             0x14 },
    { "Video Gain Red",                                  0x16 },
    { "Video Gain Green",                                0x18 },
    { "Video Gain Blue",                                 0x1A },
    { "Auto Setup",                                      0x1E },
    { "Auto Color Setup",                                0x1F },
    { "Gray Scale Expansion",                            0x2E },
    { "Video Black Level: Red",                          0x6C },
    { "Video Black Level: Green",                        0x6E },
    { "Video Black Level: Blue",                         0x70 },
    { "Gamma",                                           0x72 },
    { "Adjust Zoom",                                     0x7C },
    { "Sharpness",                                       0x87 },

    /* ---- Display Control ---- */
    { "Display Usage Time",                              0xC0 },
    { "Display Controller ID",                           0xC8 },
    { "Display Firmware Level",                          0xC9 },
    { "OSD Language",                                    0xCC },
    { "Power Mode",                                      0xD6 },
    { "VCP  Version",                                    0xDF },

    /* ---- Geometry ---- */
    { "Bottom Corner Flare",                             0x4A },
    { "Bottom Corner Hook",                              0x4C },
    { "Display Scaling",                                 0x86 },
    { "Horizontal Convergence M / G",                    0x29 },
    { "Horizontal Convergence R / B",                    0x28 },
    { "Horizontal Keystone",                             0x42 },
    { "Horizontal linearity",                            0x2A },
    { "Horizontal Linearity Balance",                    0x2C },
    { "Horizontal Mirror (Flip)",                        0x82 },
    { "Horizontal Parallelogram",                        0x40 },
    { "Horizontal Pincushion",                           0x24 },
    { "Horizontal Pincushion Balance",                   0x26 },
    { "Horizontal Position (Phase)",                     0x20 },
    { "Horizontal Size",                                 0x22 },
    { "Rotation",                                        0x44 },
    { "Scan Mode",                                       0xDA },
    { "Top Corner Flare",                                0x46 },
    { "Top Corner Hook",                                 0x48 },
    { "Vertical Convergence M / G",                      0x39 },
    { "Vertical Convergence R / B",                      0x38 },
    { "Vertical Keystone",                               0x43 },
    { "Vertical Linearity",                              0x3A },
    { "Vertical Linearity Balance",                      0x3C },
    { "Vertical Mirror (Flip)",                          0x84 },
    { "Vertical Parallelogram",                          0x41 },
    { "Vertical Pincushion",                             0x34 },
    { "Vertical Pincushion Balance",                     0x36 },
    { "Vertical Position (Phase)",                       0x30 },
    { "Vertical Size",                                   0x32 },
    { "Window Position (BR_X)",                          0x97 },
    { "Window Position (BR_Y)",                          0x98 },
    { "Window Position (TL_X)",                          0x95 },
    { "Window Position (TL_Y)",                          0x96 },

    /* ---- Miscellaneous ---- */
    { "Active Control",                                  0x52 },
    { "Ambient Light Sensor",                            0x66 },
    { "Application Enable Key",                          0xC6 },
    { "Asset Tag",                                       0xD2 },
    { "Auxiliary Display Data ",                         0xCF },
    { "Auxiliary Display Size",                          0xCE },
    { "Auxiliary Power Output",                          0xD7 },
    { "Degauss",                                         0x01 },
    { "Display Descriptor Length",                       0xC2 },
    { "Display Identification Data Operation",           0x78 },
    { "Display Technology Type",                         0xB6 },
    /* 原名含弯引号 (U+2018 / U+2019), 拆成相邻字面量避免 hex 转义吞掉后续字符 */
    { "Enable Display of \xe2\x80\x98" "Display Descriptor" "\xe2\x80\x99", 0xC4 },
    { "Flat Panel Sub-Pixel Layout",                     0xB2 },
    { "Input Source",                                    0x60 },
    { "New Control Value",                               0x02 },
    { "Output Select",                                   0xD0 },
    { "Performance Preservation",                        0x54 },
    { "Remote Procedure Call",                           0x76 },
    { "Scratch Pad",                                     0xDE },
    { "Soft Controls",                                   0x03 },
    { "Status Indicators (Host)",                        0xCD },
    { "Transmit Display Descriptor",                     0xC3 },
    { "TV-Channel Up / Down",                            0x8B },

    /* ---- Audio ---- */
    { "Audio: Balance L/R",                              0x93 },
    { "Audio: Bass",                                     0x91 },
    { "Audio: Jack Connection Status",                   0x65 },
    { "Audio: Microphone Volume",                        0x64 },
    { "Audio: Mute (screen blank)",                      0x8D },
    { "Audio: Processor Mode",                           0x94 },
    { "Audio: Speaker Select",                           0x63 },
    { "Audio: Speaker Volume",                           0x62 },
    { "Audio: Treble",                                   0x8F },
};

/* ------------------------------------------------------------------ *
 *  枚举表
 * ------------------------------------------------------------------ */

/* 0x14 Select Color Preset */
static const VcpEnumEntry color_preset_entries[] = {
    { "sRGB",           0x01 },
    { "Display Native", 0x02 },
    { "4000K",          0x03 },
    { "5000K",          0x04 },
    { "6500K",          0x05 },
    { "7500K",          0x06 },
    { "8200K",          0x07 },
    { "9300K",          0x08 },
    { "10000K",         0x09 },
    { "11500K",         0x0A },
    { "User Mode 1",    0x0B },
    { "User Mode 2",    0x0C },
    { "User Mode 3",    0x0D },
};

/* 0x1E Auto Setup */
static const VcpEnumEntry auto_setup_entries[] = {
    { "off",             0x00 },
    { "Manual Perform",  0x01 },
    { "Continuous",      0x02 },
};

/* 0xD6 Power Mode */
static const VcpEnumEntry power_mode_entries[] = {
    { "on",  0x01 },
    { "off", 0x05 },
};

/* 0xCC OSD Language */
static const VcpEnumEntry osd_lang_entries[] = {
    { "Reserved/ignored",     0x00 },
    { "Chinese-traditional",  0x01 },
    { "English",              0x02 },
    { "French",               0x03 },
    { "German",               0x04 },
    { "Italian",              0x05 },
    { "Japanese",             0x06 },
    { "Korean",               0x07 },
    { "Portuguese-Portugal",  0x08 },
    { "Russian",              0x09 },
    { "Spanish",              0x0A },
    { "Swedish",              0x0B },
    { "Turkish",              0x0C },
    { "Chinese-simplified",   0x0D },
    { "Portuguese-Brazil",    0x0E },
    { "Arabic",               0x0F },
    { "Bulgarian",            0x10 },
    { "Croatian",             0x11 },
    { "Czech",                0x12 },
    { "Danish",               0x13 },
    { "Dutch",                0x14 },
    { "Estonian",             0x15 },
    { "Finnish",              0x16 },
    { "Greek",                0x17 },
    { "Hebrew",               0x18 },
    { "Hindi",                0x19 },
    { "Hungarian",            0x1A },
    { "Latvian",              0x1B },
    { "Lithuanian",           0x1C },
    { "Norwegian",            0x1D },
    { "Polish",               0x1E },
    { "Romanian",             0x1F },
    { "Serbian",              0x20 },
    { "Slovak",               0x21 },
    { "Slovenian",            0x22 },
    { "Thai",                 0x23 },
    { "Ukrainian",            0x24 },
    { "Vietnamese",           0x25 },
};

/* 0x60 Input Source */
static const VcpEnumEntry input_src_entries[] = {
    { "Analog video (R/G/B) 1",                    0x01 },
    { "Analog video (R/G/B) 2",                    0x02 },
    { "Digital video (TMDS) 1 DVI 1",              0x03 },
    { "Digital video (TMDS) 2 DVI 2",              0x04 },
    { "Composite video 1",                         0x05 },
    { "Composite video 2",                         0x06 },
    { "S-video 1",                                 0x07 },
    { "S-video 2",                                 0x08 },
    { "Tuner 1",                                   0x09 },
    { "Tuner 2",                                   0x0A },
    { "Tuner 3",                                   0x0B },
    { "Component video (YPbPr / YCbCr) 1",         0x0C },
    { "Component video (YPbPr / YCbCr) 2",         0x0D },
    { "Component video (YPbPr / YCbCr) 3",         0x0E },
    { "DisplayPort 1",                             0x0F },
    { "DisplayPort 2",                             0x10 },
    { "Digital Video (TMDS) 3 HDMI 1",             0x11 },
    { "Digital Video (TMDS) 4 HDMI 2",             0x12 },
};

/* 0xB2 Flat Panel Sub-Pixel Layout */
static const VcpEnumEntry sub_pixel_entries[] = {
    { "Sub-pixel layout is not defined", 0x00 },
    { "Red / Green / Blue vertical stripe", 0x01 },
    { "Red / Green / Blue horizontal stripe", 0x02 },
    { "Blue / Green / Red vertical stripe", 0x03 },
    { "Blue/ Green / Red horizontal stripe", 0x04 },
    { "Quad-pixel, a 2 x 2 sub-pixel structure with red at top left, blue at bottom right and green at top right and bottom left", 0x05 },
    { "Quad-pixel, a 2 x 2 sub-pixel structure with red at bottom left, blue at top right and green at top left and bottom right", 0x06 },
    { "Delta (triad)", 0x07 },
    { "Mosaic with interleaved sub-pixels of different colors", 0x08 },
};

/* ------------------------------------------------------------------ *
 *  表实例
 * ------------------------------------------------------------------ */

#define TABLE_OF(arr) { arr, sizeof(arr) / sizeof((arr)[0]) }

const VcpCodeTable VCP_CODES = TABLE_OF(vcp_code_entries);

const VcpEnumTable COLOR_PRESET              = TABLE_OF(color_preset_entries);
const VcpEnumTable AUTO_SETUP                = TABLE_OF(auto_setup_entries);
const VcpEnumTable POWER_MODE                = TABLE_OF(power_mode_entries);
const VcpEnumTable OSD_LANG                  = TABLE_OF(osd_lang_entries);
const VcpEnumTable INPUT_SRC                 = TABLE_OF(input_src_entries);
const VcpEnumTable FLAT_PANEL_SUB_PIXEL_LAYOUT = TABLE_OF(sub_pixel_entries);

/* ------------------------------------------------------------------ *
 *  查表实现
 * ------------------------------------------------------------------ */

int vcp_code_by_name(const char *name)
{
    size_t i;
    if (!name) {
        return -1;
    }
    for (i = 0; i < VCP_CODES.count; i++) {
        if (strcmp(VCP_CODES.entries[i].name, name) == 0) {
            return VCP_CODES.entries[i].code;
        }
    }
    return -1;
}

const char *vcp_code_name(uint8_t code)
{
    size_t i;
    for (i = 0; i < VCP_CODES.count; i++) {
        if (VCP_CODES.entries[i].code == code) {
            return VCP_CODES.entries[i].name;
        }
    }
    return NULL;
}

int vcp_enum_value(const VcpEnumTable *t, const char *name)
{
    size_t i;
    if (!t || !name) {
        return -1;
    }
    for (i = 0; i < t->count; i++) {
        if (strcmp(t->entries[i].name, name) == 0) {
            return t->entries[i].value;
        }
    }
    return -1;
}

const char *vcp_enum_name(const VcpEnumTable *t, uint8_t value)
{
    size_t i;
    if (!t) {
        return NULL;
    }
    for (i = 0; i < t->count; i++) {
        if (t->entries[i].value == value) {
            return t->entries[i].name;
        }
    }
    return NULL;
}
