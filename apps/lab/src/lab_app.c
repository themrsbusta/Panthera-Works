/* Panthera Lab: original DOS spreadsheet UI, backed by the sparse model. */
#include <bios.h>
#include <dos.h>
#include <i86.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lab_model.h"
#include "lab_delimited.h"
#include "lab_xlsx.h"
#include "../../shared/platform/pw_platform.h"

#define SCREEN_W PW_PLATFORM_SCREEN_WIDTH
#define SCREEN_H PW_PLATFORM_SCREEN_HEIGHT
#define CELL_W 72
#define CELL_H 22
#define ROW_HEADER_W 48
#define GRID_X 12
#define GRID_Y 126
#define VISIBLE_COLUMNS 8
#define VISIBLE_ROWS 13
#define EDIT_CAPACITY (LAB_MAX_CELL_INPUT + 1)
#define WORKBOOK_PATH_CAPACITY 128

enum { MENU_NONE = 0, MENU_FILE = 1, MENU_EDIT = 2, MENU_VIEW = 3,
       MENU_INSERT = 4, MENU_FORMAT = 5, MENU_DATA = 6, MENU_HELP = 7 };
enum { PATH_OPEN = 1, PATH_SAVE_AS = 2, PATH_RENAME_SHEET = 3,
       PATH_IMPORT_DELIMITED = 4, PATH_EXPORT_DELIMITED = 5 };
enum { DEFER_NONE = 0, DEFER_NEW = 1, DEFER_LOAD = 2, DEFER_EXIT = 3,
       DEFER_IMPORT_DELIMITED = 4 };

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned long u32;

static LabWorkbook *g_workbook;
static LabSheet *g_sheet;
static u8 g_pixels[SCREEN_W * SCREEN_H];
static u8 g_font[256][16];
static char *g_edit;
static u32 g_edit_bytes;
static u32 g_row;
static u16 g_column;
static u32 g_first_row;
static u16 g_first_column;
static int g_editing;
static int g_running = 1;
static int g_mouse_available;
static int g_lfn_driver_ready;
static int g_mouse_x;
static int g_mouse_y;
static unsigned g_mouse_buttons;
static int g_menu_open;
static int g_menu_selection;
static int g_grid_visible = 1;
static int g_dirty;
static char g_status[96];
static char g_workbook_path[WORKBOOK_PATH_CAPACITY] = "WORKBOOK.XLSX";
static char g_open_path[WORKBOOK_PATH_CAPACITY];
static int g_status_error;
static int g_open_pending;
static int g_path_mode;
static int g_confirm_pending;
static int g_deferred_action;
static int g_toolbar_hover = -1;

static const int g_toolbar_x[6] = {8, 34, 60, 98, 124, 162};
static const int g_toolbar_icon[6] = {
    PW_PLATFORM_ICON_NEW, PW_PLATFORM_ICON_OPEN, PW_PLATFORM_ICON_SAVE,
    PW_PLATFORM_ICON_UNDO, PW_PLATFORM_ICON_REDO, PW_PLATFORM_ICON_FUNCTION
};
static const char *g_toolbar_label[6] = {"NEW WORKBOOK", "OPEN WORKBOOK", "SAVE WORKBOOK",
    "UNDO", "REDO", "FUNCTION WIZARD (IN DEVELOPMENT)"};

static int toolbar_hit(int x, int y)
{
    int i;
    if (y < 50 || y >= 76) return -1;
    for (i = 0; i < 6; ++i)
        if (x >= g_toolbar_x[i] && x < g_toolbar_x[i] + 22) return i;
    return -1;
}

static void text(const char *value, int x, int y, u8 color)
{
    pw_platform_canvas_text(g_pixels, g_font, value, x, y, color);
}

static void rect(int x, int y, int width, int height, u8 color)
{
    pw_platform_canvas_fill_rect(g_pixels, x, y, width, height, color);
}

static void outline(int x, int y, int width, int height, u8 color)
{
    pw_platform_canvas_line(g_pixels, x, y, x + width - 1, y, color);
    pw_platform_canvas_line(g_pixels, x, y, x, y + height - 1, color);
    pw_platform_canvas_line(g_pixels, x + width - 1, y, x + width - 1,
                            y + height - 1, color);
    pw_platform_canvas_line(g_pixels, x, y + height - 1, x + width - 1,
                            y + height - 1, color);
}

static void status(const char *message, int is_error)
{
    strncpy(g_status, message, sizeof(g_status) - 1);
    g_status[sizeof(g_status) - 1] = '\0';
    g_status_error = is_error;
}

static const char *menu_name(int menu)
{
    switch (menu) {
        case MENU_FILE: return "File";
        case MENU_EDIT: return "Edit";
        case MENU_VIEW: return "View";
        case MENU_INSERT: return "Insert";
        case MENU_FORMAT: return "Format";
        case MENU_DATA: return "Data";
        default: return "Help";
    }
}

static int menu_x(int menu)
{
    static const int positions[8] = {0, 12, 64, 116, 176, 244, 316, 374};
    return positions[menu];
}

static int menu_from_x(int x)
{
    static const int edges[8] = {0, 56, 108, 168, 236, 308, 366, 480};
    int i;
    for (i = 0; i < 7; ++i)
        if (x >= edges[i] && x < edges[i + 1]) return i + 1;
    return MENU_NONE;
}

static int menu_count(int menu)
{
    switch (menu) {
        case MENU_FILE: return 7;
        case MENU_EDIT: return 3;
        case MENU_VIEW: return 2;
        case MENU_INSERT: return 4;
        case MENU_FORMAT: return 1;
        case MENU_DATA: return 2;
        default: return 1;
    }
}

