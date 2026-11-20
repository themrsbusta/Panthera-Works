#include <conio.h>
#include <dos.h>
#include <i86.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "showroom_model.h"
#include "showroom_pptx.h"
#include "../../shared/platform/pw_platform.h"

#define SW PW_PLATFORM_SCREEN_WIDTH
#define SH PW_PLATFORM_SCREEN_HEIGHT
#define EDIT_BYTES 440

typedef unsigned char u8;
enum { MENU_NONE = -1, MENU_FILE, MENU_EDIT, MENU_VIEW, MENU_INSERT,
       MENU_FORMAT, MENU_SLIDE_SHOW, MENU_HELP };
static ShModel g_model;
static ShModel g_loaded_model;
static u8 g_pixels[SW * SH];
static u8 g_font[256][16];
static char g_edit[EDIT_BYTES + 1];
static unsigned g_edit_bytes;
static int g_editing;
static int g_edit_body;
static int g_mouse;
static int g_lfn_driver_ready;
static int g_recovery_ready;
static int g_dirty;
static int g_mx, g_my;
static unsigned g_buttons;
static int g_toolbar_hover = -1;
static int g_menu_open = MENU_NONE;
static int g_menu_selection;
static int g_running = 1;
static char g_status[80] = "F2 TITLE  F3 BODY  F5/S SAVE PPTX  INSERT SLIDE";
static const int g_toolbar_x[6] = {28, 54, 80, 112, 138, 164};
static const int g_toolbar_icon[6] = {
    PW_PLATFORM_ICON_ADD_SLIDE, PW_PLATFORM_ICON_DELETE,
    PW_PLATFORM_ICON_PREVIOUS, PW_PLATFORM_ICON_NEXT, PW_PLATFORM_ICON_SAVE,
    PW_PLATFORM_ICON_PRESENT
};
static const char *g_toolbar_label[6] = {"NEW SLIDE", "DELETE SLIDE", "PREVIOUS SLIDE",
    "NEXT SLIDE", "SAVE PRESENTATION", "PRESENTATION MODE (IN DEVELOPMENT)"};
static const int g_menu_x[7] = {8, 56, 104, 152, 210, 270, 368};
static const char *g_menu_names[7] = {"File", "Edit", "View", "Insert", "Format", "Slide Show", "Help"};

static void commit_edit(void);
static void start_edit(int body, int replace);
static void toolbar_action(int index);
static void save_presentation(void);
static void open_presentation(void);

static void text(const char *s, int x, int y, u8 color)
{ pw_platform_canvas_text(g_pixels, g_font, s, x, y, color); }

static void text_fit(const char *s, int x, int y, u8 color, unsigned maximum)
{
    char clipped[64];
    unsigned length;
    if (s == NULL || maximum == 0) return;
    if (maximum > sizeof(clipped) - 1) maximum = sizeof(clipped) - 1;
    length = (unsigned)strlen(s);
    if (length <= maximum) { text(s, x, y, color); return; }
    if (maximum > 3) {
        memcpy(clipped, s, maximum - 3);
        clipped[maximum - 3] = '.';
        clipped[maximum - 2] = '.';
        clipped[maximum - 1] = '.';
        clipped[maximum] = '\0';
    } else {
        memcpy(clipped, s, maximum);
        clipped[maximum] = '\0';
    }
    text(clipped, x, y, color);
}

static void rect(int x, int y, int w, int h, u8 c)
{ pw_platform_canvas_fill_rect(g_pixels, x, y, w, h, c); }

static void frame(int x, int y, int w, int h, u8 c)
{
    pw_platform_canvas_line(g_pixels, x, y, x + w - 1, y, c);
    pw_platform_canvas_line(g_pixels, x, y, x, y + h - 1, c);
    pw_platform_canvas_line(g_pixels, x + w - 1, y, x + w - 1, y + h - 1, c);
    pw_platform_canvas_line(g_pixels, x, y + h - 1, x + w - 1, y + h - 1, c);
}

static int menu_from_x(int x)
{
    static const int right[7] = {48, 96, 144, 204, 264, 354, 416};
    int i;
    for (i = 0; i < 7; ++i)
        if (x >= g_menu_x[i] && x < right[i]) return i;
    return MENU_NONE;
}

static int menu_count(int menu)
{
    if (menu == MENU_FILE) return 5;
    if (menu == MENU_EDIT) return 3;
    if (menu == MENU_INSERT) return 3;
    if (menu == MENU_HELP) return 1;
    return 2;
}

