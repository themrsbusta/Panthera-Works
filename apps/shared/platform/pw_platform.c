#include <dos.h>
#include <i86.h>
#include <string.h>

#include "pw_platform.h"

/* Required by the Sybase Open Watcom Public License for executable distributions. */
const char pw_open_watcom_runtime_notice[] =
    "Open Watcom C/C++32 runtime code is included in this executable. Its Covered Code source is available under the Sybase Open Watcom Public License 1.0 at https://github.com/open-watcom/open-watcom-v2";

typedef unsigned short u16;
typedef unsigned long u32;

#pragma pack(1)
typedef struct {
    char signature[4];
    u16 version;
    u32 oem_string_ptr;
    u32 capabilities;
    u32 video_mode_ptr;
    u16 total_memory;
    u16 oem_software_revision;
    u32 oem_vendor_name_ptr;
    u32 oem_product_name_ptr;
    u32 oem_product_revision_ptr;
    unsigned char reserved[222];
    unsigned char oem_data[256];
} PwVbeInfoBlock;

typedef struct {
    u16 mode_attributes;
    unsigned char win_a_attributes;
    unsigned char win_b_attributes;
    u16 win_granularity;
    u16 win_size;
    u16 win_a_segment;
    u16 win_b_segment;
    u32 win_function_ptr;
    u16 bytes_per_scanline;
    u16 x_resolution;
    u16 y_resolution;
    unsigned char x_char_size;
    unsigned char y_char_size;
    unsigned char number_of_planes;
    unsigned char bits_per_pixel;
    unsigned char number_of_banks;
    unsigned char memory_model;
    unsigned char bank_size;
    unsigned char number_of_image_pages;
    unsigned char reserved1;
    unsigned char red_mask_size;
    unsigned char red_field_position;
    unsigned char green_mask_size;
    unsigned char green_field_position;
    unsigned char blue_mask_size;
    unsigned char blue_field_position;
    unsigned char reserved_mask_size;
    unsigned char reserved_field_position;
    unsigned char direct_color_mode_info;
    u32 physical_base;
    u32 offscreen_memory_offset;
    u16 offscreen_memory_size;
    unsigned char reserved2[206];
} PwVbeModeInfo;

typedef struct {
    pw_platform_u8 blue;
    pw_platform_u8 green;
    pw_platform_u8 red;
    pw_platform_u8 reserved;
} PwVbePaletteEntry;
#pragma pack()

static PwVbeInfoBlock g_vbe;
static PwVbeModeInfo g_mode;
static pw_platform_u8 *g_framebuffer;
static u32 g_framebuffer_linear;
static u32 g_framebuffer_pitch;
static int g_video_error;
static int g_mouse_available;
static PwVbePaletteEntry g_palette_entry;

static unsigned long vbe_get_info(void *buffer);
#pragma aux vbe_get_info = \
    "mov ax,ds" \
    "mov es,ax" \
    "mov ax,4F00h" \
    "int 10h" \
    "movzx eax,ax" \
    parm [edi] value [eax] modify [eax ecx esi edi];

static unsigned long vbe_get_mode_info(unsigned short mode, void *buffer);
#pragma aux vbe_get_mode_info = \
    "mov ax,ds" \
    "mov es,ax" \
    "mov ax,4F01h" \
    "int 10h" \
    "movzx eax,ax" \
    parm [ecx] [edi] value [eax] modify [eax ecx esi edi];

static unsigned long vbe_set_palette_entry(unsigned short index, void *entry);
#pragma aux vbe_set_palette_entry = \
    "mov ax,ds" \
    "mov es,ax" \
    "mov ax,4F09h" \
    "xor ebx,ebx" \
    "mov cx,1" \
    "int 10h" \
    "movzx eax,ax" \
    parm [dx] [edi] value [eax] modify [eax ebx ecx esi edi];