static const char *menu_item(int menu, int index)
{
    static const char *file[] = {"New Workbook", "Open XLSX...", "Save XLSX", "Save As...",
        "Import CSV/TSV...", "Export CSV/TSV...", "Exit"};
    static const char *edit[] = {"Undo  Ctrl+Z", "Redo  Ctrl+Y", "Edit Cell (F2)"};
    static const char *view[] = {"Recenter Selection", "Show Grid"};
    static const char *insert[] = {"New Worksheet", "Delete Worksheet", "Rename Worksheet...", "Function Wizard..."};
    static const char *format[] = {"Cell Format..."};
    static const char *data[] = {"Recalculate", "Sort / Filter..."};
    static const char *help[] = {"About Panthera Lab"};
    if (menu == MENU_FILE && index < 7) return file[index];
    if (menu == MENU_EDIT && index < 3) return edit[index];
    if (menu == MENU_VIEW && index < 2)
        return index == 1 && g_grid_visible ? "Hide Grid" : view[index];
    if (menu == MENU_INSERT && index < 4) return insert[index];
    if (menu == MENU_FORMAT) return format[0];
    if (menu == MENU_DATA && index < 2) return data[index];
    return help[0];
}

static void close_menu(void)
{
    g_menu_open = MENU_NONE;
    g_menu_selection = 0;
}

static void open_menu(int menu)
{
    g_menu_open = menu;
    g_menu_selection = 0;
}

static void begin_edit(int replace)
{
    const LabCell *cell;
    const char *input;
    if (!g_edit) return;
    cell = lab_sheet_get(g_sheet, g_row, g_column);
    input = cell != NULL && cell->input != NULL ? cell->input : "";
    if (!replace) {
        g_edit_bytes = (u32)strlen(input);
        if (g_edit_bytes >= EDIT_CAPACITY) g_edit_bytes = EDIT_CAPACITY - 1;
        memcpy(g_edit, input, g_edit_bytes);
    } else g_edit_bytes = 0;
    g_edit[g_edit_bytes] = '\0';
    g_editing = 1;
}

static void commit_edit(void)
{
    int result;
    if (!g_editing) return;
    result = lab_workbook_set_cell_input(g_workbook,
                lab_workbook_active_index(g_workbook), g_row, g_column, g_edit);
    if (result != LAB_MODEL_OK) {
        status(result == LAB_MODEL_NO_MEMORY ? "NOT ENOUGH MEMORY" :
               "CELL INPUT REJECTED", 1);
    } else if (lab_sheet_recalculate(g_sheet) != LAB_MODEL_OK) {
        status("RECALCULATION FAILED", 1);
    } else { status("CELL UPDATED", 0); g_dirty = 1; }
    g_editing = 0;
    g_edit_bytes = 0;
    g_edit[0] = '\0';
}

static void cancel_edit(void)
{
    g_editing = 0;
    g_edit_bytes = 0;
    if (g_edit) g_edit[0] = '\0';
}

static void move_selection(int row_delta, int column_delta)
{
    if (row_delta < 0 && g_row > 0) --g_row;
    else if (row_delta > 0 && g_row + 1 < LAB_MAX_ROWS) ++g_row;
    if (column_delta < 0 && g_column > 0) --g_column;
    else if (column_delta > 0 && g_column + 1 < LAB_MAX_COLUMNS) ++g_column;
    if (g_row < g_first_row) g_first_row = g_row;
    if (g_row >= g_first_row + VISIBLE_ROWS) g_first_row = g_row - VISIBLE_ROWS + 1;
    if (g_column < g_first_column) g_first_column = g_column;
    if (g_column >= g_first_column + VISIBLE_COLUMNS)
        g_first_column = (u16)(g_column - VISIBLE_COLUMNS + 1);
    lab_workbook_set_sheet_selection(g_workbook,
        lab_workbook_active_index(g_workbook), g_row, g_column);
}

static void restore_active_sheet_view(void)
{
    u32 active = lab_workbook_active_index(g_workbook);
    g_sheet = lab_workbook_active_sheet(g_workbook);
    if (lab_workbook_get_sheet_selection(g_workbook, active, &g_row, &g_column) != LAB_MODEL_OK)
        g_row = 0, g_column = 0;
    g_first_row = g_row;
    g_first_column = g_column;
    if (g_row >= VISIBLE_ROWS) g_first_row = g_row - VISIBLE_ROWS + 1;
    if ((u32)g_column >= VISIBLE_COLUMNS)
        g_first_column = (u16)(g_column - VISIBLE_COLUMNS + 1);
}

static void apply_history(int redo)
{
    u32 sheet_index;
    u32 row;
    u16 column;
    int result;
    if (g_open_pending || g_confirm_pending) return;
    if (g_editing) commit_edit();
    result = redo ? lab_workbook_redo(g_workbook, &sheet_index, &row, &column) :
                    lab_workbook_undo(g_workbook, &sheet_index, &row, &column);
    if (result == LAB_MODEL_OK) {
        g_sheet = lab_workbook_active_sheet(g_workbook);
        g_row = row;
        g_column = column;
        g_first_row = row >= VISIBLE_ROWS ? row - VISIBLE_ROWS + 1 : 0;
        g_first_column = column >= VISIBLE_COLUMNS ?
            (u16)(column - VISIBLE_COLUMNS + 1) : 0;
        lab_sheet_recalculate(g_sheet);
        g_dirty = 1;
        status(redo ? "REDO COMPLETE" : "UNDO COMPLETE", 0);
    } else if (result == LAB_WORKBOOK_NOTHING_TO_UNDO ||
               result == LAB_WORKBOOK_NOTHING_TO_REDO) {
        status(redo ? "NOTHING TO REDO" : "NOTHING TO UNDO", 0);
    } else status("UNDO/REDO FAILED", 1);
}

static void new_workbook(void)
{
    LabWorkbook *fresh;
    fresh = lab_workbook_create();
    if (fresh == NULL) { status("NEW WORKBOOK NEEDS MORE MEMORY", 1); return; }
    lab_workbook_destroy(g_workbook);
    g_workbook = fresh;
    g_sheet = lab_workbook_active_sheet(g_workbook);
    g_row = 0;
    g_column = 0;
    g_first_row = 0;
    g_first_column = 0;
    strcpy(g_workbook_path, "WORKBOOK.XLSX");
    status("NEW WORKBOOK", 0);
}