static const char *menu_item(int menu, int index)
{
    static const char *file[] = {"New Slide", "Open SHOWROOM.PPTX", "Save Presentation", "Save As...", "Exit"};
    static const char *edit[] = {"Undo", "Redo", "Delete Slide"};
    static const char *view[] = {"Fit Slide", "Show Rulers"};
    static const char *insert[] = {"New Slide", "Title Text...", "Body Text..."};
    static const char *format[] = {"Font...", "Paragraph..."};
    static const char *show[] = {"View Show", "Set Up Show..."};
    if (menu == MENU_FILE) return file[index];
    if (menu == MENU_EDIT) return edit[index];
    if (menu == MENU_VIEW) return view[index];
    if (menu == MENU_INSERT) return insert[index];
    if (menu == MENU_FORMAT) return format[index];
    if (menu == MENU_SLIDE_SHOW) return show[index];
    return "About Panthera Showroom";
}

static const char *menu_shortcut(int menu, int index)
{
    if (menu == MENU_FILE && index == 0) return "Ins";
    if (menu == MENU_FILE && index == 2) return "Ctrl+S";
    if (menu == MENU_FILE && index == 4) return "Alt+F4";
    if (menu == MENU_INSERT && index == 0) return "Ins";
    if (menu == MENU_INSERT && index == 1) return "F2";
    if (menu == MENU_INSERT && index == 2) return "F3";
    return "";
}

static void open_menu(int menu)
{
    g_menu_open = menu;
    g_menu_selection = 0;
}

static void close_menu(void)
{
    g_menu_open = MENU_NONE;
    g_menu_selection = 0;
}

static void activate_menu(void)
{
    int menu = g_menu_open;
    int item = g_menu_selection;
    close_menu();
    if (g_editing) commit_edit();
    if (menu == MENU_FILE && item == 0) toolbar_action(0);
    else if (menu == MENU_FILE && item == 1) open_presentation();
    else if (menu == MENU_FILE && item == 2) save_presentation();
    else if (menu == MENU_FILE && item == 3) strcpy(g_status, "SAVE AS IS STILL IN DEVELOPMENT");
    else if (menu == MENU_FILE && item == 4) g_running = 0;
    else if (menu == MENU_EDIT && item == 2) toolbar_action(1);
    else if (menu == MENU_INSERT && item == 0) toolbar_action(0);
    else if (menu == MENU_INSERT && item == 1) start_edit(0, 0);
    else if (menu == MENU_INSERT && item == 2) start_edit(1, 0);
    else if (menu == MENU_HELP) strcpy(g_status, "PANTHERA SHOWROOM - PRESENTATION EDITOR");
    else strcpy(g_status, "THIS PRESENTATION COMMAND IS STILL IN DEVELOPMENT");
}

static int toolbar_hit(int x, int y)
{
    int i;
    if (x >= 0 && x < 27 && y >= 110 && y < 454) return 8;
    if (y >= 65 && y < 91) {
        if (x >= 460 && x < 484) return 6;
        if (x >= 486 && x < 510) return 7;
        if ((x >= 8 && x < 445) || (x >= 518 && x < 598)) return 9;
        return -1;
    }
    if (y < 39 || y >= 65) return -1;
    for (i = 0; i < 6; ++i)
        if (x >= g_toolbar_x[i] && x < g_toolbar_x[i] + 22) return i;
    if (x >= 194 && x < 615) return 10;
    return -1;
}

static void toolbar_action(int index)
{
    if (g_editing) commit_edit();
    if (index == 0) {
        if (sh_model_insert(&g_model, g_model.current + 1)) { g_dirty = 1; strcpy(g_status, "NEW SLIDE ADDED"); }
        else strcpy(g_status, "SLIDE LIMIT REACHED");
    } else if (index == 1) {
        if (sh_model_remove(&g_model, g_model.current)) { g_dirty = 1; strcpy(g_status, "SLIDE DELETED"); }
        else strcpy(g_status, "KEEP AT LEAST ONE SLIDE");
    } else if (index == 2) {
        if (g_model.current) { --g_model.current; strcpy(g_status, "PREVIOUS SLIDE"); }
        else strcpy(g_status, "FIRST SLIDE");
    } else if (index == 3) {
        if (g_model.current + 1 < g_model.count) { ++g_model.current; strcpy(g_status, "NEXT SLIDE"); }
        else strcpy(g_status, "LAST SLIDE");
    } else if (index == 4) save_presentation();
    else if (index == 5) strcpy(g_status, "PRESENTATION MODE IS STILL IN DEVELOPMENT");
    else if (index == 6) start_edit(0, 0);
    else if (index == 7) start_edit(1, 0);
    else if (index == 8) strcpy(g_status, "DRAWING TOOLS ARE STILL IN DEVELOPMENT");
    else if (index == 9) strcpy(g_status, "TEXT FORMATTING IS STILL IN DEVELOPMENT");
    else if (index == 10) strcpy(g_status, "OPEN, CLIPBOARD AND REVIEW COMMANDS ARE IN DEVELOPMENT");
}

