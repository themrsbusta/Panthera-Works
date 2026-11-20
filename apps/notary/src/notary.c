/*
 * Panthera Notary: native 32-bit NF-DOS editor prototype.
 * Original Panthera source; the reference projects informed feature research only.
 */
#include <conio.h>
#include <dos.h>
#include <i86.h>
#include <io.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../../shared/ooxml/docx_format.h"
#include "../../shared/ooxml/docx_reader.h"
#include "../../shared/platform/pw_platform.h"

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned long u32;

#define SCREEN_W PW_PLATFORM_SCREEN_WIDTH
#define SCREEN_H PW_PLATFORM_SCREEN_HEIGHT
#define MAX_PATH_BYTES 96
#define HISTORY_ITEMS 128
#define DOC_COLUMNS 54
#define DOC_ROWS 18
#define DOC_X 104
#define DOC_Y 134
#define CELL_W 8
#define CELL_H 16

enum {
    CHAR_BOLD = 1,
    CHAR_ITALIC = 2,
    CHAR_UNDERLINE = 4,
    CHAR_NUMBERED = 32,
    CHAR_BULLETED = 64,
    CHAR_LIST_MASK = 96,
    EDIT_TEXT = 0,
    EDIT_STYLE_RANGE = 1,
    EDIT_INSERT_STYLE = 2,
    EDIT_INSERT_ALIGNMENT = 3,
    PARA_ALIGN_MASK = 24,
    PARA_ALIGN_LEFT = 0,
    PARA_ALIGN_CENTER = 8,
    PARA_ALIGN_RIGHT = 16,
    PARA_ALIGN_JUSTIFY = 24
};

#pragma pack(1)
typedef struct {
    char magic[8];
    u16 version;
    u16 header_bytes;
    u32 encoding;
    u32 content_bytes;
    u32 crc32;
    u32 flags;
} NativeHeader;
#pragma pack()

typedef struct {
    u32 position;
    u8 *removed;
    u8 *inserted;
    u8 *removed_style;
    u8 *inserted_style;
    u32 removed_bytes;
    u32 inserted_bytes;
    u8 kind;
    u8 old_insert_style;
    u8 new_insert_style;
    clock_t time;
} Edit;

typedef struct {
    const char *label;
    const char *shortcut;
    int action;
} MenuItem;

enum { MENU_NONE = 0, MENU_FILE = 1, MENU_EDIT = 2 };

static const MenuItem g_file_menu[] = {
    {"New", "Ctrl+N", 0},
    {"Open...", "Ctrl+O", 1},
    {"Save", "Ctrl+S", 2},
    {"Save As...", "F2", 7},
    {"Exit", "Alt+F4", 8}
};

static const MenuItem g_edit_menu[] = {
    {"Undo", "Ctrl+Z", 3},
    {"Redo", "Ctrl+Y", 4},
    {"Find...", "Ctrl+F", 5},
    {"Replace...", "Ctrl+H", 6}
};

static int g_mouse_available;
static int g_mouse_x;
static int g_mouse_y;
static int g_mouse_hover = -1;
static int g_format_hover = -1;
static unsigned g_mouse_buttons;
static int g_mouse_selecting;
static int g_menu_open;
static int g_menu_selection;
static u8 g_pixels[SCREEN_W * SCREEN_H];
static u8 g_font[256][16];
static u8 *g_text;
static u8 *g_char_style;
static u32 g_text_bytes;
static u32 g_text_capacity;
static u32 g_cursor;
static u8 g_insert_style;
static u8 g_insert_alignment;
static u32 g_anchor;
static int g_selecting;
static int g_dirty;
static int g_running = 1;
static int g_exit_prompt;
static int g_status_error;
static char g_status[80];
static char g_prompt_title[80];
static char g_prompt_value[80];
static int g_prompt_active;
static char g_path[MAX_PATH_BYTES];
static u8 *g_clipboard;
static u32 g_clipboard_bytes;
static Edit g_history[HISTORY_ITEMS];
static unsigned g_history_count;
static unsigned g_history_cursor;
static int g_lfn_driver_ready;
static char g_log_path[] = "NOTARY.LOG";
static FILE *g_log;

static int selection_range(u32 *start, u32 *bytes);
static void status_set(const char *text, int is_error);

static void log_line(const char *text)
{
    if (g_log != NULL) {
        fputs(text, g_log);
        fputc('\n', g_log);
        fflush(g_log);
    }
}

static void pixel(int x, int y, u8 color)
{
    pw_platform_canvas_pixel(g_pixels, x, y, color);
}

static void fill_rect(int x, int y, int width, int height, u8 color)
{
    pw_platform_canvas_fill_rect(g_pixels, x, y, width, height, color);
}

static void draw_glyph(unsigned char glyph, int x, int y, u8 foreground, u8 background, int opaque)
{
    pw_platform_canvas_glyph(g_pixels, g_font, glyph, x, y,
                             foreground, background, opaque);
}

static void draw_ascii(const char *text, int x, int y, u8 color)
{
    pw_platform_canvas_text(g_pixels, g_font, text, x, y, color);
}

static void draw_line(int x0, int y0, int x1, int y1, u8 color)
{
    pw_platform_canvas_line(g_pixels, x0, y0, x1, y1, color);
}

static void draw_outline_rect(int x, int y, int width, int height, u8 color)
{
    draw_line(x, y, x + width - 1, y, color);
    draw_line(x, y, x, y + height - 1, color);
    draw_line(x + width - 1, y, x + width - 1, y + height - 1, color);
    draw_line(x, y + height - 1, x + width - 1, y + height - 1, color);
}

static void invoke_toolbar_action(int action);
static void invoke_format_action(int action);

static int menu_item_count(int menu)
{
    if (menu == MENU_FILE) return (int)(sizeof(g_file_menu) / sizeof(g_file_menu[0]));
    if (menu == MENU_EDIT) return (int)(sizeof(g_edit_menu) / sizeof(g_edit_menu[0]));
    return 0;
}

static const MenuItem *menu_item_at(int menu, int index)
{
    if (index < 0 || index >= menu_item_count(menu)) return NULL;
    return menu == MENU_FILE ? &g_file_menu[index] : &g_edit_menu[index];
}

static int menu_left(int menu)
{
    return menu == MENU_EDIT ? 64 : 8;
}

static int menu_width(int menu)
{
    return 190;
}

static int menu_bar_hit(int x, int y)
{
    if (y < 24 || y >= 50) return MENU_NONE;
    if (x >= 8 && x < 52) return MENU_FILE;
    if (x >= 64 && x < 108) return MENU_EDIT;
    return MENU_NONE;
}

static int menu_item_hit(int x, int y)
{
    int row;
    int top = 49;
    if (g_menu_open == MENU_NONE || x < menu_left(g_menu_open) ||
        x >= menu_left(g_menu_open) + menu_width(g_menu_open) || y < top + 3) return -1;
    row = (y - (top + 3)) / 22;
    return row < menu_item_count(g_menu_open) ? row : -1;
}

static void menu_open(int menu)
{
    g_menu_open = menu;
    g_menu_selection = 0;
}

static void menu_close(void)
{
    g_menu_open = MENU_NONE;
    g_menu_selection = -1;
}

static void menu_move_selection(int delta)
{
    int count = menu_item_count(g_menu_open);
    if (count == 0) return;
    g_menu_selection = (g_menu_selection + delta + count) % count;
}

static void menu_activate_selection(void)
{
    const MenuItem *item = menu_item_at(g_menu_open, g_menu_selection);
    int action;
    if (item == NULL) return;
    action = item->action;
    menu_close();
    invoke_toolbar_action(action);
}

static int toolbar_button_x(int index)
{
    static const int positions[7] = {8, 34, 60, 98, 124, 162, 188};
    return index >= 0 && index < 7 ? positions[index] : -100;
}

static int toolbar_hit(int x, int y)
{
    int i;
    if (y < 52 || y >= 72) return -1;
    for (i = 0; i < 7; ++i) {
        int left = toolbar_button_x(i);
        if (x >= left && x < left + 22) return i;
    }
    return -1;
}

static int format_button_x(int index)
{
    static const int positions[11] = {286, 310, 334, 374, 398, 422, 446, 478, 502, 526, 550};
    return index >= 0 && index < 11 ? positions[index] : -100;
}

static int format_toolbar_hit(int x, int y)
{
    int i;
    if (y < 77 || y >= 99) return -1;
    for (i = 0; i < 11; ++i) {
        int left = format_button_x(i);
        if (x >= left && x < left + 20) return i;
    }
    return -1;
}

static void draw_toolbar_icon(int index, int x, int y)
{
    pw_platform_canvas_toolbar_icon(g_pixels, index, x, y, 0, 1);
}

static const char *toolbar_label(int index)
{
    static const char *labels[7] = {"NEW", "OPEN", "SAVE", "UNDO", "REDO", "FIND", "REPLACE"};
    return index >= 0 && index < 7 ? labels[index] : "";
}

static void draw_toolbar(void)
{
    int i;
    for (i = 0; i < 7; ++i) {
        int x = toolbar_button_x(i);
        u8 face = g_mouse_hover == i ? 9 : 7;
        fill_rect(x, 52, 22, 20, face);
        draw_line(x, 52, x + 21, 52, 15);
        draw_line(x, 52, x, 71, 15);
        draw_line(x + 21, 52, x + 21, 71, 8);
        draw_line(x, 71, x + 21, 71, 8);
        draw_toolbar_icon(i, x + 2, 53);
    }
    draw_line(91, 54, 91, 70, 8);
    draw_line(155, 54, 155, 70, 8);
}

static void draw_format_icon(int index, int x, int y)
{
    int row;
    if (index == 0) {
        draw_ascii("B", x + 6, y + 2, 0);
        draw_line(x + 6, y + 2, x + 14, y + 2, 0);
        draw_line(x + 6, y + 18, x + 14, y + 18, 0);
    } else if (index == 1) {
        draw_line(x + 5, y + 2, x + 14, y + 2, 0);
        draw_line(x + 9, y + 2, x + 9, y + 18, 0);
        draw_line(x + 5, y + 18, x + 14, y + 18, 0);
    } else if (index == 2) {
        draw_line(x + 5, y + 15, x + 14, y + 15, 0);
        draw_line(x + 9, y + 2, x + 9, y + 14, 0);
        draw_line(x + 6, y + 2, x + 12, y + 2, 0);
    } else if (index >= 3 && index <= 6) {
        for (row = 0; row < 4; ++row) {
            int left = x + 4;
            int right = x + 15;
            int yy = y + 4 + row * 4;
            if (index == 3) right = x + (row % 2 ? 13 : 10);
            else if (index == 4) { left = x + (row % 2 ? 6 : 8); right = x + (row % 2 ? 17 : 15); }
            else if (index == 5) { left = x + 4; right = x + (row % 2 ? 13 : 10); }
            else { left = x + 4; right = x + 15; }
            draw_line(left, yy, right, yy, 0);
        }
        if (index == 6) draw_line(x + 4, y + 18, x + 15, y + 18, 0);
    } else if (index == 7 || index == 8) {
        for (row = 0; row < 3; ++row) {
            int yy = y + 4 + row * 5;
            pixel(x + 3, yy, 1);
            draw_line(x + 7, yy, x + 16, yy, 0);
        }
        if (index == 7) {
            draw_line(x + 3, y + 2, x + 3, y + 17, 1);
        } else {
            draw_ascii("1", x + 1, y + 2, 1);
            draw_ascii("2", x + 1, y + 7, 1);
            draw_ascii("3", x + 1, y + 12, 1);
        }
    } else {
        draw_line(x + 4, y + 5, x + 15, y + 5, 0);
        draw_line(x + 4, y + 10, x + 15, y + 10, 0);
        draw_line(x + 4, y + 15, x + 15, y + 15, 0);
        if (index == 9) draw_line(x + 4, y + 10, x + 10, y + 10, 1);
        else draw_line(x + 9, y + 10, x + 15, y + 10, 1);
    }
}