static int load_workbook_path(void)
{
    int result;
    if (g_open_path[0] == '\0') { status("ENTER XLSX PATH", 1); return 0; }
    g_open_pending = 0;
    if (g_dirty) {
        g_confirm_pending = 1;
        g_deferred_action = DEFER_LOAD;
        status("UNSAVED CHANGES: Y SAVE  N DISCARD  ESC CANCEL", 1);
        return 0;
    }
    if (!g_lfn_driver_ready) { status("LOAD DOSLFN TO OPEN .XLSX", 1); return 0; }
    result = lab_xlsx_load(&g_workbook, g_open_path);
    if (result == LAB_XLSX_OK) {
        restore_active_sheet_view();
        strncpy(g_workbook_path, g_open_path, sizeof(g_workbook_path) - 1);
        g_workbook_path[sizeof(g_workbook_path) - 1] = '\0';
        g_dirty = 0;
        status("OPENED XLSX WORKBOOK", 0);
    } else if (result == LAB_XLSX_UNSUPPORTED) status("XLSX COMPRESSION/FEATURE NOT SUPPORTED YET", 1);
    else if (result == LAB_XLSX_NO_MEMORY) status("XLSX OPEN NEEDS MORE MEMORY", 1);
    else if (result == LAB_XLSX_BAD_XML) status("XLSX WORKSHEET XML INVALID", 1);
    else if (result == LAB_XLSX_BAD_EOCD) status("XLSX ZIP END RECORD INVALID", 1);
    else if (result == LAB_XLSX_BAD_DIRECTORY) status("XLSX ZIP DIRECTORY INVALID", 1);
    else if (result == LAB_XLSX_BAD_LOCAL_HEADER) status("XLSX ZIP ENTRY HEADER INVALID", 1);
    else if (result == LAB_XLSX_BAD_LOCAL_SIGNATURE) status("XLSX ZIP LOCAL SIGNATURE INVALID", 1);
    else if (result == LAB_XLSX_BAD_LOCAL_FIELDS) status("XLSX ZIP LOCAL FIELDS INVALID", 1);
    else if (result == LAB_XLSX_LOCAL_SEEK_FAILED) status("XLSX ZIP LOCAL SEEK FAILED", 1);
    else if (result == LAB_XLSX_LOCAL_READ_FAILED) status("XLSX ZIP LOCAL READ FAILED", 1);
    else if (result == LAB_XLSX_LOCAL_SIGNATURE_MISMATCH) status("XLSX ZIP SIGNATURE MISMATCH", 1);
    else if (result == LAB_XLSX_BAD_DEFLATE) status("XLSX DEFLATE STREAM INVALID", 1);
    else if (result == LAB_XLSX_BAD_CRC) status("XLSX ZIP ENTRY CRC INVALID", 1);
    else status("XLSX OPEN FAILED; CURRENT WORKBOOK KEPT", 1);
    return result == LAB_XLSX_OK;
}

static void begin_open_workbook(void)
{
    if (!g_lfn_driver_ready) { status("LOAD DOSLFN TO OPEN .XLSX", 1); return; }
    g_open_path[0] = '\0';
    g_open_pending = 1;
    g_path_mode = PATH_OPEN;
    status("OPEN XLSX PATH; ENTER=OPEN ESC=CANCEL", 0);
}

static void begin_save_as(void)
{
    if (!g_lfn_driver_ready) { status("LOAD DOSLFN TO SAVE .XLSX", 1); return; }
    g_open_path[0] = '\0';
    g_open_pending = 1;
    g_path_mode = PATH_SAVE_AS;
    status("SAVE AS XLSX PATH; ENTER=SAVE ESC=CANCEL", 0);
}

static void begin_rename_worksheet(void)
{
    g_open_path[0] = '\0';
    g_open_pending = 1;
    g_path_mode = PATH_RENAME_SHEET;
    status("ENTER WORKSHEET NAME; ENTER=RENAME ESC=CANCEL", 0);
}

static void begin_import_delimited(void)
{
    if (!g_lfn_driver_ready) { status("LOAD DOSLFN TO IMPORT CSV/TSV", 1); return; }
    g_open_path[0] = '\0';
    g_open_pending = 1;
    g_path_mode = PATH_IMPORT_DELIMITED;
    status("ENTER CSV/TSV PATH; ENTER=IMPORT ESC=CANCEL", 0);
}

static void begin_export_delimited(void)
{
    if (!g_lfn_driver_ready) { status("LOAD DOSLFN TO EXPORT CSV/TSV", 1); return; }
    g_open_path[0] = '\0';
    g_open_pending = 1;
    g_path_mode = PATH_EXPORT_DELIMITED;
    status("ENTER .CSV OR .TSV PATH; ENTER=EXPORT ESC=CANCEL", 0);
}

static unsigned char delimited_delimiter(const char *path)
{
    unsigned length = (unsigned)strlen(path);
    if (length >= 4 && stricmp(path + length - 4, ".TSV") == 0) return '\t';
    if (length >= 4 && stricmp(path + length - 4, ".CSV") == 0) return ',';
    return 0;
}

static int import_delimited_path(void)
{
    unsigned char delimiter;
    int result;
    if (g_open_path[0] == '\0') { status("ENTER CSV/TSV PATH", 1); return 0; }
    if (g_dirty) {
        g_confirm_pending = 1;
        g_deferred_action = DEFER_IMPORT_DELIMITED;
        status("UNSAVED CHANGES: Y SAVE  N DISCARD  ESC CANCEL", 1);
        return 0;
    }
    delimiter = delimited_delimiter(g_open_path);
    if (delimiter == 0) { status("IMPORT REQUIRES .CSV OR .TSV", 1); return 0; }
    result = lab_delimited_load(&g_workbook, g_open_path, delimiter);
    if (result == LAB_DELIMITED_OK) {
        restore_active_sheet_view();
        strcpy(g_workbook_path, "WORKBOOK.XLSX");
        g_dirty = 1;
        status("IMPORTED CSV/TSV; SAVE AS XLSX", 0);
        return 1;
    }
    if (result == LAB_DELIMITED_NO_MEMORY) status("CSV/TSV IMPORT NEEDS MORE MEMORY", 1);
    else if (result == LAB_DELIMITED_BAD_FORMAT) status("CSV/TSV FORMAT INVALID; CURRENT KEPT", 1);
    else status("CSV/TSV IMPORT FAILED; CURRENT KEPT", 1);
    return 0;
}