static void draw_toolbar(void)
{
    int i;
    static const int extra_x[13] = {194, 220, 246, 280, 306, 332, 358,
                                    384, 410, 436, 540, 566, 592};
    static const int extra_icon[13] = {PW_PLATFORM_ICON_OPEN, PW_PLATFORM_ICON_FIND,
        PW_PLATFORM_ICON_REPLACE, PW_PLATFORM_ICON_UNDO, PW_PLATFORM_ICON_REDO,
        PW_PLATFORM_ICON_FUNCTION, PW_PLATFORM_ICON_TITLE, PW_PLATFORM_ICON_BODY,
        PW_PLATFORM_ICON_FIND, PW_PLATFORM_ICON_REPLACE, PW_PLATFORM_ICON_PRESENT,
        PW_PLATFORM_ICON_FUNCTION, PW_PLATFORM_ICON_FIND};
    for (i = 0; i < 6; ++i) {
        int x = g_toolbar_x[i];
        u8 face = g_toolbar_hover == i ? 9 : 7;
        u8 ink = i == 5 || (i == 4 && !g_lfn_driver_ready) ? 8 : 0;
        u8 accent = ink == 8 ? 8 : 1;
        rect(x, 42, 22, 22, face);
        pw_platform_canvas_line(g_pixels, x, 42, x + 21, 42, 15);
        pw_platform_canvas_line(g_pixels, x, 42, x, 63, 15);
        pw_platform_canvas_line(g_pixels, x + 21, 42, x + 21, 63, 8);
        pw_platform_canvas_line(g_pixels, x, 63, x + 21, 63, 8);
        pw_platform_canvas_toolbar_icon(g_pixels, g_toolbar_icon[i], x + 2, 43, ink, accent);
    }
    for (i = 0; i < 2; ++i) {
        int x = i == 0 ? 460 : 486;
        u8 face = g_toolbar_hover == 6 + i ? 9 : 7;
        rect(x, 67, 22, 22, face);
        pw_platform_canvas_line(g_pixels, x, 67, x + 21, 67, 15);
        pw_platform_canvas_line(g_pixels, x, 67, x, 88, 15);
        pw_platform_canvas_line(g_pixels, x + 21, 67, x + 21, 88, 8);
        pw_platform_canvas_line(g_pixels, x, 88, x + 21, 88, 8);
        pw_platform_canvas_toolbar_icon(g_pixels,
            i == 0 ? PW_PLATFORM_ICON_TITLE : PW_PLATFORM_ICON_BODY,
            x + 2, 68, 0, 1);
    }
    for (i = 0; i < 13; ++i) {
        int x = extra_x[i];
        rect(x, 42, 22, 22, 7);
        pw_platform_canvas_line(g_pixels, x, 42, x + 21, 42, 15);
        pw_platform_canvas_line(g_pixels, x, 42, x, 63, 15);
        pw_platform_canvas_line(g_pixels, x + 21, 42, x + 21, 63, 8);
        pw_platform_canvas_line(g_pixels, x, 63, x + 21, 63, 8);
        pw_platform_canvas_toolbar_icon(g_pixels, extra_icon[i], x + 2, 43, 0, 8);
    }
    pw_platform_canvas_line(g_pixels, 78, 44, 78, 61, 8);
    pw_platform_canvas_line(g_pixels, 108, 44, 108, 61, 8);
    pw_platform_canvas_line(g_pixels, 136, 44, 136, 61, 8);
    pw_platform_canvas_line(g_pixels, 190, 44, 190, 61, 8);
    pw_platform_canvas_line(g_pixels, 274, 44, 274, 61, 8);
    pw_platform_canvas_line(g_pixels, 534, 44, 534, 61, 8);
    pw_platform_canvas_line(g_pixels, 8, 90, 624, 90, 8);
}

static void draw_menu_bar(void)
{
    int i;
    rect(0, 20, SW, 20, 7);
    pw_platform_canvas_line(g_pixels, 0, 39, SW - 1, 39, 8);
    for (i = 0; i < 7; ++i) {
        int x = g_menu_x[i];
        if (g_menu_open == i) rect(x - 3, 21, (i == MENU_SLIDE_SHOW ? 72 : 52), 18, 1);
        text(g_menu_names[i], x, 26, g_menu_open == i ? 15 : 0);
    }
}