static const char *format_label(int index)
{
    static const char *labels[11] = {
        "BOLD", "ITALIC", "UNDERLINE", "ALIGN LEFT", "CENTER", "ALIGN RIGHT",
        "JUSTIFY", "BULLETS", "NUMBERING", "DECREASE INDENT", "INCREASE INDENT"
    };
    return index >= 0 && index < 11 ? labels[index] : "";
}

static void draw_formatting_toolbar(void)
{
    static const char *combo_text[3] = {"NORMAL", "SYSTEM", "12"};
    static const int combo_x[3] = {8, 104, 238};
    static const int combo_w[3] = {88, 126, 40};
    int i;
    for (i = 0; i < 3; ++i) {
        int x = combo_x[i];
        int width = combo_w[i];
        fill_rect(x, 77, width, 20, 15);
        draw_line(x, 77, x + width - 1, 77, 8);
        draw_line(x, 77, x, 96, 8);
        draw_line(x + width - 1, 77, x + width - 1, 96, 15);
        draw_line(x, 96, x + width - 1, 96, 15);
        draw_ascii(combo_text[i], x + 4, 79, 0);
        draw_line(x + width - 12, 83, x + width - 5, 83, 0);
        draw_line(x + width - 12, 83, x + width - 9, 87, 0);
        draw_line(x + width - 9, 87, x + width - 5, 83, 0);
    }
    draw_line(278, 78, 278, 96, 8);
    draw_line(366, 78, 366, 96, 8);
    for (i = 0; i < 11; ++i) {
        int x = format_button_x(i);
        u8 face = g_format_hover == i ? 9 : 7;
        if (i < 3) {
            u8 flag = (u8)(1U << i);
            u8 style = g_insert_style;
            if (g_cursor < g_text_bytes)
                style = g_char_style[g_cursor];
            else if (g_cursor > 0 && g_text[g_cursor - 1] != '\n')
                style = g_char_style[g_cursor - 1];
            if (style & flag) face = g_format_hover == i ? 10 : 9;
        } else if (i >= 3 && i <= 6) {
            u8 alignment = g_insert_alignment;
            if (g_cursor < g_text_bytes)
                alignment = (u8)(g_char_style[g_cursor] & PARA_ALIGN_MASK);
            else if (g_cursor > 0 && g_text[g_cursor - 1] != '\n')
                alignment = (u8)(g_char_style[g_cursor - 1] & PARA_ALIGN_MASK);
            if (alignment == (u8)((i - 3) * 8)) face = g_format_hover == i ? 10 : 9;
        } else if (i == 7 || i == 8) {
            u8 style = g_insert_style;
            if (g_cursor < g_text_bytes) style = g_char_style[g_cursor];
            else if (g_cursor > 0) style = g_char_style[g_cursor - 1];
            if (style & (i == 7 ? CHAR_BULLETED : CHAR_NUMBERED))
                face = g_format_hover == i ? 10 : 9;
        }
        fill_rect(x, 77, 20, 20, face);
        draw_line(x, 77, x + 19, 77, 15);
        draw_line(x, 77, x, 96, 15);
        draw_line(x + 19, 77, x + 19, 96, 8);
        draw_line(x, 96, x + 19, 96, 8);
        draw_format_icon(i, x, 77);
    }
}

static void draw_rulers(void)
{
    int x;
    int mark;
    fill_rect(88, 101, 466, 16, 7);
    draw_line(88, 101, 553, 101, 15);
    draw_line(88, 116, 553, 116, 8);
    for (mark = 0; mark <= 6; ++mark) {
        x = DOC_X + mark * 72;
        draw_ascii(mark == 0 ? "1" : mark == 1 ? "2" : mark == 2 ? "3" :
                   mark == 3 ? "4" : mark == 4 ? "5" : mark == 5 ? "6" : "7",
                   x + 2, 101, 0);
        draw_line(x, 112, x, 116, 0);
        if (mark < 6) {
            draw_line(x + 24, 114, x + 24, 116, 8);
            draw_line(x + 48, 114, x + 48, 116, 8);
        }
    }
    fill_rect(64, DOC_Y, 14, DOC_ROWS * CELL_H, 7);
    for (mark = 0; mark < DOC_ROWS; ++mark) {
        int y = DOC_Y + mark * CELL_H;
        draw_line(73, y, 77, y, mark % 5 == 0 ? 0 : 8);
    }
}

static void draw_menu_bar(void)
{
    if (g_menu_open == MENU_FILE) fill_rect(8, 25, 44, 23, 1);
    if (g_menu_open == MENU_EDIT) fill_rect(64, 25, 44, 23, 1);
    draw_ascii("FILE", 12, 29, g_menu_open == MENU_FILE ? 15 : 0);
    draw_ascii("EDIT", 68, 29, g_menu_open == MENU_EDIT ? 15 : 0);
    draw_ascii("INSERT", 124, 29, 0);
    draw_ascii("FORMAT", 212, 29, 0);
    draw_ascii("VIEW", 292, 29, 0);
    draw_ascii("HELP", 356, 29, 0);
}

static void draw_dropdown_menu(void)
{
    int x;
    int y = 49;
    int width;
    int height;
    int i;
    if (g_menu_open == MENU_NONE) return;
    x = menu_left(g_menu_open);
    width = menu_width(g_menu_open);
    height = menu_item_count(g_menu_open) * 22 + 6;
    fill_rect(x + 4, y + 4, width, height, 8);
    fill_rect(x, y, width, height, 7);
    draw_outline_rect(x, y, width, height, 0);
    for (i = 0; i < menu_item_count(g_menu_open); ++i) {
        const MenuItem *item = menu_item_at(g_menu_open, i);
        int item_y = y + 3 + i * 22;
        int selected = i == g_menu_selection;
        int color = selected ? 15 : 0;
        int shortcut_x;
        if (selected) fill_rect(x + 2, item_y, width - 4, 21, 1);
        draw_ascii(item->label, x + 10, item_y + 2, color);
        shortcut_x = x + width - 10 - (int)strlen(item->shortcut) * 8;
        draw_ascii(item->shortcut, shortcut_x, item_y + 2, color);
    }
}

static void draw_mouse_pointer(void)
{
    if (!g_mouse_available) return;
    draw_line(g_mouse_x, g_mouse_y, g_mouse_x, g_mouse_y + 12, 0);
    draw_line(g_mouse_x, g_mouse_y, g_mouse_x + 9, g_mouse_y + 8, 0);
    draw_line(g_mouse_x, g_mouse_y + 12, g_mouse_x + 4, g_mouse_y + 10, 0);
    draw_line(g_mouse_x + 4, g_mouse_y + 10, g_mouse_x + 8, g_mouse_y + 16, 0);
    draw_line(g_mouse_x + 8, g_mouse_y + 16, g_mouse_x + 11, g_mouse_y + 14, 0);
    draw_line(g_mouse_x + 11, g_mouse_y + 14, g_mouse_x + 8, g_mouse_y + 8, 0);
    draw_line(g_mouse_x + 8, g_mouse_y + 8, g_mouse_x, g_mouse_y, 0);
    draw_line(g_mouse_x + 2, g_mouse_y + 3, g_mouse_x + 2, g_mouse_y + 9, 15);
    draw_line(g_mouse_x + 3, g_mouse_y + 3, g_mouse_x + 7, g_mouse_y + 7, 15);
}

static void present(void)
{
    pw_platform_video_present(g_pixels);
}

static unsigned long crc32_bytes(const u8 *data, u32 bytes)
{
    u32 crc = 0xffffffffUL;
    u32 i;
    for (i = 0; i < bytes; ++i) {
        unsigned bit;
        crc ^= data[i];
        for (bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320UL & (u32)-(int)(crc & 1));
    }
    return crc ^ 0xffffffffUL;
}

static int doc_reserve(u32 need)
{
    u32 capacity;
    u8 *next_text;
    u8 *next_style;
    if (need <= g_text_capacity) return 1;
    capacity = g_text_capacity == 0 ? 4096UL : g_text_capacity;
    while (capacity < need) {
        if (capacity > 0x7fffffffUL / 2) {
            capacity = need;
            break;
        }
        capacity *= 2;
    }
    next_text = (u8 *)realloc(g_text, capacity);
    if (next_text == NULL) return 0;
    g_text = next_text;
    next_style = (u8 *)realloc(g_char_style, capacity);
    if (next_style == NULL) return 0;
    g_char_style = next_style;
    g_text_capacity = capacity;
    return 1;
}

static void history_release(Edit *edit)
{
    if (edit->removed != NULL) free(edit->removed);
    if (edit->inserted != NULL) free(edit->inserted);
    if (edit->removed_style != NULL) free(edit->removed_style);
    if (edit->inserted_style != NULL) free(edit->inserted_style);
    memset(edit, 0, sizeof(*edit));
}

static void history_clear(void)
{
    unsigned i;
    for (i = 0; i < g_history_count; ++i) history_release(&g_history[i]);
    g_history_count = 0;
    g_history_cursor = 0;
}

static int history_drop_oldest(void)
{
    unsigned i;
    if (g_history_count == 0 || g_history_cursor == 0) return 0;
    history_release(&g_history[0]);
    for (i = 1; i < g_history_count; ++i) g_history[i - 1] = g_history[i];
    --g_history_count;
    --g_history_cursor;
    memset(&g_history[g_history_count], 0, sizeof(g_history[g_history_count]));
    return 1;
}

static u8 document_insertion_style(u32 position)
{
    u8 paragraph_alignment = g_insert_alignment;
    u8 paragraph_numbering = (u8)(g_insert_style & CHAR_LIST_MASK);
    if (position < g_text_bytes) {
        paragraph_alignment = (u8)(g_char_style[position] & PARA_ALIGN_MASK);
        paragraph_numbering = (u8)(g_char_style[position] & CHAR_LIST_MASK);
    } else if (position > 0) {
        paragraph_numbering = (u8)(g_char_style[position - 1] & CHAR_LIST_MASK);
        if (g_text[position - 1] != '\n')
            paragraph_alignment = (u8)(g_char_style[position - 1] & PARA_ALIGN_MASK);
    }
    return (u8)((g_insert_style & 7) | paragraph_numbering | paragraph_alignment);
}