static int export_delimited_path(void)
{
    unsigned char delimiter;
    int result;
    if (g_open_path[0] == '\0') { status("ENTER CSV/TSV PATH", 1); return 0; }
    delimiter = delimited_delimiter(g_open_path);
    if (delimiter == 0) { status("EXPORT REQUIRES .CSV OR .TSV", 1); return 0; }
    result = lab_delimited_save(g_sheet, g_open_path, delimiter);
    if (result == LAB_DELIMITED_OK) { status("EXPORTED ACTIVE SHEET CSV/TSV", 0); return 1; }
    if (result == LAB_DELIMITED_NO_MEMORY) status("CSV/TSV EXPORT NEEDS MORE MEMORY", 1);
    else status("CSV/TSV EXPORT FAILED; ORIGINAL KEPT", 1);
    return 0;
}

static void rename_active_worksheet(void)
{
    int result;
    if (g_open_path[0] == '\0') {
        status("WORKSHEET NAME CANNOT BE EMPTY", 1);
        g_open_pending = 1;
        return;
    }
    result = lab_workbook_rename_sheet(g_workbook,
                                       lab_workbook_active_index(g_workbook),
                                       g_open_path);
    if (result == LAB_MODEL_OK) {
        g_open_pending = 0;
        g_dirty = 1;
        status("WORKSHEET RENAMED", 0);
    } else if (result == LAB_WORKBOOK_DUPLICATE_NAME) {
        status("WORKSHEET NAME ALREADY EXISTS", 1);
        g_open_pending = 1;
    } else if (result == LAB_WORKBOOK_BAD_NAME) {
        status("USE 1-31 PRINTABLE CHARACTERS", 1);
        g_open_pending = 1;
    } else {
        status("COULD NOT RENAME WORKSHEET", 1);
        g_open_pending = 1;
    }
}

static int save_workbook_to(const char *path)
{
    int result;
    if (!g_lfn_driver_ready) {
        status("LOAD DOSLFN TO SAVE .XLSX", 1);
        return 0;
    }
    result = lab_xlsx_save(g_workbook, path);
    if (result == LAB_XLSX_OK) { status("SAVED XLSX WORKBOOK", 0); return 1; }
    else if (result == LAB_XLSX_NO_MEMORY) status("XLSX SAVE NEEDS MORE MEMORY", 1);
    else status("XLSX SAVE FAILED; ORIGINAL KEPT", 1);
    return 0;
}

static int save_workbook(void)
{
    if (!save_workbook_to(g_workbook_path)) return 0;
    g_dirty = 0;
    return 1;
}

static void save_as_path(void)
{
    unsigned length = (unsigned)strlen(g_open_path);
    if (!length) { status("ENTER XLSX PATH", 1); g_open_pending = 1; return; }
    if (length < 5 || stricmp(g_open_path + length - 5, ".XLSX") != 0) {
        status("SAVE AS REQUIRES .XLSX EXTENSION", 1);
        g_open_pending = 1;
        return;
    }
    if (!save_workbook_to(g_open_path)) { g_open_pending = 1; return; }
    strncpy(g_workbook_path, g_open_path, sizeof(g_workbook_path) - 1);
    g_workbook_path[sizeof(g_workbook_path) - 1] = '\0';
    g_dirty = 0;
    status("SAVED AS XLSX WORKBOOK", 0);
}

static void perform_deferred_action(int action)
{
    if (action == DEFER_NEW) new_workbook();
    else if (action == DEFER_EXIT) g_running = 0;
    else if (action == DEFER_IMPORT_DELIMITED && !import_delimited_path()) g_dirty = 1;
}

static void answer_unsaved(int choice)
{
    int action = g_deferred_action;
    g_confirm_pending = 0;
    g_deferred_action = DEFER_NONE;
    if (choice == 0) { status("OPERATION CANCELLED; WORKBOOK KEPT", 0); return; }
    if (choice == 1) {
        if (!save_workbook()) return;
    } else g_dirty = 0;
    if (action == DEFER_LOAD) {
        if (!load_workbook_path() && choice == 2) g_dirty = 1;
    } else if (action == DEFER_IMPORT_DELIMITED) {
        if (!import_delimited_path() && choice == 2) g_dirty = 1;
    } else perform_deferred_action(action);
}

static void request_new_workbook(void)
{
    if (!g_dirty) { new_workbook(); return; }
    g_confirm_pending = 1;
    g_deferred_action = DEFER_NEW;
    status("UNSAVED CHANGES: Y SAVE  N DISCARD  ESC CANCEL", 1);
}

static void request_exit(void)
{
    if (!g_dirty) { g_running = 0; return; }
    g_confirm_pending = 1;
    g_deferred_action = DEFER_EXIT;
    status("UNSAVED CHANGES: Y SAVE  N DISCARD  ESC CANCEL", 1);
}

static void activate_toolbar(int index)
{
    if (index == 0) request_new_workbook();
    else if (index == 1) begin_open_workbook();
    else if (index == 2) save_workbook();
    else if (index == 3 || index == 4) apply_history(index == 4);
    else if (index == 5) status("FUNCTION WIZARD IS STILL IN DEVELOPMENT", 1);
}

static void activate_sheet(u32 index)
{
    if (lab_workbook_set_active(g_workbook, index) != LAB_MODEL_OK) return;
    restore_active_sheet_view();
    sprintf(g_status, "ACTIVE: %.31s", lab_workbook_sheet_name(g_workbook, index));
    g_status_error = 0;
}