static void draw_format_bar(void)
{
    int x;
    rect(0, 65, SW, 25, 8);
    rect(8, 68, 116, 18, 7); frame(8, 68, 116, 18, 0);
    text("Arial", 13, 73, 0); text("v", 111, 73, 0);
    rect(128, 68, 45, 18, 7); frame(128, 68, 45, 18, 0);
    text("24", 135, 73, 0); text("v", 161, 73, 0);
    for (x = 178; x < 254; x += 25) {
        rect(x, 68, 22, 18, 7);
        pw_platform_canvas_line(g_pixels, x, 68, x + 21, 68, 15);
        pw_platform_canvas_line(g_pixels, x, 68, x, 85, 15);
        pw_platform_canvas_line(g_pixels, x + 21, 68, x + 21, 85, 8);
        pw_platform_canvas_line(g_pixels, x, 85, x + 21, 85, 8);
    }
    text("B", 185, 73, 0);
    text("I", 210, 73, 0);
    text("U", 235, 73, 0);
    rect(259, 68, 28, 18, 7); frame(259, 68, 28, 18, 0);
    text("A", 269, 73, 0); pw_platform_canvas_line(g_pixels, 268, 84, 278, 84, 1);
    for (x = 306; x < 388; x += 27) {
        rect(x, 68, 24, 18, 7);
        pw_platform_canvas_line(g_pixels, x, 68, x + 23, 68, 15);
        pw_platform_canvas_line(g_pixels, x, 68, x, 85, 15);
        pw_platform_canvas_line(g_pixels, x + 23, 68, x + 23, 85, 8);
        pw_platform_canvas_line(g_pixels, x, 85, x + 23, 85, 8);
    }
    text("=", 314, 73, 0); text("|||", 341, 73, 0); text("...", 368, 73, 0);
    rect(400, 68, 42, 18, 7); frame(400, 68, 42, 18, 0); text("68%", 405, 73, 0);
    pw_platform_canvas_line(g_pixels, 174, 69, 174, 86, 8);
    pw_platform_canvas_line(g_pixels, 296, 69, 296, 86, 8);
    pw_platform_canvas_line(g_pixels, 394, 69, 394, 86, 8);
}

static void draw_tool_rail(void)
{
    int i, y;
    rect(0, 110, 27, 344, 7);
    pw_platform_canvas_line(g_pixels, 26, 110, 26, 454, 8);
    for (i = 0; i < 8; ++i) {
        y = 114 + i * 30;
        rect(3, y, 21, 24, 7);
        pw_platform_canvas_line(g_pixels, 3, y, 23, y, 15);
        pw_platform_canvas_line(g_pixels, 3, y, 3, y + 23, 15);
        pw_platform_canvas_line(g_pixels, 23, y, 23, y + 23, 8);
        pw_platform_canvas_line(g_pixels, 3, y + 23, 23, y + 23, 8);
        if (i == 0) {
            pw_platform_canvas_line(g_pixels, 8, y + 5, 17, y + 18, 0);
            pw_platform_canvas_line(g_pixels, 8, y + 5, 8, y + 15, 0);
            pw_platform_canvas_line(g_pixels, 8, y + 15, 13, y + 12, 0);
        } else if (i == 1) {
            text("T", 9, y + 6, 0);
        } else if (i == 2) {
            pw_platform_canvas_line(g_pixels, 7, y + 17, 19, y + 6, 0);
        } else if (i == 3) {
            frame(7, y + 6, 13, 12, 0);
        } else if (i == 4) {
            pw_platform_canvas_line(g_pixels, 7, y + 17, 13, y + 5, 0);
            pw_platform_canvas_line(g_pixels, 13, y + 5, 19, y + 17, 0);
            pw_platform_canvas_line(g_pixels, 7, y + 17, 19, y + 17, 0);
        } else if (i == 5) {
            pw_platform_canvas_line(g_pixels, 7, y + 7, 19, y + 7, 0);
            pw_platform_canvas_line(g_pixels, 7, y + 12, 19, y + 12, 0);
            pw_platform_canvas_line(g_pixels, 7, y + 17, 19, y + 17, 0);
        } else if (i == 6) {
            pw_platform_canvas_line(g_pixels, 7, y + 18, 12, y + 6, 1);
            pw_platform_canvas_line(g_pixels, 12, y + 6, 20, y + 18, 1);
        } else {
            pw_platform_canvas_line(g_pixels, 7, y + 7, 18, y + 7, 0);
            pw_platform_canvas_line(g_pixels, 7, y + 7, 7, y + 18, 0);
            pw_platform_canvas_line(g_pixels, 7, y + 18, 18, y + 18, 0);
        }
    }
}