static int history_push(u32 position, const u8 *removed, u32 removed_bytes,
                        const u8 *inserted, u32 inserted_bytes, int groupable)
{
    Edit *edit;
    u8 *new_inserted;
    u8 *new_inserted_style;
    u8 *new_removed = NULL;
    u8 *new_removed_style = NULL;
    clock_t now = clock();
    if (groupable && removed_bytes == 0 && inserted_bytes == 1 &&
        inserted[0] != '\n' && g_history_cursor == g_history_count && g_history_count > 0) {
        edit = &g_history[g_history_count - 1];
        if (edit->removed_bytes == 0 && edit->inserted_bytes > 0 &&
            edit->position + edit->inserted_bytes == position &&
            (now - edit->time) <= 2 * CLOCKS_PER_SEC && edit->inserted_bytes < 65535UL) {
            new_inserted = (u8 *)realloc(edit->inserted, edit->inserted_bytes + 1);
            if (new_inserted != NULL) {
                edit->inserted = new_inserted;
                new_inserted_style = (u8 *)realloc(edit->inserted_style, edit->inserted_bytes + 1);
                if (new_inserted_style == NULL) return 0;
                edit->inserted_style = new_inserted_style;
                edit->inserted[edit->inserted_bytes] = inserted[0];
                edit->inserted_style[edit->inserted_bytes] = document_insertion_style(position);
                ++edit->inserted_bytes;
                edit->time = now;
                return 1;
            }
        }
    }
    while (g_history_count > g_history_cursor) {
        history_release(&g_history[g_history_count - 1]);
        --g_history_count;
    }
    while (g_history_count >= HISTORY_ITEMS) {
        if (!history_drop_oldest()) break;
    }
    edit = &g_history[g_history_count];
    memset(edit, 0, sizeof(*edit));
    if (removed_bytes != 0) {
        new_removed = (u8 *)malloc(removed_bytes);
        new_removed_style = (u8 *)malloc(removed_bytes);
        if (new_removed == NULL || new_removed_style == NULL) {
            if (new_removed != NULL) free(new_removed);
            if (new_removed_style != NULL) free(new_removed_style);
            return 0;
        }
        memcpy(new_removed, removed, removed_bytes);
        memcpy(new_removed_style, g_char_style + position, removed_bytes);
    }
    if (inserted_bytes != 0) {
        new_inserted = (u8 *)malloc(inserted_bytes);
        new_inserted_style = (u8 *)malloc(inserted_bytes);
        if (new_inserted == NULL || new_inserted_style == NULL) {
            if (new_removed != NULL) free(new_removed);
            if (new_removed_style != NULL) free(new_removed_style);
            if (new_inserted != NULL) free(new_inserted);
            if (new_inserted_style != NULL) free(new_inserted_style);
            return 0;
        }
        memcpy(new_inserted, inserted, inserted_bytes);
        memset(new_inserted_style, document_insertion_style(position), inserted_bytes);
    } else {
        new_inserted = NULL;
        new_inserted_style = NULL;
    }
    edit->position = position;
    edit->removed = new_removed;
    edit->inserted = new_inserted;
    edit->removed_style = new_removed_style;
    edit->inserted_style = new_inserted_style;
    edit->removed_bytes = removed_bytes;
    edit->inserted_bytes = inserted_bytes;
    edit->kind = EDIT_TEXT;
    edit->time = now;
    ++g_history_count;
    ++g_history_cursor;
    return 1;
}

static void doc_splice_raw(u32 position, u32 remove_bytes, const u8 *inserted,
                           const u8 *inserted_style, u32 inserted_bytes)
{
    u32 tail = g_text_bytes - position - remove_bytes;
    u8 default_style = document_insertion_style(position);
    if (inserted_bytes > remove_bytes)
        memmove(g_text + position + inserted_bytes, g_text + position + remove_bytes, tail);
    else if (inserted_bytes < remove_bytes)
        memmove(g_text + position + inserted_bytes, g_text + position + remove_bytes, tail);
    if (inserted_bytes > remove_bytes)
        memmove(g_char_style + position + inserted_bytes,
                g_char_style + position + remove_bytes, tail);
    else if (inserted_bytes < remove_bytes)
        memmove(g_char_style + position + inserted_bytes,
                g_char_style + position + remove_bytes, tail);
    if (inserted_bytes != 0) memcpy(g_text + position, inserted, inserted_bytes);
    if (inserted_bytes != 0) {
        if (inserted_style != NULL) memcpy(g_char_style + position, inserted_style, inserted_bytes);
        else memset(g_char_style + position, default_style, inserted_bytes);
    }
    g_text_bytes = g_text_bytes - remove_bytes + inserted_bytes;
    g_text[g_text_bytes] = 0;
    g_char_style[g_text_bytes] = 0;
}

static int doc_splice(u32 position, u32 remove_bytes, const u8 *inserted,
                      u32 inserted_bytes, int groupable)
{
    if (position > g_text_bytes || remove_bytes > g_text_bytes - position) return 0;
    if (inserted_bytes > 0xffffffffUL - (g_text_bytes - remove_bytes)) return 0;
    if (!doc_reserve(g_text_bytes - remove_bytes + inserted_bytes + 1)) return 0;
    if (!history_push(position, g_text + position, remove_bytes, inserted,
                      inserted_bytes, groupable)) return 0;
    doc_splice_raw(position, remove_bytes, inserted, NULL, inserted_bytes);
    g_cursor = position + inserted_bytes;
    g_anchor = g_cursor;
    g_selecting = 0;
    g_dirty = 1;
    return 1;
}

static Edit *history_new_entry(void)
{
    while (g_history_count > g_history_cursor) {
        history_release(&g_history[g_history_count - 1]);
        --g_history_count;
    }
    while (g_history_count >= HISTORY_ITEMS) {
        if (!history_drop_oldest()) return NULL;
    }
    memset(&g_history[g_history_count], 0, sizeof(g_history[g_history_count]));
    return &g_history[g_history_count++];
}

static int history_push_style_range(u32 position, u32 bytes,
                                    const u8 *old_style, const u8 *new_style)
{
    Edit *edit;
    u8 *old_copy;
    u8 *new_copy;
    if (bytes == 0) return 0;
    old_copy = (u8 *)malloc(bytes);
    new_copy = (u8 *)malloc(bytes);
    if (old_copy == NULL || new_copy == NULL) {
        if (old_copy != NULL) free(old_copy);
        if (new_copy != NULL) free(new_copy);
        return 0;
    }
    memcpy(old_copy, old_style, bytes);
    memcpy(new_copy, new_style, bytes);
    edit = history_new_entry();
    if (edit == NULL) { free(old_copy); free(new_copy); return 0; }
    edit->kind = EDIT_STYLE_RANGE;
    edit->position = position;
    edit->removed_style = old_copy;
    edit->inserted_style = new_copy;
    edit->removed_bytes = bytes;
    edit->inserted_bytes = bytes;
    edit->time = clock();
    ++g_history_cursor;
    return 1;
}

static int history_push_insert_style(u8 old_style, u8 new_style)
{
    Edit *edit = history_new_entry();
    if (edit == NULL) return 0;
    edit->kind = EDIT_INSERT_STYLE;
    edit->old_insert_style = old_style;
    edit->new_insert_style = new_style;
    edit->time = clock();
    ++g_history_cursor;
    return 1;
}

static int history_push_insert_alignment(u8 old_alignment, u8 new_alignment)
{
    Edit *edit = history_new_entry();
    if (edit == NULL) return 0;
    edit->kind = EDIT_INSERT_ALIGNMENT;
    edit->old_insert_style = old_alignment;
    edit->new_insert_style = new_alignment;
    edit->time = clock();
    ++g_history_cursor;
    return 1;
}

static void toggle_character_format(u8 flag)
{
    u32 start;
    u32 bytes;
    u32 index;
    if (selection_range(&start, &bytes)) {
        u8 *next;
        int all_enabled = 1;
        if (bytes == 0 || bytes > 0x7fffffffUL) return;
        next = (u8 *)malloc(bytes);
        if (next == NULL) { status_set("FORMAT FAILED; NOT ENOUGH MEMORY", 1); return; }
        for (index = 0; index < bytes; ++index)
            if ((g_char_style[start + index] & flag) == 0) { all_enabled = 0; break; }
        for (index = 0; index < bytes; ++index)
            next[index] = all_enabled ? (u8)(g_char_style[start + index] & (u8)~flag)
                                      : (u8)(g_char_style[start + index] | flag);
        if (!history_push_style_range(start, bytes, g_char_style + start, next)) {
            free(next);
            status_set("FORMAT FAILED; HISTORY FULL", 1);
            return;
        }
        memcpy(g_char_style + start, next, bytes);
        free(next);
        g_dirty = 1;
        if (flag == CHAR_BOLD) log_line(all_enabled ? "FORMAT_BOLD_OFF" : "FORMAT_BOLD_ON");
        else if (flag == CHAR_ITALIC) log_line(all_enabled ? "FORMAT_ITALIC_OFF" : "FORMAT_ITALIC_ON");
        else if (flag == CHAR_UNDERLINE) log_line(all_enabled ? "FORMAT_UNDERLINE_OFF" : "FORMAT_UNDERLINE_ON");
        status_set(all_enabled ? "FORMATTING REMOVED" : "FORMATTING APPLIED", 0);
    } else {
        u8 next_style = (u8)(g_insert_style ^ flag);
        if (!history_push_insert_style(g_insert_style, next_style)) {
            status_set("FORMAT FAILED; HISTORY FULL", 1);
            return;
        }
        g_insert_style = next_style;
        if (flag == CHAR_BOLD) log_line((next_style & flag) ? "FORMAT_BOLD_ON" : "FORMAT_BOLD_OFF");
        else if (flag == CHAR_ITALIC) log_line((next_style & flag) ? "FORMAT_ITALIC_ON" : "FORMAT_ITALIC_OFF");
        else if (flag == CHAR_UNDERLINE) log_line((next_style & flag) ? "FORMAT_UNDERLINE_ON" : "FORMAT_UNDERLINE_OFF");
        status_set((next_style & flag) ? "FORMATTING FOR NEXT TEXT ON" : "FORMATTING FOR NEXT TEXT OFF", 0);
    }
}

static void paragraph_bounds(u32 position, u32 *start, u32 *end)
{
    if (position > g_text_bytes) position = g_text_bytes;
    *start = position;
    while (*start > 0 && g_text[*start - 1] != '\n') --*start;
    *end = position;
    while (*end < g_text_bytes && g_text[*end] != '\n') ++*end;
    if (*end < g_text_bytes) ++*end;
}

static void apply_paragraph_alignment(u8 alignment)
{
    u32 selection_start;
    u32 selection_bytes;
    u32 start;
    u32 end;
    u32 bytes;
    u32 index;
    u8 *next;
    int changed = 0;
    if (alignment & (u8)~PARA_ALIGN_MASK) return;
    if (selection_range(&selection_start, &selection_bytes)) {
        u32 last_position = selection_start + selection_bytes;
        if (last_position > selection_start && g_text[last_position - 1] == '\n') --last_position;
        paragraph_bounds(selection_start, &start, &end);
        {
            u32 ignored_start;
            paragraph_bounds(last_position, &ignored_start, &end);
        }
    } else paragraph_bounds(g_cursor, &start, &end);
    bytes = end - start;
    if (bytes == 0) {
        if (g_insert_alignment == alignment) return;
        if (!history_push_insert_alignment(g_insert_alignment, alignment)) {
            status_set("ALIGNMENT FAILED; HISTORY FULL", 1);
            return;
        }
        g_insert_alignment = alignment;
        if (alignment == PARA_ALIGN_CENTER) log_line("PARA_ALIGN_CENTER");
        else if (alignment == PARA_ALIGN_RIGHT) log_line("PARA_ALIGN_RIGHT");
        else if (alignment == PARA_ALIGN_JUSTIFY) log_line("PARA_ALIGN_JUSTIFY");
        else log_line("PARA_ALIGN_LEFT");
        status_set("PARAGRAPH ALIGNMENT SET", 0);
        return;
    }
    next = (u8 *)malloc(bytes);
    if (next == NULL) { status_set("ALIGNMENT FAILED; NOT ENOUGH MEMORY", 1); return; }
    for (index = 0; index < bytes; ++index) {
        next[index] = (u8)((g_char_style[start + index] & (u8)~PARA_ALIGN_MASK) | alignment);
        if ((g_char_style[start + index] & PARA_ALIGN_MASK) != alignment) changed = 1;
    }
    if (!changed) { free(next); status_set("PARAGRAPH ALIGNMENT ALREADY SET", 0); return; }
    if (!history_push_style_range(start, bytes, g_char_style + start, next)) {
        free(next);
        status_set("ALIGNMENT FAILED; HISTORY FULL", 1);
        return;
    }
    memcpy(g_char_style + start, next, bytes);
    free(next);
    g_dirty = 1;
    if (alignment == PARA_ALIGN_CENTER) log_line("PARA_ALIGN_CENTER");
    else if (alignment == PARA_ALIGN_RIGHT) log_line("PARA_ALIGN_RIGHT");
    else if (alignment == PARA_ALIGN_JUSTIFY) log_line("PARA_ALIGN_JUSTIFY");
    else log_line("PARA_ALIGN_LEFT");
    status_set("PARAGRAPH ALIGNMENT SET", 0);
}