static void new_worksheet(void)
{
    char name[LAB_WORKSHEET_NAME_BYTES + 1];
    u32 number;
    int result;
    for (number = 1; number != 0xffffffffUL; ++number) {
        sprintf(name, "Sheet%lu", number);
        result = lab_workbook_add_sheet(g_workbook, name, NULL);
        if (result == LAB_MODEL_OK) {
            g_sheet = lab_workbook_active_sheet(g_workbook);
            restore_active_sheet_view();
            g_dirty = 1;
            status("NEW WORKSHEET ADDED", 0);
            return;
        }
        if (result != LAB_WORKBOOK_DUPLICATE_NAME) {
            status(result == LAB_WORKBOOK_NO_MEMORY ? "NOT ENOUGH MEMORY FOR WORKSHEET" :
                   "COULD NOT ADD WORKSHEET", 1);
            return;
        }
    }
    status("WORKSHEET NAME SPACE EXHAUSTED", 1);
}

static void delete_active_worksheet(void)
{
    int result = lab_workbook_remove_sheet(g_workbook,
                                           lab_workbook_active_index(g_workbook));
    if (result == LAB_WORKBOOK_LAST_SHEET) {
        status("A WORKBOOK MUST KEEP ONE WORKSHEET", 1);
        return;
    }
    if (result != LAB_MODEL_OK) {
        status("COULD NOT DELETE WORKSHEET", 1);
        return;
    }
    g_sheet = lab_workbook_active_sheet(g_workbook);
    restore_active_sheet_view();
    g_dirty = 1;
    status("WORKSHEET DELETED", 0);
}

static void draw_sheet_tabs(void)
{
    u32 count = lab_workbook_sheet_count(g_workbook);
    u32 active = lab_workbook_active_index(g_workbook);
    u32 first = active > 4 ? active - 4 : 0;
    u32 i;
    rect(0, 434, SCREEN_W, 23, 8);
    for (i = first; i < count && i < first + 5; ++i) {
        int x = 8 + (int)(i - first) * 110;
        int selected = i == active;
        char clipped[14];
        const char *name = lab_workbook_sheet_name(g_workbook, i);
        unsigned n = name != NULL ? (unsigned)strlen(name) : 0;
        if (n > sizeof(clipped) - 1) n = sizeof(clipped) - 1;
        if (n) memcpy(clipped, name, n);
        clipped[n] = '\0';
        rect(x, 435, 106, 21, selected ? 1 : 7);
        outline(x, 435, 106, 21, selected ? 1 : 0);
        text(clipped, x + 5, 438, selected ? 15 : 0);
    }
    rect(568, 435, 34, 21, 7);
    outline(568, 435, 34, 21, 0);
    text("+", 580, 438, 1);
}

static void draw_toolbar(void)
{
    int i;
    for (i = 0; i < 6; ++i) {
        int x = g_toolbar_x[i];
        u8 face = g_toolbar_hover == i ? 9 : 7;
        u8 ink = (i >= 3 || ((i == 1 || i == 2) && !g_lfn_driver_ready)) ? 8 : 0;
        u8 accent = ink == 8 ? 8 : 1;
        rect(x, 52, 22, 20, face);
        pw_platform_canvas_line(g_pixels, x, 52, x + 21, 52, 15);
        pw_platform_canvas_line(g_pixels, x, 52, x, 71, 15);
        pw_platform_canvas_line(g_pixels, x + 21, 52, x + 21, 71, 8);
        pw_platform_canvas_line(g_pixels, x, 71, x + 21, 71, 8);
        pw_platform_canvas_toolbar_icon(g_pixels, g_toolbar_icon[i], x + 2, 53, ink, accent);
    }
    pw_platform_canvas_line(g_pixels, 91, 54, 91, 70, 8);
    pw_platform_canvas_line(g_pixels, 155, 54, 155, 70, 8);
}

static void activate_menu(void)
{
    switch (g_menu_open) {
        case MENU_FILE:
            if (g_menu_selection == 0) request_new_workbook();
            else if (g_menu_selection == 1) begin_open_workbook();
            else if (g_menu_selection == 2) save_workbook();
            else if (g_menu_selection == 3) begin_save_as();
            else if (g_menu_selection == 4) begin_import_delimited();
            else if (g_menu_selection == 5) begin_export_delimited();
            else request_exit();
            break;
        case MENU_EDIT:
            if (g_menu_selection == 0 || g_menu_selection == 1)
                apply_history(g_menu_selection == 1);
            else begin_edit(0);
            break;
        case MENU_VIEW:
            if (g_menu_selection == 0) {
                g_first_row = g_row;
                g_first_column = g_column;
                status("SELECTION RECENTERED", 0);
            } else {
                g_grid_visible = !g_grid_visible;
                status(g_grid_visible ? "GRID SHOWN" : "GRID HIDDEN", 0);
            }
            break;
        case MENU_INSERT:
            if (g_menu_selection == 0) new_worksheet();
            else if (g_menu_selection == 1) delete_active_worksheet();
            else if (g_menu_selection == 2) begin_rename_worksheet();
            else status("FUNCTION WIZARD IS STILL IN DEVELOPMENT", 1);
            break;
        case MENU_FORMAT:
            status("CELL FORMATTING IS STILL IN DEVELOPMENT", 1);
            break;
        case MENU_DATA:
            if (g_menu_selection == 0) {
                lab_sheet_recalculate(g_sheet);
                status("WORKBOOK RECALCULATED", 0);
            } else status("SORT/FILTER IS STILL IN DEVELOPMENT", 1);
            break;
        default:
            status("PANTHERA LAB  -  PRODUCT DEVELOPMENT BUILD", 0);
            break;
    }
    close_menu();
}

static void draw_menu(void)
{
    int i;
    int x;
    int y;
    if (g_menu_open == MENU_NONE) return;
    x = menu_x(g_menu_open);
    y = 50;
    rect(x + 6, y + 6, 190, 26 * menu_count(g_menu_open) + 8, 8);
    rect(x, y, 190, 26 * menu_count(g_menu_open) + 4, 7);
    outline(x, y, 190, 26 * menu_count(g_menu_open) + 4, 0);
    for (i = 0; i < menu_count(g_menu_open); ++i) {
        if (i == g_menu_selection) rect(x + 4, y + 4 + i * 26, 182, 24, 1);
        text(menu_item(g_menu_open, i), x + 12, y + 8 + i * 26,
             i == g_menu_selection ? 15 : 0);
    }
}