static unsigned long query_bios_font(void);
#pragma aux query_bios_font = \
    "mov ax,1130h" \
    "mov bx,0600h" \
    "int 10h" \
    "movzx eax,bp" \
    "xor edx,edx" \
    "mov dx,es" \
    "shl edx,16" \
    "or eax,edx" \
    value [eax] modify [eax ebx ecx edx esi edi];

static unsigned long bios_call(u16 ax, u16 bx, u16 cx)
{
    union REGS in_regs;
    union REGS out_regs;
    struct SREGS seg_regs;
    memset(&in_regs, 0, sizeof(in_regs));
    memset(&out_regs, 0, sizeof(out_regs));
    segread(&seg_regs);
    in_regs.w.ax = ax;
    in_regs.w.bx = bx;
    in_regs.w.cx = cx;
    int386x(0x10, &in_regs, &out_regs, &seg_regs);
    return out_regs.x.eax & 0xffffUL;
}

static int map_physical(u32 physical, u32 bytes, u32 *linear_out)
{
    union REGS in_regs;
    union REGS out_regs;
    struct SREGS seg_regs;
    memset(&in_regs, 0, sizeof(in_regs));
    memset(&out_regs, 0, sizeof(out_regs));
    segread(&seg_regs);
    in_regs.w.ax = 0x0800;
    in_regs.w.bx = (u16)(physical >> 16);
    in_regs.w.cx = (u16)physical;
    in_regs.w.si = (u16)(bytes >> 16);
    in_regs.w.di = (u16)bytes;
    int386x(0x31, &in_regs, &out_regs, &seg_regs);
    if (out_regs.x.cflag) return 0;
    *linear_out = ((u32)out_regs.w.bx << 16) | out_regs.w.cx;
    return 1;
}

static void unmap_physical(u32 linear)
{
    union REGS in_regs;
    union REGS out_regs;
    struct SREGS seg_regs;
    memset(&in_regs, 0, sizeof(in_regs));
    memset(&out_regs, 0, sizeof(out_regs));
    segread(&seg_regs);
    in_regs.w.ax = 0x0801;
    in_regs.w.bx = (u16)(linear >> 16);
    in_regs.w.cx = (u16)linear;
    int386x(0x31, &in_regs, &out_regs, &seg_regs);
}

static u16 map_real_mode_segment(u16 segment)
{
    union REGS in_regs;
    union REGS out_regs;
    struct SREGS seg_regs;
    memset(&in_regs, 0, sizeof(in_regs));
    memset(&out_regs, 0, sizeof(out_regs));
    segread(&seg_regs);
    in_regs.w.ax = 0x0002;
    in_regs.w.bx = segment;
    int386x(0x31, &in_regs, &out_regs, &seg_regs);
    if (out_regs.x.cflag) return 0;
    return out_regs.w.ax;
}

static void free_selector(u16 selector)
{
    union REGS in_regs;
    union REGS out_regs;
    struct SREGS seg_regs;
    if (selector == 0) return;
    memset(&in_regs, 0, sizeof(in_regs));
    memset(&out_regs, 0, sizeof(out_regs));
    segread(&seg_regs);
    in_regs.w.ax = 0x0001;
    in_regs.w.bx = selector;
    int386x(0x31, &in_regs, &out_regs, &seg_regs);
}