static void apply_list_style(u8 list_flag)
{
    u32 selection_start;
    u32 selection_bytes;
    u32 start;
    u32 end;
    u32 bytes;
    u32 index;
    u8 *next;
    u8 next_style;
    int all_requested = 1;
    const char *log_on;
    const char *log_off;
    if (list_flag != CHAR_NUMBERED && list_flag != CHAR_BULLETED) return;
    log_on = list_flag == CHAR_NUMBERED ? "NUMBERING_ON" : "BULLETS_ON";
    log_off = list_flag == CHAR_NUMBERED ? "NUMBERING_OFF" : "BULLETS_OFF";
    if (selection_range(&selection_start, &selection_bytes)) {
        u32 last_position = selection_start + selection_bytes;
        if (last_position > selection_start && g_text[last_position - 1] == '\n') --last_position;
        paragraph_bounds(selection_start, &start, &end);
        {
            u32 ignored_start;
            paragraph_bounds(last_position, &ignored_start, &end);
        }
    } else paragraph_bounds(g_cursor, &start, &end);
    bytes = end - start;
    if (bytes == 0) {
        if (g_cursor > 0 && g_text[g_cursor - 1] == '\n') {
            u8 old_style = g_char_style[g_cursor - 1];
            u8 new_style = (old_style & list_flag) ?
                (u8)(old_style & (u8)~CHAR_LIST_MASK) :
                (u8)((old_style & (u8)~CHAR_LIST_MASK) | list_flag);
            if (!history_push_style_range(g_cursor - 1, 1, &old_style, &new_style)) {
                status_set("LIST FAILED; HISTORY FULL", 1);
                return;
            }
            g_char_style[g_cursor - 1] = new_style;
            g_dirty = 1;
            log_line((new_style & list_flag) ? log_on : log_off);
            status_set((new_style & list_flag) ?
                       (list_flag == CHAR_NUMBERED ? "NUMBERED LIST FOR NEXT PARAGRAPH" :
                                                     "BULLETED LIST FOR NEXT PARAGRAPH") :
                       (list_flag == CHAR_NUMBERED ? "NUMBERED LIST OFF FOR NEXT PARAGRAPH" :
                                                     "BULLETED LIST OFF FOR NEXT PARAGRAPH"), 0);
            return;
        }
        next_style = (g_insert_style & list_flag) ?
            (u8)(g_insert_style & (u8)~CHAR_LIST_MASK) :
            (u8)((g_insert_style & (u8)~CHAR_LIST_MASK) | list_flag);
        if (!history_push_insert_style(g_insert_style, next_style)) {
            status_set("LIST FAILED; HISTORY FULL", 1);
            return;
        }
        g_insert_style = next_style;
        g_dirty = 1;
        log_line((next_style & list_flag) ? log_on : log_off);
        status_set((next_style & list_flag) ?
                   (list_flag == CHAR_NUMBERED ? "NUMBERED LIST FOR NEXT PARAGRAPH" :
                                                 "BULLETED LIST FOR NEXT PARAGRAPH") :
                   (list_flag == CHAR_NUMBERED ? "NUMBERED LIST OFF FOR NEXT PARAGRAPH" :
                                                 "BULLETED LIST OFF FOR NEXT PARAGRAPH"), 0);
        return;
    }
    if (bytes > 0x7fffffffUL) return;
    for (index = 0; index < bytes; ++index)
        if ((g_char_style[start + index] & CHAR_LIST_MASK) != list_flag) {
            all_requested = 0;
            break;
        }
    next = (u8 *)malloc(bytes);
    if (next == NULL) { status_set("LIST FAILED; NOT ENOUGH MEMORY", 1); return; }
    for (index = 0; index < bytes; ++index)
        next[index] = all_requested ?
            (u8)(g_char_style[start + index] & (u8)~CHAR_LIST_MASK) :
            (u8)((g_char_style[start + index] & (u8)~CHAR_LIST_MASK) | list_flag);
    if (!history_push_style_range(start, bytes, g_char_style + start, next)) {
        free(next);
        status_set("LIST FAILED; HISTORY FULL", 1);
        return;
    }
    memcpy(g_char_style + start, next, bytes);
    free(next);
    g_dirty = 1;
    log_line(all_requested ? log_off : log_on);
    status_set(all_requested ? "LIST FORMATTING REMOVED" :
               (list_flag == CHAR_NUMBERED ? "NUMBERED LIST APPLIED" : "BULLETED LIST APPLIED"), 0);
}

static u32 utf8_step(const u8 *data, u32 bytes, u32 position, u32 *codepoint)
{
    u8 c;
    if (position >= bytes) { *codepoint = 0; return 0; }
    c = data[position];
    if (c < 0x80) { *codepoint = c; return 1; }
    if ((c & 0xe0) == 0xc0 && position + 1 < bytes && (data[position + 1] & 0xc0) == 0x80) {
        *codepoint = ((u32)(c & 0x1f) << 6) | (data[position + 1] & 0x3f);
        return 2;
    }
    if ((c & 0xf0) == 0xe0 && position + 2 < bytes &&
        (data[position + 1] & 0xc0) == 0x80 && (data[position + 2] & 0xc0) == 0x80) {
        *codepoint = ((u32)(c & 0x0f) << 12) | ((u32)(data[position + 1] & 0x3f) << 6) |
                     (data[position + 2] & 0x3f);
        return 3;
    }
    if ((c & 0xf8) == 0xf0 && position + 3 < bytes &&
        (data[position + 1] & 0xc0) == 0x80 && (data[position + 2] & 0xc0) == 0x80 &&
        (data[position + 3] & 0xc0) == 0x80) {
        *codepoint = ((u32)(c & 7) << 18) | ((u32)(data[position + 1] & 0x3f) << 12) |
                     ((u32)(data[position + 2] & 0x3f) << 6) | (data[position + 3] & 0x3f);
        return 4;
    }
    *codepoint = '?';
    return 1;
}

static int utf8_valid(const u8 *data, u32 bytes)
{
    u32 position = 0;
    while (position < bytes) {
        u32 cp;
        u32 step = utf8_step(data, bytes, position, &cp);
        if (step == 0 || (data[position] >= 0x80 && cp == '?')) return 0;
        if ((step == 2 && cp < 0x80) || (step == 3 && cp < 0x800) ||
            (step == 4 && cp < 0x10000) || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) return 0;
        position += step;
    }
    return 1;
}

static unsigned long cp437_unicode(unsigned char value)
{
    static const u16 map[128] = {
        0x00c7,0x00fc,0x00e9,0x00e2,0x00e4,0x00e0,0x00e5,0x00e7,
        0x00ea,0x00eb,0x00e8,0x00ef,0x00ee,0x00ec,0x00c4,0x00c5,
        0x00c9,0x00e6,0x00c6,0x00f4,0x00f6,0x00f2,0x00fb,0x00f9,
        0x00ff,0x00d6,0x00dc,0x00a2,0x00a3,0x00a5,0x20a7,0x0192,
        0x00e1,0x00ed,0x00f3,0x00fa,0x00f1,0x00d1,0x00aa,0x00ba,
        0x00bf,0x2310,0x00ac,0x00bd,0x00bc,0x00a1,0x00ab,0x00bb,
        0x2591,0x2592,0x2593,0x2502,0x2524,0x00c1,0x00c2,0x00c0,
        0x00a9,0x2563,0x2551,0x2557,0x255d,0x00a2,0x00a5,0x2510,
        0x2514,0x2534,0x252c,0x251c,0x2500,0x253c,0x00e3,0x00c3,
        0x255a,0x2554,0x2569,0x2566,0x2560,0x2550,0x256c,0x00a4,
        0x00f0,0x00d0,0x00ca,0x00cb,0x00c8,0x0131,0x00cd,0x00ce,
        0x00cf,0x2518,0x250c,0x2588,0x2584,0x258c,0x2590,0x2580,
        0x00d3,0x00df,0x00d4,0x00d2,0x00f5,0x00d5,0x00b5,0x00fe,
        0x00de,0x00da,0x00db,0x00d9,0x00fd,0x00dd,0x00af,0x00b4,
        0x00ad,0x00b1,0x2017,0x00be,0x00b6,0x00a7,0x00f7,0x00b8,
        0x00b0,0x00a8,0x00b7,0x00b9,0x00b3,0x00b2,0x25a0,0x00a0
    };
    if (value < 128) return value;
    return map[value - 128];
}

static unsigned char unicode_cp437(u32 codepoint)
{
    unsigned i;
    if (codepoint < 128) return (unsigned char)codepoint;
    for (i = 128; i < 256; ++i)
        if (cp437_unicode((u8)i) == codepoint) return (unsigned char)i;
    return '?';
}

static u32 encode_utf8(u32 cp, u8 *out)
{
    if (cp < 0x80) { out[0] = (u8)cp; return 1; }
    if (cp < 0x800) {
        out[0] = (u8)(0xc0 | (cp >> 6));
        out[1] = (u8)(0x80 | (cp & 0x3f));
        return 2;
    }
    out[0] = (u8)(0xe0 | (cp >> 12));
    out[1] = (u8)(0x80 | ((cp >> 6) & 0x3f));
    out[2] = (u8)(0x80 | (cp & 0x3f));
    return 3;
}

static u32 previous_boundary(u32 position)
{
    if (position == 0) return 0;
    --position;
    while (position > 0 && (g_text[position] & 0xc0) == 0x80) --position;
    return position;
}

static void visual_position(u32 target, u32 *row_out, u32 *column_out)
{
    u32 p = 0;
    u32 row = 0;
    u32 col = 0;
    while (p < target && p < g_text_bytes) {
        u32 cp;
        u32 step = utf8_step(g_text, g_text_bytes, p, &cp);
        if (cp == '\n') { ++row; col = 0; }
        else if (++col >= DOC_COLUMNS) { ++row; col = 0; }
        p += step;
    }
    *row_out = row;
    *column_out = col;
}

static u32 offset_at_visual(u32 target_row, u32 target_column)
{
    u32 p = 0;
    u32 row = 0;
    u32 col = 0;
    u32 last_line_start = 0;
    while (p < g_text_bytes) {
        u32 cp;
        u32 step = utf8_step(g_text, g_text_bytes, p, &cp);
        if (row == target_row && col >= target_column) return p;
        if (cp == '\n') {
            ++row; col = 0; last_line_start = p + step;
        } else if (++col >= DOC_COLUMNS) {
            ++row; col = 0; last_line_start = p + step;
        }
        p += step;
    }
    if (row == target_row) return p;
    if (target_row > row) return p;
    return last_line_start;
}

static void draw_codepoint(u32 cp, int x, int y, u8 foreground, u8 background, int opaque)
{
    draw_glyph(unicode_cp437(cp), x, y, foreground, background, opaque);
}