static void draw_mouse(void)
{
    if (!g_mouse_available) return;
    pw_platform_canvas_line(g_pixels, g_mouse_x, g_mouse_y,
                            g_mouse_x + 7, g_mouse_y + 11, 0);
    pw_platform_canvas_line(g_pixels, g_mouse_x, g_mouse_y,
                            g_mouse_x, g_mouse_y + 9, 0);
    pw_platform_canvas_line(g_pixels, g_mouse_x, g_mouse_y + 9,
                            g_mouse_x + 6, g_mouse_y + 6, 0);
}

static void draw_grid(void)
{
    u32 r;
    u16 c;
    int x0 = GRID_X + ROW_HEADER_W;
    int y0 = GRID_Y + CELL_H;
    if (!g_grid_visible) return;
    rect(GRID_X, GRID_Y, ROW_HEADER_W, CELL_H, 8);
    for (c = 0; c < VISIBLE_COLUMNS; ++c) {
        char column_name[4];
        int x = x0 + (int)c * CELL_W;
        u16 actual = (u16)(g_first_column + c);
        rect(x, GRID_Y, CELL_W, CELL_H, 8);
        outline(x, GRID_Y, CELL_W, CELL_H, 0);
        if (lab_column_format(actual, column_name, sizeof(column_name)))
            text(column_name, x + (CELL_W - (int)strlen(column_name) * 8) / 2,
                 GRID_Y + 3, 0);
    }
    for (r = 0; r < VISIBLE_ROWS; ++r) {
        char row_name[12];
        u32 actual_row = g_first_row + r;
        int y = y0 + (int)r * CELL_H;
        sprintf(row_name, "%lu", actual_row + 1);
        rect(GRID_X, y, ROW_HEADER_W, CELL_H, actual_row == g_row ? 1 : 8);
        outline(GRID_X, y, ROW_HEADER_W, CELL_H, 8);
        text(row_name, GRID_X + ROW_HEADER_W - (int)strlen(row_name) * 8 - 6,
             y + 3, actual_row == g_row ? 15 : 0);
        for (c = 0; c < VISIBLE_COLUMNS; ++c) {
            int x = x0 + (int)c * CELL_W;
            int selected = actual_row == g_row && g_first_column + c == g_column;
            const LabCell *cell = lab_sheet_get(g_sheet, actual_row,
                                                (u16)(g_first_column + c));
            const char *value = cell != NULL ? lab_cell_display(cell) : "";
            char clipped[10];
            unsigned n = 0;
            rect(x, y, CELL_W, CELL_H, selected ? 1 : 15);
            outline(x, y, CELL_W, CELL_H, selected ? 1 : 8);
            if (selected && g_editing) value = g_edit;
            while (value[n] != '\0' && n < sizeof(clipped) - 1) {
                clipped[n] = value[n];
                ++n;
            }
            clipped[n] = '\0';
            text(clipped, x + 4, y + 3, selected ? 15 : 0);
        }
    }
}

static void draw_ui(void)
{
    char address[24];
    char header[128];
    char value[128];
    u32 count = lab_sheet_count(g_sheet);
    lab_cell_reference_format(g_row, g_column, address, sizeof(address));
    pw_platform_canvas_clear(g_pixels, 7);
    rect(0, 0, SCREEN_W, 24, PW_PLATFORM_COLOR_LAB_TITLE);
    text("PANTHERA LAB  |  WORKSHEET", 10, 4, 15);
    rect(0, 24, SCREEN_W, 26, 7);
    text("FILE   EDIT   VIEW   INSERT   FORMAT   DATA   HELP", 12, 29, 0);
    rect(0, 50, SCREEN_W, 26, 8);
    draw_toolbar();
    rect(0, 76, SCREEN_W, 30, 7);
    outline(10, 80, 62, 22, 8);
    text(address, 15, 83, 0);
    outline(80, 80, 548, 22, 8);
    if (g_editing) sprintf(value, "%.120s", g_edit);
    else if (g_open_pending) sprintf(value, "%.120s", g_open_path);
    else {
        const LabCell *selected = lab_sheet_get(g_sheet, g_row, g_column);
        sprintf(value, "%.120s", selected != NULL && selected->input ? selected->input : "");
    }
    text(value, 86, 83, 0);
    sprintf(header, "%.31s  SHEET %lu OF %lu  CELLS %lu",
            lab_workbook_sheet_name(g_workbook, lab_workbook_active_index(g_workbook)),
            lab_workbook_active_index(g_workbook) + 1,
            lab_workbook_sheet_count(g_workbook), count);
    text(header, 12, 108, 1);
    draw_grid();
    draw_sheet_tabs();
    rect(0, 457, SCREEN_W, 23, 7);
    if (g_toolbar_hover >= 0) text(g_toolbar_label[g_toolbar_hover], 8, 461, 0);
    else text(g_status[0] ? g_status : "F2 EDIT  ENTER COMMIT  ARROWS MOVE  ESC EXIT", 8, 461,
              g_status_error ? 4 : 0);
    draw_menu();
    if (g_confirm_pending) {
        rect(148, 184, 344, 102, 8);
        outline(148, 184, 344, 102, 0);
        rect(152, 188, 336, 94, 7);
        text("UNSAVED WORKBOOK CHANGES", 178, 198, 1);
        text("SAVE BEFORE CONTINUING?", 190, 214, 0);
        rect(210, 226, 120, 32, 1); outline(210, 226, 120, 32, 0);
        rect(338, 226, 120, 32, 8); outline(338, 226, 120, 32, 0);
        text("Y  SAVE", 246, 236, 15);
        text("N  DISCARD", 356, 236, 0);
        text("ESC  CANCEL", 264, 266, 0);
    }
    draw_mouse();
    pw_platform_video_present(g_pixels);
}