int pw_platform_video_start(void)
{
    u32 visible;
    u32 physical_aligned;
    u32 offset;
    u32 mapped;
    g_video_error = PW_PLATFORM_VIDEO_OK;
    memset(&g_vbe, 0, sizeof(g_vbe));
    memcpy(g_vbe.signature, "VBE2", 4);
    if (vbe_get_info(&g_vbe) != 0x004f || memcmp(g_vbe.signature, "VESA", 4) != 0) {
        g_video_error = PW_PLATFORM_VIDEO_VBE_QUERY;
        return 0;
    }
    memset(&g_mode, 0, sizeof(g_mode));
    if (vbe_get_mode_info(0x0101, &g_mode) != 0x004f ||
        !(g_mode.mode_attributes & 0x0080) || g_mode.physical_base == 0 ||
        g_mode.x_resolution != PW_PLATFORM_SCREEN_WIDTH ||
        g_mode.y_resolution != PW_PLATFORM_SCREEN_HEIGHT || g_mode.bits_per_pixel != 8) {
        g_video_error = PW_PLATFORM_VIDEO_MODE_INFO;
        return 0;
    }
    if (bios_call(0x4f02, 0x4101, 0) != 0x004f) {
        g_video_error = PW_PLATFORM_VIDEO_MODE_SET;
        return 0;
    }
    visible = (u32)g_mode.bytes_per_scanline * PW_PLATFORM_SCREEN_HEIGHT;
    physical_aligned = g_mode.physical_base & ~4095UL;
    offset = g_mode.physical_base - physical_aligned;
    mapped = (offset + visible + 4095UL) & ~4095UL;
    if (!map_physical(physical_aligned, mapped, &g_framebuffer_linear)) {
        bios_call(3, 0, 0);
        g_video_error = PW_PLATFORM_VIDEO_LFB_MAP;
        return 0;
    }
    g_framebuffer = (pw_platform_u8 *)(g_framebuffer_linear + offset);
    g_framebuffer_pitch = g_mode.bytes_per_scanline;
    return 1;
}

int pw_platform_video_set_palette_entry(unsigned index,
                                        unsigned red, unsigned green, unsigned blue)
{
    unsigned long status;
    if (g_framebuffer == NULL || index > 255 || red > 255 || green > 255 || blue > 255)
        return 0;
    /* VBE 4F09h uses B,G,R,reserved entries; VBE initializes the DAC to six bits. */
    g_palette_entry.blue = (pw_platform_u8)(blue >> 2);
    g_palette_entry.green = (pw_platform_u8)(green >> 2);
    g_palette_entry.red = (pw_platform_u8)(red >> 2);
    g_palette_entry.reserved = 0;
    status = vbe_set_palette_entry((u16)index, &g_palette_entry);
    return (status & 0xffffUL) == 0x004fUL;
}

void pw_platform_video_stop(void)
{
    if (g_framebuffer_linear != 0) {
        unmap_physical(g_framebuffer_linear);
        g_framebuffer_linear = 0;
        g_framebuffer = NULL;
    }
    bios_call(3, 0, 0);
}

const char *pw_platform_video_error(void)
{
    switch (g_video_error) {
        case PW_PLATFORM_VIDEO_VBE_QUERY: return "VBE_QUERY_FAIL";
        case PW_PLATFORM_VIDEO_MODE_INFO: return "VBE_MODE_UNAVAILABLE";
        case PW_PLATFORM_VIDEO_MODE_SET: return "VBE_MODE_SET_FAIL";
        case PW_PLATFORM_VIDEO_LFB_MAP: return "DPMI_LFB_MAP_FAIL";
        default: return "VIDEO_INIT_FAIL";
    }
}

void pw_platform_video_present(const pw_platform_u8 *pixels)
{
    u32 row;
    if (g_framebuffer == NULL || pixels == NULL) return;
    for (row = 0; row < PW_PLATFORM_SCREEN_HEIGHT; ++row)
        memcpy(g_framebuffer + row * g_framebuffer_pitch,
               pixels + row * PW_PLATFORM_SCREEN_WIDTH,
               PW_PLATFORM_SCREEN_WIDTH);
}

void pw_platform_canvas_clear(pw_platform_u8 *pixels, pw_platform_u8 color)
{
    if (pixels != NULL) memset(pixels, color,
        PW_PLATFORM_SCREEN_WIDTH * PW_PLATFORM_SCREEN_HEIGHT);
}

void pw_platform_canvas_pixel(pw_platform_u8 *pixels, int x, int y, pw_platform_u8 color)
{
    if (pixels != NULL && x >= 0 && x < PW_PLATFORM_SCREEN_WIDTH &&
        y >= 0 && y < PW_PLATFORM_SCREEN_HEIGHT)
        pixels[(u32)y * PW_PLATFORM_SCREEN_WIDTH + x] = color;
}