static void draw_formatted_codepoint(u32 cp, int x, int y, u8 foreground,
                                     u8 background, int opaque, u8 style)
{
    unsigned char glyph = unicode_cp437(cp);
    int row;
    int col;
    for (row = 0; row < CELL_H; ++row) {
        u8 bits = g_font[glyph][row];
        int slant = (style & CHAR_ITALIC) ? (row < 8 ? 1 : 0) : 0;
        for (col = 0; col < CELL_W; ++col) {
            int on = (bits & (0x80U >> col)) != 0;
            int px = x + col + slant;
            if (on) {
                pixel(px, y + row, foreground);
                if ((style & CHAR_BOLD) && px + 1 < x + CELL_W + 1)
                    pixel(px + 1, y + row, foreground);
            } else if (opaque) pixel(px, y + row, background);
        }
    }
    if (style & CHAR_UNDERLINE) draw_line(x, y + CELL_H - 2, x + CELL_W - 1, y + CELL_H - 2, foreground);
}

static int visual_row_offset(u32 visual_row)
{
    u32 start = offset_at_visual(visual_row, 0);
    u32 cursor = start;
    u32 columns = 0;
    u8 alignment = g_insert_alignment;
    while (cursor < g_text_bytes && columns < DOC_COLUMNS) {
        u32 cp;
        u32 step = utf8_step(g_text, g_text_bytes, cursor, &cp);
        if (cp == '\n') break;
        ++columns;
        cursor += step;
    }
    if (start < g_text_bytes) alignment = (u8)(g_char_style[start] & PARA_ALIGN_MASK);
    else if (start > 0 && g_text[start - 1] != '\n')
        alignment = (u8)(g_char_style[start - 1] & PARA_ALIGN_MASK);
    {
        int offset = 0;
        if (alignment == PARA_ALIGN_CENTER) offset = (int)(DOC_COLUMNS - columns) / 2;
        else if (alignment == PARA_ALIGN_RIGHT) offset = (int)(DOC_COLUMNS - columns);
        if (start < g_text_bytes && (g_char_style[start] & CHAR_LIST_MASK)) offset += 4;
        else if (start == g_text_bytes && start > 0 && (g_char_style[start - 1] & CHAR_LIST_MASK)) offset += 4;
        return offset;
    }
}

static u32 paragraph_start_at(u32 position)
{
    if (position > g_text_bytes) position = g_text_bytes;
    while (position > 0 && g_text[position - 1] != '\n') --position;
    return position;
}

static u32 numbered_list_index(u32 target_start)
{
    u32 cursor = 0;
    u32 index = 0;
    int sequence = 0;
    while (cursor <= target_start && cursor < g_text_bytes) {
        u32 end = cursor;
        int numbered;
        while (end < g_text_bytes && g_text[end] != '\n') ++end;
        numbered = (g_char_style[cursor] & CHAR_NUMBERED) != 0;
        if (numbered) ++sequence;
        else sequence = 0;
        if (cursor == target_start) { index = numbered ? (u32)sequence : 0; break; }
        cursor = end < g_text_bytes ? end + 1 : g_text_bytes;
    }
    return index;
}

static void draw_document(void)
{
    u32 cursor_row;
    u32 cursor_col;
    u32 row = 0;
    u32 col = 0;
    u32 pos = 0;
    u32 top;
    u32 sel_a = g_anchor < g_cursor ? g_anchor : g_cursor;
    u32 sel_b = g_anchor > g_cursor ? g_anchor : g_cursor;
    visual_position(g_cursor, &cursor_row, &cursor_col);
    top = cursor_row >= DOC_ROWS ? cursor_row - DOC_ROWS + 1 : 0;
    while (pos < g_text_bytes) {
        u32 cp;
        u32 step = utf8_step(g_text, g_text_bytes, pos, &cp);
        if (cp == '\n') { ++row; col = 0; pos += step; continue; }
        if (row >= top && row < top + DOC_ROWS && col < DOC_COLUMNS) {
            int selected = g_selecting && pos >= sel_a && pos < sel_b;
            int row_offset = visual_row_offset(row);
            u32 row_start = offset_at_visual(row, 0);
            u32 para_start = paragraph_start_at(row_start);
            if (para_start == row_start && row_start < g_text_bytes &&
                (g_char_style[row_start] & CHAR_LIST_MASK)) {
                if (g_char_style[row_start] & CHAR_NUMBERED) {
                    char number[16];
                    u32 list_index = numbered_list_index(para_start);
                    sprintf(number, "%lu.", (unsigned long)list_index);
                    draw_ascii(number, DOC_X + (row_offset - 4) * CELL_W,
                               DOC_Y + (int)(row - top) * CELL_H, 0);
                } else {
                    draw_ascii("*", DOC_X + (row_offset - 3) * CELL_W,
                               DOC_Y + (int)(row - top) * CELL_H, 0);
                }
            }
            draw_formatted_codepoint(cp, DOC_X + (int)(col + row_offset) * CELL_W,
                                     DOC_Y + (int)(row - top) * CELL_H,
                                     selected ? 15 : 0, selected ? 1 : 15, selected,
                                     g_char_style[pos]);
        }
        ++col;
        if (col >= DOC_COLUMNS) { ++row; col = 0; }
        pos += step;
    }
    if (cursor_row >= top && cursor_row < top + DOC_ROWS) {
        int x = DOC_X + (int)(cursor_col + visual_row_offset(cursor_row)) * CELL_W;
        int y = DOC_Y + (int)(cursor_row - top) * CELL_H;
        fill_rect(x, y + 1, 2, CELL_H - 2, 4);
    }
}

static void draw_ui(void)
{
    char status[80];
    u32 row;
    u32 col;
    pw_platform_canvas_clear(g_pixels, 7);
    fill_rect(0, 0, SCREEN_W, 24, 1);
    fill_rect(0, 24, SCREEN_W, 25, 7);
    fill_rect(0, 50, SCREEN_W, 67, 8);
    fill_rect(0, 451, SCREEN_W, 29, 7);
    fill_rect(78, 118, 484, 323, 0);
    fill_rect(80, 120, 480, 319, 15);
    fill_rect(80, 120, 480, 1, 8);
    fill_rect(80, 120, 1, 319, 8);
    draw_ascii("PANTHERA NOTARY  -  DOCUMENT WORKSHOP", 10, 4, 15);
    draw_menu_bar();
    draw_toolbar();
    draw_formatting_toolbar();
    draw_rulers();
    draw_document();
    visual_position(g_cursor, &row, &col);
    sprintf(status, "%-20s  %s  LINE %lu  COL %lu  UNDO %u/%u",
            g_path[0] ? g_path : "UNTITLED", g_dirty ? "MODIFIED" : "SAVED",
            row + 1, col + 1, g_history_cursor, g_history_count);
    draw_ascii(status, 8, 457, g_status_error ? 4 : 0);
    if (g_mouse_available && g_mouse_hover >= 0)
        draw_ascii(toolbar_label(g_mouse_hover), 8, 437, 1);
    else if (g_mouse_available && g_format_hover >= 0)
        draw_ascii(format_label(g_format_hover), 8, 437, 1);
    else if (g_status[0] != '\0') draw_ascii(g_status, 8, 437, g_status_error ? 4 : 1);
    if (g_prompt_active) {
        fill_rect(84, 174, 472, 112, 8);
        fill_rect(86, 176, 468, 108, 15);
        draw_ascii(g_prompt_title, 104, 190, 1);
        draw_ascii(g_prompt_value, 104, 222, 0);
        draw_ascii("ENTER = ACCEPT   ESC = CANCEL", 104, 254, 8);
    }
    if (g_exit_prompt) {
        fill_rect(92, 183, 456, 84, 8);
        fill_rect(94, 185, 452, 80, 15);
        draw_ascii("UNSAVED DOCUMENT", 112, 195, 4);
        draw_ascii("Y = SAVE NOW    N = EXIT AND DISCARD    ESC = RETURN", 112, 223, 0);
    }
    draw_dropdown_menu();
    draw_mouse_pointer();
    present();
}

static void status_set(const char *text, int is_error)
{
    strncpy(g_status, text, sizeof(g_status) - 1);
    g_status[sizeof(g_status) - 1] = 0;
    g_status_error = is_error;
}

static int path_has_docx_extension(const char *path)
{
    const char *slash1 = strrchr(path, '\\');
    const char *slash2 = strrchr(path, '/');
    const char *slash = slash1;
    const char *dot = strrchr(path, '.');
    if (slash == NULL || (slash2 != NULL && slash2 > slash)) slash = slash2;
    if (dot == NULL || (slash != NULL && dot < slash)) return 0;
    if (strlen(dot) != 5) return 0;
    return ((dot[1] == 'D' || dot[1] == 'd') &&
            (dot[2] == 'O' || dot[2] == 'o') &&
            (dot[3] == 'C' || dot[3] == 'c') &&
            (dot[4] == 'X' || dot[4] == 'x') && dot[5] == 0);
}

static int choose_sidecar(const char *path, char kind, char *output, unsigned output_bytes)
{
    unsigned i;
    const char *slash1 = strrchr(path, '\\');
    const char *slash2 = strrchr(path, '/');
    const char *slash = slash1;
    const char *dot = strrchr(path, '.');
    unsigned base;
    char candidate[MAX_PATH_BYTES];
    if (slash == NULL || (slash2 != NULL && slash2 > slash)) slash = slash2;
    base = (dot != NULL && (slash == NULL || dot > slash)) ? (unsigned)(dot - path) : (unsigned)strlen(path);
    if (base + 5 >= sizeof(candidate)) return 0;
    for (i = 0; i < 100; ++i) {
        sprintf(candidate, "%.*s.%c%02u", (int)base, path, kind, i);
        if (access(candidate, 0) != 0) {
            if (strlen(candidate) + 1 > output_bytes) return 0;
            strcpy(output, candidate);
            return 1;
        }
    }
    return 0;
}

static int save_docx(const char *path)
{
    char temp_path[MAX_PATH_BYTES];
    char backup_path[MAX_PATH_BYTES];
    int format_result;
    int had_target;
    int target_moved = 0;
    if (!g_lfn_driver_ready) {
        status_set("RUN NOTARY.BAT TO ENABLE DOCX NAMES", 1);
        log_line("DOCX_SAVE_BLOCKED_LFN_UNAVAILABLE");
        return 0;
    }
    if (!choose_sidecar(path, 'T', temp_path, sizeof(temp_path))) {
        status_set("NO FREE TEMP NAME; DOCUMENT NOT SAVED", 1); return 0;
    }
    format_result = pw_docx_write_rich(temp_path, g_text, g_char_style, g_text_bytes);
    if (format_result != PW_DOCX_OK) {
        remove(temp_path);
        status_set(format_result == PW_DOCX_NO_MEMORY ?
                   "NOT ENOUGH MEMORY TO SAVE DOCX" : "DOCX WRITE FAILED; ORIGINAL KEPT", 1);
        return 0;
    }
    had_target = access(path, 0) == 0;
    if (had_target) {
        if (!choose_sidecar(path, 'B', backup_path, sizeof(backup_path))) {
            remove(temp_path); status_set("NO FREE BACKUP NAME; ORIGINAL KEPT", 1); return 0;
        }
        if (rename(path, backup_path) != 0) {
            remove(temp_path); status_set("CANNOT PRESERVE ORIGINAL FILE", 1); return 0;
        }
        target_moved = 1;
    }
    if (rename(temp_path, path) != 0) {
        if (target_moved) rename(backup_path, path);
        remove(temp_path); status_set("RENAME FAILED; ORIGINAL RESTORED", 1); return 0;
    }
    strncpy(g_path, path, sizeof(g_path) - 1);
    g_path[sizeof(g_path) - 1] = 0;
    g_dirty = 0;
    status_set(target_moved ? "SAVED; PREVIOUS VERSION KEPT AS BACKUP" : "DOCUMENT SAVED", 0);
    log_line("DOCX_SAVE_PASS");
    return 1;
}