static void draw_rulers(void)
{
    int x, y, n;
    rect(151, 94, 476, 16, 7);
    frame(151, 94, 476, 16, 8);
    for (x = 168, n = 0; x < 620; x += 24, ++n) {
        int tick = n % 5 == 0 ? 9 : (n % 2 == 0 ? 6 : 3);
        pw_platform_canvas_line(g_pixels, x, 109 - tick, x, 108, 0);
        if (n % 5 == 0) { char label[8]; sprintf(label, "%d", n / 5 + 1); text(label, x + 2, 95, 0); }
    }
    rect(142, 110, 13, 344, 7);
    frame(142, 110, 13, 344, 8);
    for (y = 120, n = 0; y < 448; y += 24, ++n) {
        int tick = n % 5 == 0 ? 8 : 4;
        pw_platform_canvas_line(g_pixels, 154 - tick, y, 154, y, 0);
        if (n % 5 == 0) { char label[8]; sprintf(label, "%d", n / 5 + 1); text(label, 143, y + 2, 0); }
    }
}

static void draw_slide_tools(void)
{
    int i, y;
    rect(624, 94, 16, 360, 7);
    pw_platform_canvas_line(g_pixels, 624, 94, 624, 454, 8);
    for (i = 0; i < 8; ++i) {
        y = 101 + i * 34;
        rect(626, y, 12, 26, 8);
        frame(626, y, 12, 26, 0);
        if (i == 0) text("T", 629, y + 8, 0);
        else if (i == 1) text("P", 629, y + 8, 0);
        else if (i == 2) text("C", 629, y + 8, 0);
        else if (i == 3) text("A", 629, y + 8, 0);
        else text(".", 631, y + 8, 0);
    }
}

static void draw_open_menu(void)
{
    int i, x, y, width = 192;
    if (g_menu_open == MENU_NONE) return;
    x = g_menu_x[g_menu_open] - 2;
    y = 40;
    rect(x + 5, y + 5, width, 24 * menu_count(g_menu_open) + 8, 8);
    rect(x, y, width, 24 * menu_count(g_menu_open) + 4, 7);
    frame(x, y, width, 24 * menu_count(g_menu_open) + 4, 0);
    for (i = 0; i < menu_count(g_menu_open); ++i) {
        const char *shortcut = menu_shortcut(g_menu_open, i);
        if (i == g_menu_selection) rect(x + 3, y + 3 + i * 24, width - 6, 22, 1);
        text(menu_item(g_menu_open, i), x + 9, y + 8 + i * 24,
             i == g_menu_selection ? 15 : 0);
        if (shortcut[0]) text(shortcut, x + 154, y + 8 + i * 24,
                              i == g_menu_selection ? 15 : 0);
    }
}

static void start_edit(int body, int replace)
{
    const ShSlide *slide = sh_model_get(&g_model, g_model.current);
    const char *source;
    if (slide == NULL) return;
    g_edit_body = body;
    source = body ? slide->body : slide->title;
    g_edit_bytes = replace ? 0 : (unsigned)strlen(source);
    if (g_edit_bytes > EDIT_BYTES) g_edit_bytes = EDIT_BYTES;
    if (!replace) memcpy(g_edit, source, g_edit_bytes);
    g_edit[g_edit_bytes] = '\0';
    g_editing = 1;
}

static void commit_edit(void)
{
    int ok = g_edit_body ? sh_model_set_body(&g_model, g_model.current, g_edit) :
                           sh_model_set_title(&g_model, g_model.current, g_edit);
    if (ok) g_dirty = 1;
    strcpy(g_status, ok ? "SLIDE UPDATED" : "TEXT LIMIT REACHED");
    g_editing = 0;
}

static void draw_ui(void);

static void save_presentation(void)
{
    int result;
    if (!g_lfn_driver_ready) {
        strcpy(g_status, "LOAD DOSLFN BEFORE SAVING PPTX");
        draw_ui();
        return;
    }
    if (!g_recovery_ready) {
        strcpy(g_status, "SAVE BLOCKED: RECOVERY FILES NEED ATTENTION");
        draw_ui();
        return;
    }
    strcpy(g_status, "SAVING PPTX...");
    draw_ui();
    result = sh_pptx_save(&g_model);
    if (result == SH_PPTX_OK) g_dirty = 0;
    strcpy(g_status, result == SH_PPTX_OK ? "SAVED SHOWROOM.PPTX (BACKUP: SHOWROOM.BAK)" :
           result == SH_PPTX_BACKUP_ERROR ? "SAVE FAILED: CANNOT PRESERVE BACKUP" :
           result == SH_PPTX_RECOVERY_REQUIRED ? "SAVE INTERRUPTED: RESTART TO RECOVER" :
           result == SH_PPTX_COMMIT_COPY_ERROR ? "SAVE FAILED: COPY TO PPTX" :
           result == SH_PPTX_WRITE_ERROR ? "SAVE FAILED: DISK OR ZIP ERROR" :
           "SAVE FAILED: NOT ENOUGH MEMORY");
    if (result == SH_PPTX_RECOVERY_REQUIRED) g_recovery_ready = 0;
}