void pw_platform_canvas_fill_rect(pw_platform_u8 *pixels, int x, int y,
                                  int width, int height, pw_platform_u8 color)
{
    int row;
    int left;
    int top;
    int right;
    int bottom;
    if (pixels == NULL || width <= 0 || height <= 0) return;
    left = x < 0 ? 0 : x;
    top = y < 0 ? 0 : y;
    right = x + width > PW_PLATFORM_SCREEN_WIDTH ? PW_PLATFORM_SCREEN_WIDTH : x + width;
    bottom = y + height > PW_PLATFORM_SCREEN_HEIGHT ? PW_PLATFORM_SCREEN_HEIGHT : y + height;
    if (left >= right || top >= bottom) return;
    for (row = top; row < bottom; ++row)
        memset(pixels + (u32)row * PW_PLATFORM_SCREEN_WIDTH + left, color, right - left);
}

void pw_platform_canvas_line(pw_platform_u8 *pixels, int x0, int y0, int x1, int y1,
                             pw_platform_u8 color)
{
    int dx = x1 - x0;
    int dy = y1 - y0;
    int sx = dx < 0 ? -1 : 1;
    int sy = dy < 0 ? -1 : 1;
    int ax = dx < 0 ? -dx : dx;
    int ay = dy < 0 ? -dy : dy;
    int error = ax - ay;
    for (;;) {
        int twice;
        pw_platform_canvas_pixel(pixels, x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        twice = error * 2;
        if (twice > -ay) { error -= ay; x0 += sx; }
        if (twice < ax) { error += ax; y0 += sy; }
    }
}

/* Compact, original 2006-era toolbar glyphs shared by the native apps. */
void pw_platform_canvas_toolbar_icon(pw_platform_u8 *pixels, int icon, int x, int y,
                                     pw_platform_u8 ink, pw_platform_u8 accent)
{
    int xx;
    int yy;
    if (pixels == NULL) return;
    if (icon == PW_PLATFORM_ICON_NEW || icon == PW_PLATFORM_ICON_ADD_SLIDE) {
        pw_platform_canvas_fill_rect(pixels, x + 4, y + 1, 11, 16, 15);
        pw_platform_canvas_line(pixels, x + 4, y + 1, x + 14, y + 1, ink);
        pw_platform_canvas_line(pixels, x + 4, y + 1, x + 4, y + 16, ink);
        pw_platform_canvas_line(pixels, x + 14, y + 1, x + 14, y + 4, ink);
        pw_platform_canvas_line(pixels, x + 4, y + 16, x + 14, y + 16, ink);
        pw_platform_canvas_line(pixels, x + 11, y + 1, x + 14, y + 4, ink);
        pw_platform_canvas_line(pixels, x + 11, y + 1, x + 11, y + 4, ink);
        pw_platform_canvas_line(pixels, x + 11, y + 4, x + 14, y + 4, ink);
        pw_platform_canvas_line(pixels, x + 7, y + 9, x + 12, y + 9, accent);
        if (icon == PW_PLATFORM_ICON_ADD_SLIDE) {
            pw_platform_canvas_line(pixels, x + 9, y + 7, x + 9, y + 11, accent);
        }
    } else if (icon == PW_PLATFORM_ICON_OPEN) {
        pw_platform_canvas_fill_rect(pixels, x + 3, y + 6, 15, 10, 15);
        pw_platform_canvas_fill_rect(pixels, x + 4, y + 4, 7, 3, 15);
        pw_platform_canvas_line(pixels, x + 3, y + 6, x + 4, y + 4, ink);
        pw_platform_canvas_line(pixels, x + 4, y + 4, x + 10, y + 4, ink);
        pw_platform_canvas_line(pixels, x + 10, y + 4, x + 12, y + 6, ink);
        pw_platform_canvas_line(pixels, x + 3, y + 6, x + 18, y + 6, ink);
        pw_platform_canvas_line(pixels, x + 3, y + 6, x + 3, y + 15, ink);
        pw_platform_canvas_line(pixels, x + 18, y + 6, x + 18, y + 15, ink);
        pw_platform_canvas_line(pixels, x + 3, y + 15, x + 18, y + 15, ink);
        pw_platform_canvas_line(pixels, x + 4, y + 9, x + 17, y + 9, accent);
    } else if (icon == PW_PLATFORM_ICON_SAVE) {
        pw_platform_canvas_fill_rect(pixels, x + 3, y + 2, 15, 15, accent);
        pw_platform_canvas_line(pixels, x + 3, y + 2, x + 17, y + 2, ink);
        pw_platform_canvas_line(pixels, x + 3, y + 2, x + 3, y + 16, ink);
        pw_platform_canvas_line(pixels, x + 17, y + 2, x + 17, y + 16, ink);
        pw_platform_canvas_line(pixels, x + 3, y + 16, x + 17, y + 16, ink);
        pw_platform_canvas_fill_rect(pixels, x + 6, y + 3, 8, 5, 15);
        pw_platform_canvas_fill_rect(pixels, x + 6, y + 10, 9, 6, 15);
        pw_platform_canvas_line(pixels, x + 6, y + 3, x + 13, y + 3, ink);
        pw_platform_canvas_line(pixels, x + 6, y + 3, x + 6, y + 7, ink);
        pw_platform_canvas_line(pixels, x + 13, y + 3, x + 13, y + 7, ink);
        pw_platform_canvas_line(pixels, x + 6, y + 7, x + 13, y + 7, ink);
        pw_platform_canvas_line(pixels, x + 6, y + 10, x + 14, y + 10, ink);
        pw_platform_canvas_line(pixels, x + 6, y + 10, x + 6, y + 15, ink);
        pw_platform_canvas_line(pixels, x + 14, y + 10, x + 14, y + 15, ink);
        pw_platform_canvas_line(pixels, x + 6, y + 15, x + 14, y + 15, ink);
    } else if (icon == PW_PLATFORM_ICON_UNDO || icon == PW_PLATFORM_ICON_REDO) {
        int left = icon == PW_PLATFORM_ICON_UNDO;
        int start_x = left ? x + 15 : x + 4;
        int end_x = left ? x + 5 : x + 14;
        pw_platform_canvas_line(pixels, start_x, y + 7, end_x, y + 7, ink);
        pw_platform_canvas_line(pixels, end_x, y + 7, end_x + (left ? 4 : -4), y + 3, ink);
        pw_platform_canvas_line(pixels, end_x, y + 7, end_x + (left ? 4 : -4), y + 11, ink);
        pw_platform_canvas_line(pixels, x + 6, y + 7, x + 6, y + 12, accent);
        pw_platform_canvas_line(pixels, x + 6, y + 12, x + 15, y + 12, accent);
    } else if (icon == PW_PLATFORM_ICON_FIND) {
        for (yy = -5; yy <= 5; ++yy) {
            for (xx = -5; xx <= 5; ++xx) {
                int distance = xx * xx + yy * yy;
                if (distance >= 19 && distance <= 27)
                    pw_platform_canvas_pixel(pixels, x + 8 + xx, y + 7 + yy, ink);
            }
        }
        pw_platform_canvas_line(pixels, x + 12, y + 11, x + 17, y + 16, ink);
    } else if (icon == PW_PLATFORM_ICON_REPLACE) {
        pw_platform_canvas_line(pixels, x + 3, y + 5, x + 16, y + 5, accent);
        pw_platform_canvas_line(pixels, x + 16, y + 5, x + 12, y + 2, accent);
        pw_platform_canvas_line(pixels, x + 16, y + 5, x + 12, y + 8, accent);
        pw_platform_canvas_line(pixels, x + 16, y + 13, x + 3, y + 13, ink);
        pw_platform_canvas_line(pixels, x + 3, y + 13, x + 7, y + 10, ink);
        pw_platform_canvas_line(pixels, x + 3, y + 13, x + 7, y + 16, ink);
    } else if (icon == PW_PLATFORM_ICON_DELETE) {
        pw_platform_canvas_line(pixels, x + 5, y + 4, x + 15, y + 4, ink);
        pw_platform_canvas_line(pixels, x + 8, y + 2, x + 12, y + 2, ink);
        pw_platform_canvas_line(pixels, x + 6, y + 5, x + 7, y + 16, ink);
        pw_platform_canvas_line(pixels, x + 14, y + 5, x + 13, y + 16, ink);
        pw_platform_canvas_line(pixels, x + 7, y + 16, x + 13, y + 16, ink);
        pw_platform_canvas_line(pixels, x + 9, y + 7, x + 9, y + 13, accent);
        pw_platform_canvas_line(pixels, x + 11, y + 7, x + 11, y + 13, accent);
    } else if (icon == PW_PLATFORM_ICON_PREVIOUS || icon == PW_PLATFORM_ICON_NEXT) {
        int left = icon == PW_PLATFORM_ICON_PREVIOUS;
        int edge = left ? x + 5 : x + 15;
        int tip = left ? x + 14 : x + 5;
        pw_platform_canvas_line(pixels, edge, y + 3, edge, y + 15, ink);
        pw_platform_canvas_line(pixels, tip, y + 3, tip, y + 15, accent);
        pw_platform_canvas_line(pixels, tip, y + 3, left ? edge : edge, y + 9, accent);
        pw_platform_canvas_line(pixels, tip, y + 15, edge, y + 9, accent);
    } else if (icon == PW_PLATFORM_ICON_PRESENT) {
        pw_platform_canvas_line(pixels, x + 2, y + 2, x + 17, y + 2, ink);
        pw_platform_canvas_line(pixels, x + 2, y + 2, x + 2, y + 12, ink);
        pw_platform_canvas_line(pixels, x + 17, y + 2, x + 17, y + 12, ink);
        pw_platform_canvas_line(pixels, x + 2, y + 12, x + 17, y + 12, ink);
        pw_platform_canvas_line(pixels, x + 8, y + 13, x + 8, y + 16, ink);
        pw_platform_canvas_line(pixels, x + 11, y + 13, x + 11, y + 16, ink);
        pw_platform_canvas_line(pixels, x + 6, y + 16, x + 13, y + 16, ink);
        pw_platform_canvas_line(pixels, x + 7, y + 5, x + 12, y + 7, accent);
        pw_platform_canvas_line(pixels, x + 12, y + 7, x + 7, y + 10, accent);
        pw_platform_canvas_line(pixels, x + 7, y + 10, x + 7, y + 5, accent);
    } else if (icon == PW_PLATFORM_ICON_FUNCTION) {
        pw_platform_canvas_line(pixels, x + 5, y + 4, x + 11, y + 4, ink);
        pw_platform_canvas_line(pixels, x + 7, y + 4, x + 5, y + 15, ink);
        pw_platform_canvas_line(pixels, x + 4, y + 8, x + 9, y + 8, ink);
        pw_platform_canvas_line(pixels, x + 11, y + 9, x + 17, y + 15, accent);
        pw_platform_canvas_line(pixels, x + 17, y + 9, x + 11, y + 15, accent);
    } else if (icon == PW_PLATFORM_ICON_TITLE) {
        pw_platform_canvas_line(pixels, x + 4, y + 3, x + 16, y + 3, ink);
        pw_platform_canvas_line(pixels, x + 4, y + 3, x + 4, y + 16, ink);
        pw_platform_canvas_line(pixels, x + 16, y + 3, x + 16, y + 16, ink);
        pw_platform_canvas_line(pixels, x + 4, y + 16, x + 16, y + 16, ink);
        pw_platform_canvas_line(pixels, x + 8, y + 5, x + 13, y + 5, accent);
        pw_platform_canvas_line(pixels, x + 10, y + 5, x + 10, y + 12, accent);
        pw_platform_canvas_line(pixels, x + 8, y + 12, x + 13, y + 12, accent);
    } else if (icon == PW_PLATFORM_ICON_BODY) {
        pw_platform_canvas_line(pixels, x + 4, y + 3, x + 4, y + 16, ink);
        pw_platform_canvas_line(pixels, x + 7, y + 4, x + 17, y + 4, accent);
        pw_platform_canvas_line(pixels, x + 7, y + 8, x + 15, y + 8, ink);
        pw_platform_canvas_line(pixels, x + 7, y + 12, x + 17, y + 12, ink);
        pw_platform_canvas_line(pixels, x + 7, y + 16, x + 14, y + 16, ink);
    }
}

void pw_platform_canvas_glyph(pw_platform_u8 *pixels,
                              const pw_platform_u8 font[256][16],
                              unsigned char glyph, int x, int y,
                              pw_platform_u8 foreground,
                              pw_platform_u8 background, int opaque)
{
    int row;
    int col;
    if (pixels == NULL || font == NULL) return;
    for (row = 0; row < 16; ++row) {
        pw_platform_u8 bits = font[glyph][row];
        for (col = 0; col < 8; ++col) {
            if (bits & (0x80U >> col))
                pw_platform_canvas_pixel(pixels, x + col, y + row, foreground);
            else if (opaque)
                pw_platform_canvas_pixel(pixels, x + col, y + row, background);
        }
    }
}

void pw_platform_canvas_text(pw_platform_u8 *pixels,
                             const pw_platform_u8 font[256][16],
                             const char *text, int x, int y,
                             pw_platform_u8 color)
{
    if (text == NULL) return;
    while (*text != '\0') {
        pw_platform_canvas_glyph(pixels, font, (unsigned char)*text,
                                 x, y, color, 7, 0);
        x += 8;
        ++text;
    }
}

static const pw_platform_u8 g_fallback_glyphs[36][7] = {
    {14,17,17,31,17,17,17}, {30,17,17,30,17,17,30}, {14,17,16,16,16,17,14},
    {30,17,17,17,17,17,30}, {31,16,16,30,16,16,31}, {31,16,16,30,16,16,16},
    {14,17,16,23,17,17,15}, {17,17,17,31,17,17,17}, {14,4,4,4,4,4,14},
    {7,2,2,2,18,18,12}, {17,18,20,24,20,18,17}, {16,16,16,16,16,16,31},
    {17,27,21,21,17,17,17}, {17,25,21,19,17,17,17}, {14,17,17,17,17,17,14},
    {30,17,17,30,16,16,16}, {14,17,17,17,21,18,13}, {30,17,17,30,20,18,17},
    {15,16,16,14,1,1,30}, {31,4,4,4,4,4,4}, {17,17,17,17,17,17,14},
    {17,17,17,17,17,10,4}, {17,17,17,21,21,21,10}, {17,17,10,4,10,17,17},
    {17,17,10,4,4,4,4}, {31,1,2,4,8,16,31},
    {14,17,19,21,25,17,14}, {4,12,4,4,4,4,14}, {14,17,1,2,4,8,31},
    {30,1,1,14,1,1,30}, {2,6,10,18,31,2,2}, {31,16,16,30,1,1,30},
    {14,16,16,30,17,17,14}, {31,1,2,4,8,8,8}, {14,17,17,14,17,17,14},
    {14,17,17,15,1,1,14}
};

void pw_platform_font_init_fallback(pw_platform_u8 font[256][16])
{
    unsigned i;
    memset(font, 0, 256U * 16U);
    for (i = 0; i < 256; ++i) {
        unsigned row;
        unsigned glyph = 0;
        if (i >= 'a' && i <= 'z') glyph = i - 'a';
        else if (i >= 'A' && i <= 'Z') glyph = i - 'A';
        else if (i >= '0' && i <= '9') glyph = 26 + i - '0';
        if ((i >= 'a' && i <= 'z') || (i >= 'A' && i <= 'Z') ||
            (i >= '0' && i <= '9')) {
            for (row = 0; row < 7; ++row) {
                unsigned col;
                for (col = 0; col < 5; ++col) {
                    if (g_fallback_glyphs[glyph][row] & (1U << (4 - col)))
                        font[i][row + 4] |= (pw_platform_u8)(0x80U >> (col + 1));
                }
            }
        } else if (i != ' ') {
            font[i][5] = 0x18;
            font[i][6] = 0x18;
            font[i][7] = 0x18;
            font[i][8] = 0x18;
            font[i][9] = 0x18;
        }
    }
}

int pw_platform_font_load_bios(pw_platform_u8 font[256][16])
{
    static pw_platform_u8 candidate_font[256][16];
    u16 start_segment;
    u16 start_offset;
    unsigned glyph;
    unsigned row;
    unsigned long font_pointer = query_bios_font();
    start_segment = (u16)(font_pointer >> 16);
    start_offset = (u16)font_pointer;
    if (start_segment == 0 || start_offset == 0) return 0;
    for (glyph = 0; glyph < 256; ++glyph) {
        u32 physical = (((u32)start_segment << 4) + start_offset + glyph * 16UL) & 0x000fffffUL;
        u16 segment = (u16)(physical >> 4);
        u16 offset = (u16)(physical & 0x0f);
        u16 selector = map_real_mode_segment(segment);
        volatile unsigned char __far *source;
        if (selector == 0) return 0;
        source = (volatile unsigned char __far *)MK_FP(selector, offset);
        for (row = 0; row < 16; ++row) candidate_font[glyph][row] = source[row];
        free_selector(selector);
    }
    memcpy(font, candidate_font, sizeof(candidate_font));
    return 1;
}

static void mouse_interrupt(u16 ax, u16 bx, u16 cx, u16 dx, union REGS *out_regs)
{
    union REGS in_regs;
    struct SREGS seg_regs;
    memset(&in_regs, 0, sizeof(in_regs));
    memset(out_regs, 0, sizeof(*out_regs));
    segread(&seg_regs);
    in_regs.w.ax = ax;
    in_regs.w.bx = bx;
    in_regs.w.cx = cx;
    in_regs.w.dx = dx;
    int386x(0x33, &in_regs, out_regs, &seg_regs);
}

int pw_platform_mouse_initialize(void)
{
    union REGS out_regs;
    mouse_interrupt(0x0000, 0, 0, 0, &out_regs);
    if (out_regs.w.ax != 0xffff) {
        g_mouse_available = 0;
        return 0;
    }
    mouse_interrupt(0x0007, 0, 0, PW_PLATFORM_SCREEN_WIDTH - 1, &out_regs);
    mouse_interrupt(0x0008, 0, 0, PW_PLATFORM_SCREEN_HEIGHT - 1, &out_regs);
    mouse_interrupt(0x0004, 0, PW_PLATFORM_SCREEN_WIDTH / 2,
                    PW_PLATFORM_SCREEN_HEIGHT / 2, &out_regs);
    mouse_interrupt(0x0002, 0, 0, 0, &out_regs);
    g_mouse_available = 1;
    return 1;
}

void pw_platform_mouse_restore(void)
{
    union REGS out_regs;
    if (g_mouse_available) mouse_interrupt(0x0001, 0, 0, 0, &out_regs);
}

int pw_platform_mouse_read_state(int *x, int *y, unsigned *buttons)
{
    union REGS out_regs;
    if (!g_mouse_available || x == NULL || y == NULL || buttons == NULL) return 0;
    mouse_interrupt(0x0003, 0, 0, 0, &out_regs);
    *x = out_regs.w.cx >= PW_PLATFORM_SCREEN_WIDTH ? PW_PLATFORM_SCREEN_WIDTH - 1 : out_regs.w.cx;
    *y = out_regs.w.dx >= PW_PLATFORM_SCREEN_HEIGHT ? PW_PLATFORM_SCREEN_HEIGHT - 1 : out_regs.w.dx;
    *buttons = (unsigned)out_regs.w.bx & 7;
    return 1;
}