static int write_plain_text(const char *path)
{
    FILE *file;
    char temp_path[MAX_PATH_BYTES];
    char backup_path[MAX_PATH_BYTES];
    int moved = 0;
    if (!choose_sidecar(path, 'T', temp_path, sizeof(temp_path))) return 0;
    file = fopen(temp_path, "wb");
    if (file == NULL) return 0;
    if ((g_text_bytes && fwrite(g_text, 1, g_text_bytes, file) != g_text_bytes) ||
        fflush(file) != 0) { fclose(file); remove(temp_path); return 0; }
    if (fclose(file) != 0) { remove(temp_path); return 0; }
    if (access(path, 0) == 0) {
        if (!choose_sidecar(path, 'B', backup_path, sizeof(backup_path)) || rename(path, backup_path) != 0) {
            remove(temp_path); return 0;
        }
        moved = 1;
    }
    if (rename(temp_path, path) != 0) {
        if (moved) rename(backup_path, path);
        remove(temp_path); return 0;
    }
    return 1;
}

static int load_document(const char *path)
{
    FILE *file;
    NativeHeader header;
    u8 signature[4];
    u8 *candidate = NULL;
    u8 *candidate_style = NULL;
    u32 bytes = 0;
    u32 capacity = 0;
    int is_native = 0;
    int is_docx = 0;
    int is_zip;
    if (path_has_docx_extension(path) && !g_lfn_driver_ready) {
        status_set("RUN NOTARY.BAT TO ENABLE DOCX NAMES", 1);
        log_line("DOCX_OPEN_BLOCKED_LFN_UNAVAILABLE");
        return 0;
    }
    file = fopen(path, "rb");
    if (file == NULL) { status_set("OPEN FAILED; CURRENT DOCUMENT KEPT", 1); return 0; }
    memset(&header, 0, sizeof(header));
    memset(signature, 0, sizeof(signature));
    is_zip = fread(signature, 1, sizeof(signature), file) == sizeof(signature) &&
             signature[0] == 'P' && signature[1] == 'K' &&
             signature[2] == 3 && signature[3] == 4;
    rewind(file);
    if (is_zip) {
        PwZip32Buffer imported;
        PwZip32Buffer imported_style;
        int read_result;
        fclose(file);
        imported.data = NULL;
        imported.size = 0;
        imported_style.data = NULL;
        imported_style.size = 0;
        read_result = pw_docx_read_rich(path, 0x7ffffffeUL, 0x7ffffffeUL,
                                        &imported, &imported_style);
        if (read_result != PW_DOCX_READ_OK) {
            if (read_result == PW_DOCX_READ_NO_MEMORY)
                status_set("NOT ENOUGH MEMORY; CURRENT DOCUMENT KEPT", 1);
            else if (read_result == PW_DOCX_READ_TOO_LARGE)
                status_set("DOCX CONTENT TOO LARGE; CURRENT DOCUMENT KEPT", 1);
            else status_set("DOCX OPEN FAILED; CURRENT DOCUMENT KEPT", 1);
            return 0;
        }
        candidate = imported.data;
        candidate_style = imported_style.data;
        bytes = imported.size;
        capacity = bytes + 1;
        is_docx = 1;
        file = NULL;
        goto candidate_ready;
    }
    if (fread(&header, 1, sizeof(header), file) == sizeof(header) &&
        memcmp(header.magic, "PNTDOC1", 7) == 0) {
        is_native = 1;
        if (header.version != 1 || header.header_bytes != sizeof(header) || header.encoding != 65001) {
            fclose(file); status_set("UNSUPPORTED PNT VERSION OR ENCODING", 1); return 0;
        }
        if (header.content_bytes == 0xffffffffUL) { fclose(file); status_set("DOCUMENT SIZE INVALID", 1); return 0; }
        candidate = (u8 *)malloc(header.content_bytes + 1);
        if (candidate == NULL) { fclose(file); status_set("NOT ENOUGH MEMORY; CURRENT DOCUMENT KEPT", 1); return 0; }
        if (header.content_bytes && fread(candidate, 1, header.content_bytes, file) != header.content_bytes) {
            free(candidate); fclose(file); status_set("TRUNCATED PNT FILE; CURRENT DOCUMENT KEPT", 1); return 0;
        }
        if (fgetc(file) != EOF || crc32_bytes(candidate, header.content_bytes) != header.crc32 ||
            !utf8_valid(candidate, header.content_bytes)) {
            free(candidate); fclose(file); status_set("PNT VALIDATION FAILED; CURRENT DOCUMENT KEPT", 1); return 0;
        }
        bytes = header.content_bytes;
        candidate[bytes] = 0;
    } else {
        int c;
        u32 raw_bytes;
        u8 *converted;
        rewind(file);
        while ((c = fgetc(file)) != EOF) {
            if (bytes + 2 > capacity) {
                u32 next_capacity = capacity == 0 ? 4096UL : capacity * 2;
                u8 *next;
                if (next_capacity < capacity || next_capacity > 0x7fffffffUL)
                    next_capacity = capacity + 4096UL;
                next = (u8 *)realloc(candidate, next_capacity);
                if (next == NULL) { if (candidate) free(candidate); fclose(file); status_set("NOT ENOUGH MEMORY; CURRENT DOCUMENT KEPT", 1); return 0; }
                candidate = next; capacity = next_capacity;
            }
            candidate[bytes++] = (u8)c;
        }
        if (candidate == NULL) {
            candidate = (u8 *)malloc(1);
            if (candidate == NULL) { fclose(file); status_set("NOT ENOUGH MEMORY", 1); return 0; }
        }
        raw_bytes = bytes;
        if (raw_bytes >= 3 && candidate[0] == 0xef && candidate[1] == 0xbb && candidate[2] == 0xbf) {
            memmove(candidate, candidate + 3, raw_bytes - 3);
            raw_bytes -= 3;
        }
        if (utf8_valid(candidate, raw_bytes)) {
            u32 source = 0;
            u32 target = 0;
            while (source < raw_bytes) {
                u8 value = candidate[source++];
                if (value == '\r') {
                    if (source < raw_bytes && candidate[source] == '\n') ++source;
                    candidate[target++] = '\n';
                } else candidate[target++] = value;
            }
            bytes = target;
            candidate[bytes] = 0;
        } else {
            u32 source = 0;
            u32 target = 0;
            u32 maximum;
            if (raw_bytes > 0x7fffffffUL / 3) { free(candidate); fclose(file); status_set("TEXT FILE TOO LARGE TO IMPORT", 1); return 0; }
            maximum = raw_bytes * 3 + 1;
            converted = (u8 *)malloc(maximum);
            if (converted == NULL) { free(candidate); fclose(file); status_set("NOT ENOUGH MEMORY; CURRENT DOCUMENT KEPT", 1); return 0; }
            while (source < raw_bytes) {
                u8 value = candidate[source++];
                u8 encoded[4];
                u32 count;
                if (value == '\r') {
                    if (source < raw_bytes && candidate[source] == '\n') ++source;
                    converted[target++] = '\n';
                } else {
                    count = encode_utf8(cp437_unicode(value), encoded);
                    memcpy(converted + target, encoded, count);
                    target += count;
                }
            }
            free(candidate);
            candidate = converted;
            bytes = target;
            capacity = maximum;
            candidate[bytes] = 0;
        }
    }
    fclose(file);
    file = NULL;
candidate_ready:
    if (is_native && !utf8_valid(candidate, bytes)) { free(candidate); status_set("INVALID UTF-8", 1); return 0; }
    if (!doc_reserve(bytes + 1)) {
        free(candidate);
        if (candidate_style != NULL) free(candidate_style);
        status_set("NOT ENOUGH MEMORY; CURRENT DOCUMENT KEPT", 1);
        return 0;
    }
    if (bytes != 0) memcpy(g_text, candidate, bytes);
    g_text[bytes] = 0;
    if (candidate_style != NULL && bytes != 0) memcpy(g_char_style, candidate_style, bytes);
    else if (bytes != 0) memset(g_char_style, 0, bytes);
    g_char_style[bytes] = 0;
    if (candidate_style != NULL) free(candidate_style);
    g_insert_style = 0;
    g_insert_alignment = PARA_ALIGN_LEFT;
    free(candidate);
    g_text_bytes = bytes;
    g_cursor = 0;
    g_anchor = 0;
    g_selecting = 0;
    g_dirty = 0;
    history_clear();
    if (is_docx) {
        strncpy(g_path, path, sizeof(g_path) - 1);
        g_path[sizeof(g_path) - 1] = 0;
        status_set("DOCX DOCUMENT OPENED", 0);
        log_line("DOCX_OPEN_PASS");
    } else {
        g_path[0] = 0;
        status_set(is_native ? "PNT IMPORTED; SAVE AS DOCX" : "TEXT IMPORTED; SAVE AS DOCX", 0);
    }
    return 1;
}

static void prompt_input(const char *title, char *value, unsigned capacity)
{
    unsigned length = (unsigned)strlen(value);
    int key;
    strncpy(g_prompt_title, title, sizeof(g_prompt_title) - 1);
    g_prompt_title[sizeof(g_prompt_title) - 1] = 0;
    strncpy(g_prompt_value, value, sizeof(g_prompt_value) - 1);
    g_prompt_value[sizeof(g_prompt_value) - 1] = 0;
    g_prompt_active = 1;
    while (1) {
        draw_ui();
        key = getch();
        if (key == 27) { value[0] = 0; break; }
        if (key == 13) break;
        if (key == 8 && length > 0) {
            value[--length] = 0;
            strcpy(g_prompt_value, value);
            continue;
        }
        if (key >= 32 && key <= 126 && length + 1 < capacity) {
            value[length++] = (char)key;
            value[length] = 0;
            if (length < sizeof(g_prompt_value)) strcpy(g_prompt_value, value);
        }
    }
    g_prompt_active = 0;
}

static int selection_range(u32 *start, u32 *bytes)
{
    if (!g_selecting || g_cursor == g_anchor) return 0;
    *start = g_cursor < g_anchor ? g_cursor : g_anchor;
    *bytes = (g_cursor < g_anchor ? g_anchor : g_cursor) - *start;
    return 1;
}

static void delete_selection(void)
{
    u32 start;
    u32 bytes;
    if (selection_range(&start, &bytes)) doc_splice(start, bytes, NULL, 0, 0);
}

static void undo_edit(void)
{
    Edit *edit;
    if (g_history_cursor == 0) { status_set("NOTHING TO UNDO", 0); return; }
    edit = &g_history[g_history_cursor - 1];
    if (edit->kind == EDIT_STYLE_RANGE) {
        memcpy(g_char_style + edit->position, edit->removed_style, edit->removed_bytes);
    } else if (edit->kind == EDIT_INSERT_STYLE) {
        g_insert_style = edit->old_insert_style;
    } else if (edit->kind == EDIT_INSERT_ALIGNMENT) {
        g_insert_alignment = edit->old_insert_style;
    } else {
        doc_splice_raw(edit->position, edit->inserted_bytes, edit->removed,
                       edit->removed_style, edit->removed_bytes);
        g_cursor = edit->position + edit->removed_bytes;
        g_anchor = g_cursor;
        g_selecting = 0;
    }
    --g_history_cursor;
    if (edit->kind != EDIT_INSERT_STYLE && edit->kind != EDIT_INSERT_ALIGNMENT) g_dirty = 1;
    status_set("UNDO", 0);
}