static void open_presentation(void)
{
    int result;
    if (!g_lfn_driver_ready) {
        strcpy(g_status, "LOAD DOSLFN BEFORE OPENING PPTX");
        return;
    }
    if (g_dirty) {
        strcpy(g_status, "SAVE CURRENT SLIDES BEFORE OPEN");
        return;
    }
    strcpy(g_status, "OPENING SHOWROOM.PPTX...");
    draw_ui();
    result = sh_pptx_load("SHOWROOM.PPTX", &g_loaded_model);
    if (result == SH_PPTX_OK) {
        g_model = g_loaded_model;
        g_dirty = 0;
        strcpy(g_status, "OPENED SHOWROOM.PPTX");
    } else if (result == SH_PPTX_OPEN_LIMIT) {
        strcpy(g_status, "PPTX EXCEEDS SHOWROOM TEXT LIMITS");
    } else if (result == SH_PPTX_OPEN_NOT_FOUND) {
        strcpy(g_status, "SHOWROOM.PPTX NOT FOUND");
    } else if (result == SH_PPTX_OPEN_INVALID) {
        strcpy(g_status, "PPTX INVALID OR UNSUPPORTED");
    } else {
        strcpy(g_status, "PPTX COULD NOT BE READ");
    }
}

static void draw_ui(void)
{
    unsigned i;
    unsigned first = g_model.current > 4 ? g_model.current - 4 : 0;
    const ShSlide *slide = sh_model_get(&g_model, g_model.current);
    char label[32];
    int x, y;
    pw_platform_canvas_clear(g_pixels, 7);
    rect(0, 0, SW, 20, PW_PLATFORM_COLOR_SHOWROOM_TITLE);
    pw_platform_canvas_line(g_pixels, 0, 0, SW - 1, 0, 15);
    pw_platform_canvas_line(g_pixels, 0, 0, 0, 19, 15);
    pw_platform_canvas_line(g_pixels, 0, 19, SW - 1, 19, 8);
    text("PANTHERA SHOWROOM - PRESENTATION", 18, 5, 15);
    draw_menu_bar();
    rect(0, 40, SW, 25, 8);
    draw_format_bar();
    draw_toolbar();
    rect(0, 91, SW, 19, 7);
    draw_rulers();
    rect(27, 110, 115, 344, 8);
    text("SLIDES", 40, 116, 0);
    pw_platform_canvas_line(g_pixels, 30, 128, 139, 128, 7);
    for (i = first; i < g_model.count && i < first + 5; ++i) {
        y = 135 + (int)(i - first) * 60;
        rect(37, y, 94, 46, i == g_model.current ? 15 : 7);
        frame(37, y, 94, 46, i == g_model.current ? 1 : 8);
        rect(46, y + 8, 76, 28, 15);
        frame(46, y + 8, 76, 28, 8);
        sprintf(label, "%u", i + 1);
        text(label, 34, y + 2, 0);
        text_fit(g_model.slides[i].title, 50, y + 18, 0, 11);
    }
    text("+", 43, 441, 0);
    rect(155, 110, 469, 344, 8);
    draw_tool_rail();
    draw_slide_tools();
    rect(166, 116, 445, 333, 15);
    frame(166, 116, 445, 333, 0);
    if (slide != NULL) {
        for (x = 184; x < 594; x += 8) {
            pw_platform_canvas_line(g_pixels, x, 155, x + 3, 155,
                                   g_editing && !g_edit_body ? 1 : 8);
            pw_platform_canvas_line(g_pixels, x, 198, x + 3, 198,
                                   g_editing && !g_edit_body ? 1 : 8);
            pw_platform_canvas_line(g_pixels, x, 217, x + 3, 217,
                                   g_editing && g_edit_body ? 1 : 8);
            pw_platform_canvas_line(g_pixels, x, 392, x + 3, 392,
                                   g_editing && g_edit_body ? 1 : 8);
        }
        for (y = 155; y < 199; y += 8) {
            pw_platform_canvas_line(g_pixels, 184, y, 184, y + 3,
                                   g_editing && !g_edit_body ? 1 : 8);
            pw_platform_canvas_line(g_pixels, 593, y, 593, y + 3,
                                   g_editing && !g_edit_body ? 1 : 8);
        }
        for (y = 217; y < 393; y += 8) {
            pw_platform_canvas_line(g_pixels, 184, y, 184, y + 3,
                                   g_editing && g_edit_body ? 1 : 8);
            pw_platform_canvas_line(g_pixels, 593, y, 593, y + 3,
                                   g_editing && g_edit_body ? 1 : 8);
        }
        if (g_editing && !g_edit_body) text_fit(g_edit, 197, 173, 1, 48);
        else text_fit(slide->title, 197, 173, 1, 48);
        if (g_editing && g_edit_body) text(g_edit, 197, 235, 0);
        else text(slide->body, 197, 235, 0);
    }
    draw_open_menu();
    rect(0, 455, SW, 25, 7);
    pw_platform_canvas_line(g_pixels, 0, 455, SW - 1, 455, 15);
    sprintf(label, "SLIDE %u OF %u", g_model.current + 1, g_model.count);
    text(label, 8, 463, 0);
    text(g_toolbar_hover >= 0 && g_toolbar_hover < 6 ? g_toolbar_label[g_toolbar_hover] :
         g_toolbar_hover == 6 ? "EDIT SLIDE TITLE" :
         g_toolbar_hover == 7 ? "EDIT SLIDE BODY" : g_status, 124, 463, 0);
    if (g_mouse) {
        pw_platform_canvas_line(g_pixels, g_mx, g_my, g_mx + 7, g_my + 10, 0);
        pw_platform_canvas_line(g_pixels, g_mx, g_my, g_mx, g_my + 9, 0);
    }
    pw_platform_video_present(g_pixels);
}