static void select_from_mouse(int x, int y)
{
    int left = GRID_X + ROW_HEADER_W;
    int top = GRID_Y + CELL_H;
    if (g_grid_visible && x >= left && x < left + VISIBLE_COLUMNS * CELL_W &&
        y >= top && y < top + VISIBLE_ROWS * CELL_H) {
        if (g_editing) commit_edit();
        g_column = (u16)(g_first_column + (x - left) / CELL_W);
        g_row = g_first_row + (u32)((y - top) / CELL_H);
        lab_workbook_set_sheet_selection(g_workbook,
            lab_workbook_active_index(g_workbook), g_row, g_column);
        status("CELL SELECTED", 0);
    }
}

static int poll_mouse(void)
{
    int x;
    int y;
    unsigned buttons;
    unsigned old_buttons = g_mouse_buttons;
    int old_toolbar = g_toolbar_hover;
    int moved;
    if (!g_mouse_available || !pw_platform_mouse_read_state(&x, &y, &buttons)) return 0;
    moved = x != g_mouse_x || y != g_mouse_y;
    g_mouse_x = x;
    g_mouse_y = y;
    g_mouse_buttons = buttons;
    g_toolbar_hover = toolbar_hit(x, y);
    if ((buttons & 1) && !(old_buttons & 1) && g_confirm_pending) {
        if (y >= 226 && y < 258 && x >= 210 && x < 330) answer_unsaved(1);
        else if (y >= 226 && y < 258 && x >= 338 && x < 458) answer_unsaved(2);
        else if (y >= 184 && y < 286) answer_unsaved(0);
    } else if ((buttons & 1) && !(old_buttons & 1) && g_open_pending) {
        /* Path entry is keyboard-driven; clicks must not activate the sheet underneath. */
    } else if ((buttons & 1) && !(old_buttons & 1)) {
        if (y >= 434 && y < 457) {
            if (x >= 568 && x < 602) {
                if (g_editing) commit_edit();
                new_worksheet();
            } else if (x >= 8 && x < 558) {
                u32 first = lab_workbook_active_index(g_workbook) > 4 ?
                    lab_workbook_active_index(g_workbook) - 4 : 0;
                u32 selected = first + (u32)(x - 8) / 110;
                if (g_editing) commit_edit();
                if (selected < lab_workbook_sheet_count(g_workbook)) activate_sheet(selected);
            }
        } else if (y >= 24 && y < 50) {
            int menu = menu_from_x(x);
            if (g_editing) commit_edit();
            if (menu != MENU_NONE) open_menu(menu);
        } else if (g_menu_open != MENU_NONE &&
                   x >= menu_x(g_menu_open) && x < menu_x(g_menu_open) + 190 &&
                   y >= 50 && y < 54 + menu_count(g_menu_open) * 26) {
            int item = (y - 54) / 26;
            if (item >= 0 && item < menu_count(g_menu_open)) {
                g_menu_selection = item;
                activate_menu();
            }
        } else if (g_toolbar_hover >= 0) {
            close_menu();
            if (g_editing) commit_edit();
            activate_toolbar(g_toolbar_hover);
        } else {
            close_menu();
            select_from_mouse(x, y);
        }
    }
    return moved || buttons != old_buttons || old_toolbar != g_toolbar_hover;
}

static unsigned keyboard_shift_flags(void)
{
    union REGS in_regs;
    union REGS out_regs;
    struct SREGS seg_regs;
    memset(&in_regs, 0, sizeof(in_regs));
    memset(&out_regs, 0, sizeof(out_regs));
    segread(&seg_regs);
    in_regs.w.ax = 0x0200;
    int386x(0x16, &in_regs, &out_regs, &seg_regs);
    return out_regs.h.al;
}

static int read_key_nonblocking(int *key, int *scan)
{
    unsigned short key_data = _bios_keybrd(_KEYBRD_READY);
    if (key_data == 0) return 0;
    key_data = _bios_keybrd(_KEYBRD_READ);
    *key = key_data & 0xff;
    *scan = (key_data >> 8) & 0xff;
    return 1;
}

static void append_edit(int key)
{
    if (g_edit_bytes + 1 >= EDIT_CAPACITY) {
        status("CELL INPUT LIMIT REACHED", 1);
        return;
    }
    g_edit[g_edit_bytes++] = (char)key;
    g_edit[g_edit_bytes] = '\0';
}

static void handle_scan(int scan, unsigned flags)
{
    if (g_open_pending || g_confirm_pending) return;
    if ((flags & 8) && scan == 62) { g_running = 0; return; }
    /* DOS returns alternate extended scan codes for Ctrl+Page Up/Down on
       some BIOSes (including QEMU). Page Up/Down have no other grid action. */
    if (scan == 73 || scan == 132) {
        u32 active = lab_workbook_active_index(g_workbook);
        if (active > 0) activate_sheet(active - 1);
        return;
    }
    if (scan == 81 || scan == 145) {
        u32 active = lab_workbook_active_index(g_workbook);
        if (active + 1 < lab_workbook_sheet_count(g_workbook)) activate_sheet(active + 1);
        return;
    }
    if (g_menu_open != MENU_NONE) {
        if (scan == 72 && g_menu_selection > 0) --g_menu_selection;
        else if (scan == 80 && g_menu_selection + 1 < menu_count(g_menu_open))
            ++g_menu_selection;
        else if (scan == 75 && g_menu_open > MENU_FILE) open_menu(g_menu_open - 1);
        else if (scan == 77 && g_menu_open < MENU_HELP) open_menu(g_menu_open + 1);
        return;
    }
    if (g_editing) {
        if (scan == 75 && g_edit_bytes) {
            g_edit[--g_edit_bytes] = '\0';
        } else if (scan == 28) {
            commit_edit();
            move_selection(1, 0);
        }
        return;
    }
    if (scan == 60) { begin_edit(0); return; }
    if (scan == 68) { open_menu(MENU_FILE); return; }
    if (scan == 72) move_selection(-1, 0);
    else if (scan == 80) move_selection(1, 0);
    else if (scan == 75) move_selection(0, -1);
    else if (scan == 77) move_selection(0, 1);
}