static void redo_edit(void)
{
    Edit *edit;
    if (g_history_cursor >= g_history_count) { status_set("NOTHING TO REDO", 0); return; }
    edit = &g_history[g_history_cursor];
    if (edit->kind == EDIT_STYLE_RANGE) {
        memcpy(g_char_style + edit->position, edit->inserted_style, edit->inserted_bytes);
    } else if (edit->kind == EDIT_INSERT_STYLE) {
        g_insert_style = edit->new_insert_style;
    } else if (edit->kind == EDIT_INSERT_ALIGNMENT) {
        g_insert_alignment = edit->new_insert_style;
    } else {
        doc_splice_raw(edit->position, edit->removed_bytes, edit->inserted,
                       edit->inserted_style, edit->inserted_bytes);
        g_cursor = edit->position + edit->inserted_bytes;
        g_anchor = g_cursor;
        g_selecting = 0;
    }
    ++g_history_cursor;
    if (edit->kind != EDIT_INSERT_STYLE && edit->kind != EDIT_INSERT_ALIGNMENT) g_dirty = 1;
    status_set("REDO", 0);
}

static void insert_bytes(const u8 *bytes, u32 length, int groupable)
{
    u32 start;
    u32 selected;
    if (selection_range(&start, &selected)) {
        if (!doc_splice(start, selected, bytes, length, 0)) status_set("EDIT FAILED; MEMORY OR HISTORY FULL", 1);
    } else if (!doc_splice(g_cursor, 0, bytes, length, groupable)) {
        status_set("EDIT FAILED; MEMORY OR HISTORY FULL", 1);
    }
}

static void find_text(void)
{
    char query[64];
    u32 start;
    unsigned i;
    query[0] = 0;
    prompt_input("FIND TEXT; ENTER TO SEARCH", query, sizeof(query));
    if (query[0] == 0) return;
    start = g_cursor;
    for (i = 0; i < 2; ++i) {
        u32 p = i == 0 ? start : 0;
        while (p + strlen(query) <= g_text_bytes) {
            if (memcmp(g_text + p, query, strlen(query)) == 0) {
                g_anchor = p;
                g_cursor = p + (u32)strlen(query);
                g_selecting = 1;
                status_set("MATCH FOUND", 0);
                return;
            }
            ++p;
            while (p < g_text_bytes && (g_text[p] & 0xc0) == 0x80) ++p;
        }
    }
    status_set("TEXT NOT FOUND", 1);
}

static void replace_text(void)
{
    char query[64];
    char replacement[64];
    u32 start;
    u32 count = 0;
    query[0] = replacement[0] = 0;
    prompt_input("FIND TEXT; ENTER TO REPLACE", query, sizeof(query));
    if (query[0] == 0) return;
    prompt_input("REPLACE WITH; ENTER TO APPLY", replacement, sizeof(replacement));
    start = 0;
    while (start + strlen(query) <= g_text_bytes) {
        if (memcmp(g_text + start, query, strlen(query)) == 0) {
            if (!doc_splice(start, (u32)strlen(query), (const u8 *)replacement,
                            (u32)strlen(replacement), 0)) {
                status_set("REPLACE STOPPED; MEMORY OR HISTORY FULL", 1); return;
            }
            start += (u32)strlen(replacement);
            ++count;
        } else {
            ++start;
            while (start < g_text_bytes && (g_text[start] & 0xc0) == 0x80) ++start;
        }
    }
    sprintf(g_status, "REPLACED %lu MATCHES", count);
    g_status_error = 0;
}

static void new_document(void)
{
    g_text_bytes = 0;
    g_char_style[0] = 0;
    g_insert_style = 0;
    g_insert_alignment = PARA_ALIGN_LEFT;
    g_cursor = g_anchor = 0;
    g_selecting = 0;
    g_path[0] = 0;
    g_dirty = 0;
    history_clear();
    status_set("NEW DOCUMENT", 0);
}

static int save_as(void)
{
    char path[MAX_PATH_BYTES];
    if (g_path[0] == 0) strcpy(path, "UNTITLED.DOCX");
    else strncpy(path, g_path, sizeof(path) - 1);
    path[sizeof(path) - 1] = 0;
    prompt_input("SAVE DOCX; LONG PATH; ENTER TO SAVE", path, sizeof(path));
    if (path[0] != 0) return save_docx(path);
    return 0;
}

static int save_document(void)
{
    if (g_path[0] == 0) return save_as();
    return save_docx(g_path);
}

static void open_document(void)
{
    char path[MAX_PATH_BYTES];
    path[0] = 0;
    prompt_input("OPEN DOCX, PNT OR TEXT; PATH", path, sizeof(path));
    if (path[0] != 0) load_document(path);
}

static void export_text(void)
{
    char path[MAX_PATH_BYTES];
    path[0] = 0;
    prompt_input("EXPORT UTF-8 TEXT; 8.3 PATH", path, sizeof(path));
    if (path[0] == 0) return;
    if (write_plain_text(path)) status_set("TEXT EXPORTED", 0);
    else status_set("TEXT EXPORT FAILED; ORIGINAL KEPT", 1);
}

static void move_cursor(int key)
{
    u32 row;
    u32 col;
    u32 target;
    int shift;
    union REGS in_regs;
    union REGS out_regs;
    struct SREGS seg_regs;
    memset(&in_regs, 0, sizeof(in_regs));
    memset(&out_regs, 0, sizeof(out_regs));
    segread(&seg_regs);
    in_regs.w.ax = 0x0200;
    int386x(0x16, &in_regs, &out_regs, &seg_regs);
    shift = (out_regs.h.al & 3) != 0;
    if (!shift) { g_anchor = g_cursor; g_selecting = 0; }
    else if (!g_selecting) { g_anchor = g_cursor; g_selecting = 1; }
    if (key == 75) g_cursor = previous_boundary(g_cursor);
    else if (key == 77 && g_cursor < g_text_bytes) {
        u32 cp; g_cursor += utf8_step(g_text, g_text_bytes, g_cursor, &cp);
    } else if (key == 71) {
        visual_position(g_cursor, &row, &col); g_cursor = offset_at_visual(row, 0);
    } else if (key == 79) {
        visual_position(g_cursor, &row, &col); target = offset_at_visual(row + 1, 0);
        if (target <= g_cursor || target > g_text_bytes) target = g_text_bytes;
        else { while (target < g_text_bytes && g_text[target] != '\n' && target < g_text_bytes) ++target; }
        g_cursor = target;
    } else if (key == 72 || key == 80) {
        visual_position(g_cursor, &row, &col);
        if (key == 72 && row > 0) g_cursor = offset_at_visual(row - 1, col);
        else if (key == 80) g_cursor = offset_at_visual(row + 1, col);
    } else if (key == 83) {
        u32 cp;
        if (selection_range(&target, &row)) doc_splice(target, row, NULL, 0, 0);
        else if (g_cursor < g_text_bytes) doc_splice(g_cursor, utf8_step(g_text, g_text_bytes, g_cursor, &cp), NULL, 0, 0);
    } else if (key == 73 || key == 81) {
        if (key == 73 && row > DOC_ROWS) row -= DOC_ROWS;
        else if (key == 81) row += DOC_ROWS;
        else row = 0;
        g_cursor = offset_at_visual(row, col);
    }
    if (g_selecting && g_cursor == g_anchor) g_selecting = 0;
}

static int video_start(void)
{
    if (pw_platform_video_start()) return 1;
    log_line(pw_platform_video_error());
    return 0;
}

static void video_stop(void)
{
    pw_platform_video_stop();
}

static int clipboard_copy(int cut)
{
    u32 start;
    u32 bytes;
    u8 *next;
    if (!selection_range(&start, &bytes)) return 0;
    next = (u8 *)realloc(g_clipboard, bytes == 0 ? 1 : bytes);
    if (next == NULL) { status_set("CLIPBOARD MEMORY FAILED", 1); return 0; }
    g_clipboard = next;
    memcpy(g_clipboard, g_text + start, bytes);
    g_clipboard_bytes = bytes;
    if (cut) doc_splice(start, bytes, NULL, 0, 0);
    else status_set("COPIED TO NOTARY CLIPBOARD", 0);
    return 1;
}

static int shortcut_pressed(int key, int control_code, int ascii_key,
                            unsigned char keyboard_flags)
{
    if (key == control_code) return 1;
    return (keyboard_flags & 0x04) != 0 &&
           (key == ascii_key || key == ascii_key - ('a' - 'A'));
}

static void request_exit(void)
{
    if (g_dirty) g_exit_prompt = 1;
    else g_running = 0;
}

static int menu_handle_key(int key)
{
    int i;
    if (g_menu_open == MENU_NONE) return 0;
    if (key == 68) {
        menu_close();
        return 1;
    }
    if (key == 72) { menu_move_selection(-1); return 1; }
    if (key == 80) { menu_move_selection(1); return 1; }
    if (key == 75 || key == 77) {
        menu_open(g_menu_open == MENU_FILE ? MENU_EDIT : MENU_FILE);
        return 1;
    }
    if (key == 13) { menu_activate_selection(); return 1; }
    if (key == 27) { menu_close(); return 1; }
    if (key >= 'a' && key <= 'z') key -= 'a' - 'A';
    if (key >= 'A' && key <= 'Z') {
        for (i = 0; i < menu_item_count(g_menu_open); ++i) {
            const MenuItem *item = menu_item_at(g_menu_open, i);
            if (item != NULL && item->label[0] == key) {
                g_menu_selection = i;
                return 1;
            }
        }
    }
    return 1;
}