static int poll_mouse(void)
{
    int x, y;
    int old_x = g_mx, old_y = g_my;
    unsigned buttons;
    unsigned previous = g_buttons;
    int old_toolbar = g_toolbar_hover;
    if (!g_mouse || !pw_platform_mouse_read_state(&x, &y, &buttons)) return 0;
    g_mx = x; g_my = y; g_buttons = buttons;
    g_toolbar_hover = toolbar_hit(x, y);
    if ((buttons & 1) && !(previous & 1)) {
        if (y >= 20 && y < 40) {
            int menu = menu_from_x(x);
            if (menu != MENU_NONE) open_menu(menu);
        } else if (g_menu_open != MENU_NONE && x >= g_menu_x[g_menu_open] - 2 &&
                   x < g_menu_x[g_menu_open] + 190 && y >= 40 &&
                   y < 44 + menu_count(g_menu_open) * 24) {
            int item = (y - 43) / 24;
            if (item >= 0 && item < menu_count(g_menu_open)) {
                g_menu_selection = item;
                activate_menu();
            }
        } else if (g_toolbar_hover >= 0) {
            if (g_menu_open != MENU_NONE) close_menu();
            if (g_toolbar_hover == 8) toolbar_action(8);
            else toolbar_action(g_toolbar_hover);
        } else if (x >= 30 && x < 140 && y >= 135 && y < 435) {
            unsigned row = (unsigned)(y - 135) / 60;
            unsigned target = (g_model.current > 4 ? g_model.current - 4 : 0) + row;
            if (target < g_model.count) { g_model.current = target; strcpy(g_status, "SLIDE SELECTED"); }
        } else if (x >= 180 && x < 600 && y >= 155 && y < 199) {
            start_edit(0, 0);
        } else if (x >= 180 && x < 600 && y >= 217 && y < 393) {
            start_edit(1, 0);
        } else {
            if (g_menu_open != MENU_NONE) close_menu();
            if (x >= 0 && x < 28 && y >= 110 && y < 454)
                strcpy(g_status, "DRAWING TOOLS ARE STILL IN DEVELOPMENT");
        }
    }
    return x != old_x || y != old_y || buttons != previous || old_toolbar != g_toolbar_hover;
}

static void scan_key(int scan)
{
    if (g_menu_open != MENU_NONE) {
        if (scan == 72 && g_menu_selection > 0) --g_menu_selection;
        else if (scan == 80 && g_menu_selection + 1 < menu_count(g_menu_open))
            ++g_menu_selection;
        else if (scan == 75 && g_menu_open > MENU_FILE) open_menu(g_menu_open - 1);
        else if (scan == 77 && g_menu_open < MENU_HELP) open_menu(g_menu_open + 1);
        return;
    }
    if (g_editing) {
        if (scan == 28) commit_edit();
        else if (scan == 75 && g_edit_bytes) g_edit[--g_edit_bytes] = 0;
        return;
    }
    if (scan == 68) open_menu(MENU_FILE);
    else if (scan == 60) start_edit(0, 0);
    else if (scan == 61) start_edit(1, 0);
    else if (scan == 63) save_presentation();
    else if (scan == 72 && g_model.current) --g_model.current;
    else if (scan == 80 && g_model.current + 1 < g_model.count) ++g_model.current;
    else if (scan == 82) toolbar_action(0);
    else if (scan == 83) toolbar_action(1);
}