static void handle_key(int key)
{
    unsigned flags = keyboard_shift_flags();
    if (g_confirm_pending) {
        if (key == 27) answer_unsaved(0);
        else if (key == 'y' || key == 'Y') answer_unsaved(1);
        else if (key == 'n' || key == 'N') answer_unsaved(2);
        return;
    }
    if (g_open_pending) {
        unsigned bytes = (unsigned)strlen(g_open_path);
        if (key == 27) { g_open_pending = 0; g_path_mode = 0; status("PATH ENTRY CANCELLED", 0); }
        else if (key == 13) {
            g_open_pending = 0;
            if (g_path_mode == PATH_SAVE_AS) save_as_path();
            else if (g_path_mode == PATH_RENAME_SHEET) rename_active_worksheet();
            else if (g_path_mode == PATH_IMPORT_DELIMITED) import_delimited_path();
            else if (g_path_mode == PATH_EXPORT_DELIMITED) export_delimited_path();
            else load_workbook_path();
            if (!g_open_pending) g_path_mode = 0;
        }
        else if (key == 8 && bytes != 0) g_open_path[bytes - 1] = '\0';
        else if (key >= 32 && key <= 255 && bytes + 1 < sizeof(g_open_path)) {
            g_open_path[bytes] = (char)key; g_open_path[bytes + 1] = '\0';
        }
        return;
    }
    /* DOS character input may translate Ctrl+S/O/N to control bytes 19/15/14
       after the modifier has already been released by the BIOS queue. */
    if (key == 19 || ((flags & 4) && (key == 's' || key == 'S'))) {
        if (g_menu_open != MENU_NONE) close_menu();
        if (g_editing) commit_edit();
        save_workbook();
        return;
    }
    if (key == 26 || ((flags & 4) && (key == 'z' || key == 'Z'))) {
        if (g_menu_open != MENU_NONE) close_menu();
        apply_history(0);
        return;
    }
    if (key == 25 || ((flags & 4) && (key == 'y' || key == 'Y'))) {
        if (g_menu_open != MENU_NONE) close_menu();
        apply_history(1);
        return;
    }
    if (key == 15 || ((flags & 4) && (key == 'o' || key == 'O'))) {
        if (g_menu_open != MENU_NONE) close_menu();
        begin_open_workbook();
        return;
    }
    if (key == 14 || ((flags & 4) && (key == 'n' || key == 'N'))) {
        if (g_menu_open != MENU_NONE) close_menu();
        request_new_workbook();
        return;
    }
    if (g_menu_open != MENU_NONE) {
        if (key == 27) close_menu();
        else if (key == 13) activate_menu();
        else if (flags & 4) close_menu();
        else if (key == 'w' || key == 'W') open_menu(MENU_FILE);
        else if (key == 'e' || key == 'E') open_menu(MENU_EDIT);
        else if (key == 'v' || key == 'V') open_menu(MENU_VIEW);
        else if (key == 'i' || key == 'I') open_menu(MENU_INSERT);
        else if (key == 'f' || key == 'F') open_menu(MENU_FORMAT);
        else if (key == 'd' || key == 'D') open_menu(MENU_DATA);
        else if (key == 'h' || key == 'H') open_menu(MENU_HELP);
        return;
    }
    if (g_editing) {
        if (key == 27) cancel_edit();
        else if (key == 13) { commit_edit(); move_selection(1, 0); }
        else if (key == 8 && g_edit_bytes) g_edit[--g_edit_bytes] = '\0';
        else if (key >= 32 && key <= 255) append_edit(key);
        return;
    }
    if (key == 27) request_exit();
    else if (key == 13 || key == 0) begin_edit(0);
    else if (key == 9) move_selection(0, 1);
    else if (key >= 32 && key <= 255) { begin_edit(1); append_edit(key); }
}

int main(void)
{
    int changed;
    int key;
    int scan;
    const char *lfn_ready;
    g_workbook = lab_workbook_create();
    g_sheet = lab_workbook_active_sheet(g_workbook);
    g_edit = (char *)malloc(EDIT_CAPACITY);
    if (g_workbook == NULL || g_sheet == NULL || g_edit == NULL) {
        puts("Panthera Lab: not enough memory.");
        if (g_workbook) lab_workbook_destroy(g_workbook);
        if (g_edit) free(g_edit);
        return 2;
    }
    g_edit[0] = '\0';
    pw_platform_font_init_fallback(g_font);
    pw_platform_font_load_bios(g_font);
    if (!pw_platform_video_start()) {
        printf("Panthera Lab video init failed: %s\n", pw_platform_video_error());
        lab_workbook_destroy(g_workbook);
        free(g_edit);
        return 3;
    }
    if (!pw_platform_video_set_palette_entry(PW_PLATFORM_COLOR_LAB_TITLE, 0, 96, 0)) {
        pw_platform_video_stop();
        lab_workbook_destroy(g_workbook);
        free(g_edit);
        puts("Panthera Lab video palette setup failed.");
        return 4;
    }
    g_mouse_available = pw_platform_mouse_initialize();
    lfn_ready = getenv("PANTHERA_LFN_READY");
    g_lfn_driver_ready = lfn_ready != NULL && strcmp(lfn_ready, "1") == 0;
    if (!g_lfn_driver_ready) status("LOAD DOSLFN TO SAVE .XLSX", 1);
    else status(g_mouse_available ? "MOUSE READY  |  XLSX SAVE AVAILABLE" :
                "KEYBOARD READY  |  XLSX SAVE AVAILABLE", 0);
    draw_ui();
    while (g_running) {
        changed = poll_mouse();
        if (read_key_nonblocking(&key, &scan)) {
            if (key != 0 && key != 0xe0) scan = -1;
            if (scan >= 0) handle_scan(scan, keyboard_shift_flags());
            else handle_key(key);
            changed = 1;
        }
        if (changed) draw_ui();
    }
    pw_platform_mouse_restore();
    pw_platform_video_stop();
    lab_workbook_destroy(g_workbook);
    free(g_edit);
    return 0;
}