static void handle_key(int key)
{
    u8 encoded[4];
    u32 bytes;
    union REGS flags_in;
    union REGS flags_out;
    struct SREGS flags_seg;
    unsigned char keyboard_flags;
    memset(&flags_in, 0, sizeof(flags_in));
    memset(&flags_out, 0, sizeof(flags_out));
    segread(&flags_seg);
    flags_in.w.ax = 0x0200;
    int386x(0x16, &flags_in, &flags_out, &flags_seg);
    keyboard_flags = flags_out.h.al;
    if (g_exit_prompt) {
        if (key == 'y' || key == 'Y') { if (save_document()) g_running = 0; g_exit_prompt = 0; }
        else if (key == 'n' || key == 'N') { g_running = 0; }
        else if (key == 27) g_exit_prompt = 0;
        return;
    }
    if (key == 0 || key == 0xe0) {
        key = getch();
        if (keyboard_flags & 8) {
            if (key == 33) { menu_open(MENU_FILE); return; }
            if (key == 18) { menu_open(MENU_EDIT); return; }
            if (key == 62) { menu_close(); request_exit(); return; }
        }
        if (key == 60) { menu_close(); save_as(); return; }
        if (g_menu_open != MENU_NONE) { menu_handle_key(key); return; }
        if (key == 68) { menu_open(MENU_FILE); return; }
        move_cursor(key);
        return;
    }
    if ((keyboard_flags & 8) && (key == 'f' || key == 'F')) { menu_open(MENU_FILE); return; }
    if ((keyboard_flags & 8) && (key == 'e' || key == 'E')) { menu_open(MENU_EDIT); return; }
    if (g_menu_open != MENU_NONE) {
        if (key == 27) { menu_close(); return; }
        if (key == 13) { menu_activate_selection(); return; }
        if (keyboard_flags & 4) menu_close();
        else { menu_handle_key(key); return; }
    }
    if (key == 27) {
        request_exit();
        return;
    }
    if (shortcut_pressed(key, 19, 's', keyboard_flags)) {
        if (keyboard_flags & 3) save_as(); else save_document();
        return;
    }
    if (shortcut_pressed(key, 2, 'b', keyboard_flags)) { toggle_character_format(CHAR_BOLD); return; }
    if (shortcut_pressed(key, 9, 'i', keyboard_flags)) { toggle_character_format(CHAR_ITALIC); return; }
    if (shortcut_pressed(key, 21, 'u', keyboard_flags)) { toggle_character_format(CHAR_UNDERLINE); return; }
    if (shortcut_pressed(key, 12, 'l', keyboard_flags)) { apply_paragraph_alignment(PARA_ALIGN_LEFT); return; }
    if (shortcut_pressed(key, 5, 'e', keyboard_flags)) { apply_paragraph_alignment(PARA_ALIGN_CENTER); return; }
    if (shortcut_pressed(key, 18, 'r', keyboard_flags)) { apply_paragraph_alignment(PARA_ALIGN_RIGHT); return; }
    if (shortcut_pressed(key, 10, 'j', keyboard_flags)) { apply_paragraph_alignment(PARA_ALIGN_JUSTIFY); return; }
    if (shortcut_pressed(key, 2, 'b', keyboard_flags)) { toggle_character_format(CHAR_BOLD); return; }
    if (shortcut_pressed(key, 9, 'i', keyboard_flags)) { toggle_character_format(CHAR_ITALIC); return; }
    if (shortcut_pressed(key, 21, 'u', keyboard_flags)) { toggle_character_format(CHAR_UNDERLINE); return; }
    if (shortcut_pressed(key, 15, 'o', keyboard_flags)) { open_document(); return; }
    if (shortcut_pressed(key, 6, 'f', keyboard_flags)) { find_text(); return; }
    if (shortcut_pressed(key, 8, 'h', keyboard_flags)) { replace_text(); return; }
    if (shortcut_pressed(key, 26, 'z', keyboard_flags)) { undo_edit(); return; }
    if (shortcut_pressed(key, 25, 'y', keyboard_flags)) { redo_edit(); return; }
    if (shortcut_pressed(key, 1, 'a', keyboard_flags)) { g_anchor = 0; g_cursor = g_text_bytes; g_selecting = 1; return; }
    if (shortcut_pressed(key, 3, 'c', keyboard_flags)) { clipboard_copy(0); return; }
    if (shortcut_pressed(key, 24, 'x', keyboard_flags)) { clipboard_copy(1); return; }
    if (shortcut_pressed(key, 22, 'v', keyboard_flags)) { if (g_clipboard_bytes) insert_bytes(g_clipboard, g_clipboard_bytes, 0); return; }
    if (shortcut_pressed(key, 14, 'n', keyboard_flags)) {
        if (g_dirty) { status_set("SAVE OR DISCARD CHANGES BEFORE NEW DOCUMENT", 1); return; }
        new_document(); return;
    }
    if (shortcut_pressed(key, 5, 'e', keyboard_flags)) { export_text(); return; }
    if (key == 9) { encoded[0] = ' '; encoded[1] = ' '; encoded[2] = ' '; encoded[3] = ' '; insert_bytes(encoded, 4, 0); return; }
    if (key == 8) {
        if (g_selecting) delete_selection();
        else if (g_cursor > 0) doc_splice(previous_boundary(g_cursor), g_cursor - previous_boundary(g_cursor), NULL, 0, 0);
        return;
    }
    if (key == 13) { encoded[0] = '\n'; insert_bytes(encoded, 1, 0); return; }
    if (key >= 32 && key <= 255) {
        bytes = encode_utf8((u32)cp437_unicode((u8)key), encoded);
        insert_bytes(encoded, bytes, bytes == 1);
    }
}

static void invoke_toolbar_action(int action)
{
    switch (action) {
        case 0:
            if (g_dirty) status_set("SAVE OR DISCARD CHANGES BEFORE NEW DOCUMENT", 1);
            else new_document();
            break;
        case 1: open_document(); break;
        case 2: save_document(); break;
        case 3: undo_edit(); break;
        case 4: redo_edit(); break;
        case 5: find_text(); break;
        case 6: replace_text(); break;
        case 7: save_as(); break;
        case 8: request_exit(); break;
    }
}

static void invoke_format_action(int action)
{
    if (action == 0) toggle_character_format(CHAR_BOLD);
    else if (action == 1) toggle_character_format(CHAR_ITALIC);
    else if (action == 2) toggle_character_format(CHAR_UNDERLINE);
    else if (action >= 3 && action <= 6) apply_paragraph_alignment((u8)((action - 3) * 8));
    else if (action == 7) apply_list_style(CHAR_BULLETED);
    else if (action == 8) apply_list_style(CHAR_NUMBERED);
    else status_set(format_label(action), 0);
}

static int document_position_from_mouse(int x, int y, u32 *position)
{
    u32 current_row;
    u32 current_column;
    u32 top;
    u32 target_row;
    u32 target_column;
    if (x < DOC_X || x >= DOC_X + DOC_COLUMNS * CELL_W ||
        y < DOC_Y || y >= DOC_Y + DOC_ROWS * CELL_H) return 0;
    visual_position(g_cursor, &current_row, &current_column);
    top = current_row >= DOC_ROWS ? current_row - DOC_ROWS + 1 : 0;
    target_row = top + (u32)((y - DOC_Y) / CELL_H);
    target_column = (u32)((x - DOC_X) / CELL_W);
    {
        int offset = visual_row_offset(target_row);
        if ((int)target_column < offset) target_column = 0;
        else target_column -= (u32)offset;
    }
    *position = offset_at_visual(target_row, target_column);
    return 1;
}

static int poll_mouse(void)
{
    int x;
    int y;
    int moved;
    int hover_changed;
    int buttons_changed;
    int old_menu = g_menu_open;
    int old_menu_selection = g_menu_selection;
    int menu_header;
    int menu_row;
    int old_x = g_mouse_x;
    int old_y = g_mouse_y;
    int old_hover = g_mouse_hover;
    int old_format_hover = g_format_hover;
    unsigned old_buttons = g_mouse_buttons;
    unsigned buttons;
    u32 position;
    int action;
    if (!pw_platform_mouse_read_state(&x, &y, &buttons)) return 0;
    moved = x != old_x || y != old_y;
    g_mouse_x = x;
    g_mouse_y = y;
    g_mouse_buttons = buttons;
    if (moved) {
        char mouse_event[48];
        sprintf(mouse_event, "MOUSE_STATE %d,%d B%u", x, y, buttons);
        log_line(mouse_event);
    }
    menu_header = menu_bar_hit(x, y);
    if (g_menu_open != MENU_NONE && moved && menu_header != MENU_NONE && menu_header != g_menu_open)
        menu_open(menu_header);
    menu_row = menu_item_hit(x, y);
    if (g_menu_open != MENU_NONE && menu_row >= 0) g_menu_selection = menu_row;
    g_mouse_hover = toolbar_hit(x, y);
    g_format_hover = format_toolbar_hit(x, y);
    hover_changed = g_mouse_hover != old_hover;
    buttons_changed = buttons != old_buttons;

    if ((buttons & 1) && !(old_buttons & 1)) {
        if (menu_header != MENU_NONE) {
            if (old_menu == menu_header) menu_close();
            else menu_open(menu_header);
            g_mouse_selecting = 0;
        } else if (old_menu != MENU_NONE) {
            menu_row = menu_item_hit(x, y);
            if (menu_row >= 0) {
                g_menu_selection = menu_row;
                menu_activate_selection();
            } else menu_close();
            g_mouse_selecting = 0;
        } else {
            action = toolbar_hit(x, y);
            if (action >= 0) {
                g_mouse_selecting = 0;
                invoke_toolbar_action(action);
            } else if (g_format_hover >= 0) {
                g_mouse_selecting = 0;
                {
                    char format_click[40];
                    sprintf(format_click, "FORMAT_BUTTON_CLICK %d", g_format_hover);
                    log_line(format_click);
                }
                invoke_format_action(g_format_hover);
            } else if (document_position_from_mouse(x, y, &position)) {
                g_anchor = position;
                g_cursor = position;
                g_selecting = 0;
                g_mouse_selecting = 1;
            } else {
                g_mouse_selecting = 0;
            }
        }
    } else if (g_menu_open == MENU_NONE && (buttons & 1) && g_mouse_selecting && moved &&
               document_position_from_mouse(x, y, &position)) {
        g_cursor = position;
        g_selecting = g_cursor != g_anchor;
    }
    if (!(buttons & 1)) g_mouse_selecting = 0;
    return moved || hover_changed || buttons_changed ||
           g_format_hover != old_format_hover ||
           g_menu_open != old_menu || g_menu_selection != old_menu_selection;
}

static int initialize_document(void)
{
    g_text = (u8 *)malloc(4096);
    g_char_style = (u8 *)malloc(4096);
    if (g_text == NULL || g_char_style == NULL) {
        if (g_text != NULL) free(g_text);
        if (g_char_style != NULL) free(g_char_style);
        g_text = NULL;
        g_char_style = NULL;
        return 0;
    }
    g_text_capacity = 4096;
    g_text_bytes = 0;
    g_text[0] = 0;
    g_char_style[0] = 0;
    g_path[0] = 0;
    g_status[0] = 0;
    g_status_error = 0;
    return 1;
}

int main(int argc, char **argv)
{
    int key;
    int changed;
    const char *lfn_ready;
    g_log = fopen(g_log_path, "wb");
    log_line("NOTARY START");
    lfn_ready = getenv("PANTHERA_LFN_READY");
    g_lfn_driver_ready = (lfn_ready != NULL && strcmp(lfn_ready, "1") == 0);
    log_line(g_lfn_driver_ready ? "LFN_BOOTSTRAP_PASS" : "LFN_BOOTSTRAP_MISSING");
    if (!initialize_document()) { log_line("DOCUMENT_ALLOC_FAIL"); return 2; }
    pw_platform_font_init_fallback(g_font);
    if (pw_platform_font_load_bios(g_font)) log_line("BIOS_FONT_PASS");
    else log_line("BIOS_FONT_FALLBACK");
    g_mouse_available = pw_platform_mouse_initialize();
    log_line(g_mouse_available ? "MOUSE_INT33_PASS" : "MOUSE_INT33_UNAVAILABLE");
    if (!video_start()) {
        log_line("VIDEO_INIT_FAIL");
        pw_platform_mouse_restore();
        if (g_log) fclose(g_log);
        return 3;
    }
    log_line("VBE_640x480x8_LFB_PASS");
    if (argc > 1 && argv[1] != NULL && argv[1][0] != 0) load_document(argv[1]);
    else if (!g_lfn_driver_ready) status_set("RUN NOTARY.BAT TO ENABLE DOCX NAMES", 1);
    else if (g_mouse_available) status_set("MOUSE READY; DOCX LFN READY", 0);
    else status_set("READY - CTRL+S OR F2 SAVES AS; ESC EXITS", 0);
    draw_ui();
    while (g_running) {
        changed = poll_mouse();
        if (kbhit()) {
            key = getch();
            handle_key(key);
            changed = 1;
        }
        if (changed && g_running) draw_ui();
        else if (!changed) delay(8);
    }
    video_stop();
    pw_platform_mouse_restore();
    log_line("NOTARY_EXIT_CLEAN");
    if (g_log != NULL) fclose(g_log);
    history_clear();
    if (g_clipboard != NULL) free(g_clipboard);
    if (g_text != NULL) free(g_text);
    if (g_char_style != NULL) free(g_char_style);
    return 0;
}