int main(void)
{
    int key;
    int changed;
    int recovery;
    int open_result;
    const char *lfn_ready;
    sh_model_init(&g_model);
    lfn_ready = getenv("PANTHERA_LFN_READY");
    g_lfn_driver_ready = lfn_ready != NULL && strcmp(lfn_ready, "1") == 0;
    if (!g_lfn_driver_ready) strcpy(g_status, "LOAD DOSLFN; PPTX NEEDS LONG FILE NAMES");
    else {
        recovery = sh_pptx_recover();
        g_recovery_ready = recovery != SH_PPTX_RECOVER_ERROR;
        if (recovery == SH_PPTX_RECOVER_BACKUP_RESTORED)
            strcpy(g_status, "RECOVERED PREVIOUS PPTX AFTER INTERRUPTED SAVE");
        else if (recovery == SH_PPTX_RECOVER_COMMIT_KEPT)
            strcpy(g_status, "COMPLETED PPTX SAVE RECOVERED");
        else if (recovery == SH_PPTX_RECOVER_FIRST_SAVE_REMOVED)
            strcpy(g_status, "REMOVED INCOMPLETE FIRST SAVE");
        else if (recovery == SH_PPTX_RECOVER_ERROR)
            strcpy(g_status, "RECOVERY FAILED: PRESERVE FILES; SAVE BLOCKED");
        open_result = sh_pptx_load("SHOWROOM.PPTX", &g_loaded_model);
        if (open_result == SH_PPTX_OK) {
            g_model = g_loaded_model;
            strcpy(g_status, recovery == SH_PPTX_RECOVER_BACKUP_RESTORED ?
                   "RECOVERED & OPENED SHOWROOM.PPTX" : "OPENED SHOWROOM.PPTX");
        } else if (open_result != SH_PPTX_OPEN_NOT_FOUND &&
                   recovery == SH_PPTX_RECOVER_NONE) {
            strcpy(g_status, open_result == SH_PPTX_OPEN_LIMIT ?
                   "PPTX EXCEEDS SHOWROOM LIMITS" : "PPTX INVALID OR UNSUPPORTED");
        }
    }
    pw_platform_font_init_fallback(g_font);
    pw_platform_font_load_bios(g_font);
    if (!pw_platform_video_start()) { puts("Panthera Showroom: VBE startup failed."); return 2; }
    if (!pw_platform_video_set_palette_entry(PW_PLATFORM_COLOR_SHOWROOM_TITLE, 96, 0, 0)) {
        pw_platform_video_stop();
        puts("Panthera Showroom: VBE palette setup failed.");
        return 3;
    }
    g_mouse = pw_platform_mouse_initialize();
    draw_ui();
    while (g_running) {
        changed = poll_mouse();
        if (kbhit()) {
            key = getch();
            if (key == 0 || key == 0xe0) scan_key(getch());
            else if (g_editing) {
                if (key == 27) g_editing = 0;
                else if (key == 13) commit_edit();
                else if (key == 8 && g_edit_bytes) g_edit[--g_edit_bytes] = 0;
                else if (key >= 32 && key <= 255 && g_edit_bytes < EDIT_BYTES) {
                    g_edit[g_edit_bytes++] = (char)key; g_edit[g_edit_bytes] = 0;
                }
            } else if (g_menu_open != MENU_NONE) {
                if (key == 27) close_menu();
                else if (key == 13) activate_menu();
                else if (key == 'f' || key == 'F') open_menu(MENU_FILE);
                else if (key == 'e' || key == 'E') open_menu(MENU_EDIT);
                else if (key == 'v' || key == 'V') open_menu(MENU_VIEW);
                else if (key == 'i' || key == 'I') open_menu(MENU_INSERT);
                else if (key == 'h' || key == 'H') open_menu(MENU_HELP);
            } else if (key == 27 || key == 'q' || key == 'Q') g_running = 0;
            else if (key == 13 && g_editing) commit_edit();
            else if (key == 's' || key == 'S') save_presentation();
            else if (key == 'n' || key == 'N') toolbar_action(0);
            changed = 1;
        }
        if (changed) draw_ui();
    }
    pw_platform_mouse_restore();
    pw_platform_video_stop();
    return 0;
}
