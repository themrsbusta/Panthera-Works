#include <stdlib.h>
#include <string.h>

#include "lab_model.h"

enum { LAB_HISTORY_MAX_ENTRIES = 64 };
#define LAB_HISTORY_MAX_BYTES (1024UL * 1024UL)

struct LabSheet {
    LabCell *cells;
    LabU8 *states;
    LabU32 capacity;
    LabU32 count;
    LabU32 deleted;
};

typedef struct {
    char name[LAB_WORKSHEET_NAME_BYTES + 1];
    LabSheet *sheet;
    LabU32 active_row;
    LabU16 active_column;
} LabWorksheet;

typedef struct {
    LabSheet *sheet;
    LabU32 row;
    LabU16 column;
    LabU32 bytes;
    char *before;
    char *after;
} LabHistoryEntry;

struct LabWorkbook {
    LabWorksheet *worksheets;
    LabU32 count;
    LabU32 capacity;
    LabU32 active;
    LabHistoryEntry history[LAB_HISTORY_MAX_ENTRIES];
    LabU32 history_count;
    LabU32 history_cursor;
    LabU32 history_bytes;
};

enum { SLOT_EMPTY = 0, SLOT_USED = 1, SLOT_DELETED = 2 };

static LabU32 hash_cell(LabU32 row, LabU16 column)
{
    LabU32 value = row + 0x9e3779b9UL;
    value ^= (LabU32)column + (value << 6) + (value >> 2);
    value ^= value >> 16;
    value *= 0x7feb352dUL;
    value ^= value >> 15;
    return value;
}

static int number_from_text(const char *text, double *number)
{
    const unsigned char *cursor = (const unsigned char *)text;
    int negative = 0;
    int saw_digit = 0;
    int fractional_digits = 0;
    int exponent = 0;
    double value = 0.0;
    double scale = 1.0;
    if (*cursor == '+' || *cursor == '-') {
        negative = *cursor == '-';
        ++cursor;
    }
    while (*cursor >= '0' && *cursor <= '9') {
        saw_digit = 1;
        value = value * 10.0 + (*cursor - '0');
        ++cursor;
    }
    if (*cursor == '.') {
        ++cursor;
        while (*cursor >= '0' && *cursor <= '9') {
            saw_digit = 1;
            value = value * 10.0 + (*cursor - '0');
            ++fractional_digits;
            ++cursor;
        }
    }
    if (!saw_digit) return 0;
    while (fractional_digits-- > 0) scale *= 10.0;
    if (*cursor == 'e' || *cursor == 'E') {
        int exponent_negative = 0;
        int exponent_digits = 0;
        ++cursor;
        if (*cursor == '+' || *cursor == '-') {
            exponent_negative = *cursor == '-';
            ++cursor;
        }
        while (*cursor >= '0' && *cursor <= '9') {
            if (exponent < 308) exponent = exponent * 10 + (*cursor - '0');
            ++cursor;
            ++exponent_digits;
        }
        if (exponent_digits == 0) return 0;
        if (exponent > 308) exponent = 308;
        if (exponent_negative) exponent = -exponent;
    }
    if (*cursor != '\0') return 0;
    value /= scale;
    if (exponent > 0) while (exponent-- > 0) value *= 10.0;
    else while (exponent++ < 0) value /= 10.0;
    *number = negative ? -value : value;
    return 1;
}

static void cell_release(LabCell *cell)
{
    if (cell->input != NULL) free(cell->input);
    if (cell->display != NULL) free(cell->display);
    memset(cell, 0, sizeof(*cell));
}

static void invalidate_formula_cache(LabSheet *sheet)
{
    LabU32 i;
    for (i = 0; i < sheet->capacity; ++i) {
        LabCell *cell;
        if (sheet->states[i] != SLOT_USED) continue;
        cell = &sheet->cells[i];
        if (cell->kind != LAB_CELL_FORMULA) continue;
        if (cell->display != NULL) {
            free(cell->display);
            cell->display = NULL;
        }
        cell->calc_state = 0;
        cell->calc_error = LAB_CALC_NONE;
    }
}

static int locate_slot(const LabSheet *sheet, LabU32 row, LabU16 column,
                       LabU32 *slot_out, int *found_out)
{
    LabU32 mask;
    LabU32 slot;
    LabU32 first_deleted = 0xffffffffUL;
    LabU32 probes;
    if (sheet == NULL || sheet->capacity == 0 || slot_out == NULL || found_out == NULL)
        return 0;
    mask = sheet->capacity - 1;
    slot = hash_cell(row, column) & mask;
    for (probes = 0; probes < sheet->capacity; ++probes) {
        if (sheet->states[slot] == SLOT_EMPTY) {
            *slot_out = first_deleted != 0xffffffffUL ? first_deleted : slot;
            *found_out = 0;
            return 1;
        }
        if (sheet->states[slot] == SLOT_DELETED) {
            if (first_deleted == 0xffffffffUL) first_deleted = slot;
        } else if (sheet->cells[slot].row == row && sheet->cells[slot].column == column) {
            *slot_out = slot;
            *found_out = 1;
            return 1;
        }
        slot = (slot + 1) & mask;
    }
    if (first_deleted != 0xffffffffUL) {
        *slot_out = first_deleted;
        *found_out = 0;
        return 1;
    }
    return 0;
}

static int sheet_rehash(LabSheet *sheet, LabU32 capacity)
{
    LabCell *new_cells;
    LabU8 *new_states;
    LabU32 i;
    if (capacity < 16 || (capacity & (capacity - 1)) != 0 ||
        capacity > 0x7fffffffUL / (LabU32)sizeof(LabCell)) return 0;
    new_cells = (LabCell *)calloc(capacity, sizeof(LabCell));
    new_states = (LabU8 *)calloc(capacity, sizeof(LabU8));
    if (new_cells == NULL || new_states == NULL) {
        if (new_cells != NULL) free(new_cells);
        if (new_states != NULL) free(new_states);
        return 0;
    }
    for (i = 0; i < sheet->capacity; ++i) {
        if (sheet->states[i] == SLOT_USED) {
            LabU32 slot = hash_cell(sheet->cells[i].row, sheet->cells[i].column) & (capacity - 1);
            while (new_states[slot] == SLOT_USED) slot = (slot + 1) & (capacity - 1);
            new_cells[slot] = sheet->cells[i];
            new_states[slot] = SLOT_USED;
        }
    }
    if (sheet->cells != NULL) free(sheet->cells);
    if (sheet->states != NULL) free(sheet->states);
    sheet->cells = new_cells;
    sheet->states = new_states;
    sheet->capacity = capacity;
    sheet->deleted = 0;
    return 1;
}

LabSheet *lab_sheet_create(void)
{
    LabSheet *sheet = (LabSheet *)calloc(1, sizeof(LabSheet));
    if (sheet == NULL) return NULL;
    if (!sheet_rehash(sheet, 16)) {
        free(sheet);
        return NULL;
    }
    return sheet;
}

void lab_sheet_destroy(LabSheet *sheet)
{
    LabU32 i;
    if (sheet == NULL) return;
    for (i = 0; i < sheet->capacity; ++i)
        if (sheet->states[i] == SLOT_USED) cell_release(&sheet->cells[i]);
    free(sheet->cells);
    free(sheet->states);
    free(sheet);
}

static unsigned char name_fold(unsigned char value)
{
    if (value >= 'A' && value <= 'Z') return (unsigned char)(value + ('a' - 'A'));
    return value;
}

static int worksheet_name_valid(const char *name)
{
    unsigned bytes = 0;
    const unsigned char *p = (const unsigned char *)name;
    if (name == NULL || *name == '\0') return 0;
    while (*p) {
        if (*p < 32 || *p == ':' || *p == '\\' || *p == '/' || *p == '?' ||
            *p == '*' || *p == '[' || *p == ']') return 0;
        if (++bytes > LAB_WORKSHEET_NAME_BYTES) return 0;
        ++p;
    }
    return 1;
}

static int workbook_name_exists(const LabWorkbook *workbook, const char *name,
                                LabU32 except)
{
    LabU32 i;
    for (i = 0; i < workbook->count; ++i) {
        const unsigned char *a;
        const unsigned char *b;
        if (i == except) continue;
        a = (const unsigned char *)workbook->worksheets[i].name;
        b = (const unsigned char *)name;
        while (*a && *b && name_fold(*a) == name_fold(*b)) { ++a; ++b; }
        if (*a == 0 && *b == 0) return 1;
    }
    return 0;
}

LabWorkbook *lab_workbook_create_empty(void)
{
    return (LabWorkbook *)calloc(1, sizeof(LabWorkbook));
}

LabWorkbook *lab_workbook_create(void)
{
    LabWorkbook *workbook = lab_workbook_create_empty();
    if (workbook == NULL) return NULL;
    if (lab_workbook_add_sheet(workbook, "Sheet1", NULL) != LAB_MODEL_OK) {
        lab_workbook_destroy(workbook);
        return NULL;
    }
    return workbook;
}

void lab_workbook_destroy(LabWorkbook *workbook)
{
    LabU32 i;
    if (workbook == NULL) return;
    for (i = 0; i < workbook->history_count; ++i) {
        free(workbook->history[i].before);
        free(workbook->history[i].after);
    }
    for (i = 0; i < workbook->count; ++i)
        lab_sheet_destroy(workbook->worksheets[i].sheet);
    free(workbook->worksheets);
    free(workbook);
}

LabU32 lab_workbook_sheet_count(const LabWorkbook *workbook)
{
    return workbook != NULL ? workbook->count : 0;
}

LabU32 lab_workbook_active_index(const LabWorkbook *workbook)
{
    return workbook != NULL ? workbook->active : 0;
}

LabSheet *lab_workbook_active_sheet(LabWorkbook *workbook)
{
    return workbook != NULL && workbook->active < workbook->count ?
        workbook->worksheets[workbook->active].sheet : NULL;
}

LabSheet *lab_workbook_sheet(LabWorkbook *workbook, LabU32 index)
{
    return workbook != NULL && index < workbook->count ?
        workbook->worksheets[index].sheet : NULL;
}

const char *lab_workbook_sheet_name(const LabWorkbook *workbook, LabU32 index)
{
    return workbook != NULL && index < workbook->count ?
        workbook->worksheets[index].name : NULL;
}

int lab_workbook_set_active(LabWorkbook *workbook, LabU32 index)
{
    if (workbook == NULL) return LAB_WORKBOOK_BAD_ARGUMENT;
    if (index >= workbook->count) return LAB_WORKBOOK_OUT_OF_RANGE;
    workbook->active = index;
    return LAB_MODEL_OK;
}

int lab_workbook_add_sheet(LabWorkbook *workbook, const char *name, LabU32 *index)
{
    LabWorksheet *next;
    LabSheet *sheet;
    LabU32 capacity;
    if (workbook == NULL || name == NULL) return LAB_WORKBOOK_BAD_ARGUMENT;
    if (!worksheet_name_valid(name)) return LAB_WORKBOOK_BAD_NAME;
    if (workbook_name_exists(workbook, name, 0xffffffffUL)) return LAB_WORKBOOK_DUPLICATE_NAME;
    sheet = lab_sheet_create();
    if (sheet == NULL) return LAB_WORKBOOK_NO_MEMORY;
    if (workbook->count == workbook->capacity) {
        capacity = workbook->capacity == 0 ? 4 : workbook->capacity * 2;
        if (capacity < workbook->capacity ||
            capacity > 0x7fffffffUL / (LabU32)sizeof(LabWorksheet)) {
            lab_sheet_destroy(sheet);
            return LAB_WORKBOOK_NO_MEMORY;
        }
        next = (LabWorksheet *)realloc(workbook->worksheets,
                                       capacity * sizeof(LabWorksheet));
        if (next == NULL) {
            lab_sheet_destroy(sheet);
            return LAB_WORKBOOK_NO_MEMORY;
        }
        workbook->worksheets = next;
        workbook->capacity = capacity;
    }
    memset(&workbook->worksheets[workbook->count], 0, sizeof(LabWorksheet));
    strcpy(workbook->worksheets[workbook->count].name, name);
    workbook->worksheets[workbook->count].sheet = sheet;
    if (index != NULL) *index = workbook->count;
    workbook->active = workbook->count++;
    return LAB_MODEL_OK;
}

int lab_workbook_rename_sheet(LabWorkbook *workbook, LabU32 index,
                              const char *name)
{
    if (workbook == NULL || name == NULL) return LAB_WORKBOOK_BAD_ARGUMENT;
    if (index >= workbook->count) return LAB_WORKBOOK_OUT_OF_RANGE;
    if (!worksheet_name_valid(name)) return LAB_WORKBOOK_BAD_NAME;
    if (workbook_name_exists(workbook, name, index)) return LAB_WORKBOOK_DUPLICATE_NAME;
    strcpy(workbook->worksheets[index].name, name);
    return LAB_MODEL_OK;
}

int lab_workbook_remove_sheet(LabWorkbook *workbook, LabU32 index)
{
    LabU32 i;
    if (workbook == NULL) return LAB_WORKBOOK_BAD_ARGUMENT;
    if (index >= workbook->count) return LAB_WORKBOOK_OUT_OF_RANGE;
    if (workbook->count <= 1) return LAB_WORKBOOK_LAST_SHEET;
    /* Cell history keeps worksheet addresses; a structural removal invalidates it. */
    for (i = 0; i < workbook->history_count; ++i) {
        free(workbook->history[i].before);
        free(workbook->history[i].after);
    }
    workbook->history_count = 0;
    workbook->history_cursor = 0;
    workbook->history_bytes = 0;
    memset(workbook->history, 0, sizeof(workbook->history));
    lab_sheet_destroy(workbook->worksheets[index].sheet);
    if (index + 1 < workbook->count)
        memmove(&workbook->worksheets[index], &workbook->worksheets[index + 1],
                (workbook->count - index - 1) * sizeof(LabWorksheet));
    --workbook->count;
    if (workbook->active > index) --workbook->active;
    else if (workbook->active >= workbook->count) workbook->active = workbook->count - 1;
    return LAB_MODEL_OK;
}

static char *history_copy(const char *value)
{
    size_t bytes = strlen(value) + 1;
    char *copy = (char *)malloc(bytes);
    if (copy != NULL) memcpy(copy, value, bytes);
    return copy;
}

static void history_release_entry(LabHistoryEntry *entry)
{
    if (entry->before != NULL) free(entry->before);
    if (entry->after != NULL) free(entry->after);
    memset(entry, 0, sizeof(*entry));
}

static void history_remove_first(LabWorkbook *workbook)
{
    if (workbook->history_count == 0) return;
    if (workbook->history_bytes >= workbook->history[0].bytes)
        workbook->history_bytes -= workbook->history[0].bytes;
    else workbook->history_bytes = 0;
    history_release_entry(&workbook->history[0]);
    if (workbook->history_count > 1)
        memmove(&workbook->history[0], &workbook->history[1],
                (workbook->history_count - 1) * sizeof(LabHistoryEntry));
    --workbook->history_count;
    memset(&workbook->history[workbook->history_count], 0, sizeof(LabHistoryEntry));
    if (workbook->history_cursor != 0) --workbook->history_cursor;
}

static void history_discard_redo(LabWorkbook *workbook)
{
    while (workbook->history_count > workbook->history_cursor) {
        LabHistoryEntry *entry = &workbook->history[--workbook->history_count];
        if (workbook->history_bytes >= entry->bytes)
            workbook->history_bytes -= entry->bytes;
        else workbook->history_bytes = 0;
        history_release_entry(entry);
    }
}

int lab_workbook_set_cell_input(LabWorkbook *workbook, LabU32 sheet_index,
                                LabU32 row, LabU16 column, const char *input)
{
    LabSheet *sheet;
    const LabCell *old_cell;
    const char *old_input;
    LabHistoryEntry entry;
    size_t old_bytes;
    size_t new_bytes;
    int result;
    if (workbook == NULL || input == NULL) return LAB_MODEL_BAD_ARGUMENT;
    if (sheet_index >= workbook->count) return LAB_WORKBOOK_OUT_OF_RANGE;
    if (row >= LAB_MAX_ROWS || column >= LAB_MAX_COLUMNS) return LAB_MODEL_OUT_OF_RANGE;
    if (strlen(input) > LAB_MAX_CELL_INPUT) return LAB_MODEL_INPUT_TOO_LONG;
    sheet = workbook->worksheets[sheet_index].sheet;
    old_cell = lab_sheet_get(sheet, row, column);
    old_input = old_cell != NULL && old_cell->input != NULL ? old_cell->input : "";
    if (strcmp(old_input, input) == 0) return LAB_MODEL_OK;
    old_bytes = strlen(old_input) + 1;
    new_bytes = strlen(input) + 1;
    memset(&entry, 0, sizeof(entry));
    entry.sheet = sheet;
    entry.row = row;
    entry.column = column;
    entry.bytes = (LabU32)(old_bytes + new_bytes);
    entry.before = history_copy(old_input);
    entry.after = history_copy(input);
    if (entry.before == NULL || entry.after == NULL) {
        history_release_entry(&entry);
        return LAB_WORKBOOK_NO_MEMORY;
    }
    if (entry.bytes > LAB_HISTORY_MAX_BYTES) {
        history_release_entry(&entry);
        return LAB_WORKBOOK_NO_MEMORY;
    }
    /* Reserve the undo record before changing the cell so allocation failure is atomic. */
    result = lab_sheet_set_input(sheet, row, column, input);
    if (result != LAB_MODEL_OK) {
        history_release_entry(&entry);
        return result;
    }
    workbook->active = sheet_index;
    workbook->worksheets[sheet_index].active_row = row;
    workbook->worksheets[sheet_index].active_column = column;
    history_discard_redo(workbook);
    while (workbook->history_count >= LAB_HISTORY_MAX_ENTRIES ||
           workbook->history_bytes + entry.bytes > LAB_HISTORY_MAX_BYTES)
        history_remove_first(workbook);
    workbook->history[workbook->history_count++] = entry;
    workbook->history_cursor = workbook->history_count;
    workbook->history_bytes += entry.bytes;
    return LAB_MODEL_OK;
}

static LabU32 workbook_find_sheet(const LabWorkbook *workbook, const LabSheet *sheet)
{
    LabU32 i;
    for (i = 0; i < workbook->count; ++i)
        if (workbook->worksheets[i].sheet == sheet) return i;
    return 0xffffffffUL;
}

static int history_apply(LabWorkbook *workbook, int redo,
                         LabU32 *sheet_index, LabU32 *row, LabU16 *column)
{
    LabHistoryEntry *entry;
    LabU32 index;
    int result;
    if (workbook == NULL || sheet_index == NULL || row == NULL || column == NULL)
        return LAB_WORKBOOK_BAD_ARGUMENT;
    if ((!redo && workbook->history_cursor == 0) ||
        (redo && workbook->history_cursor >= workbook->history_count))
        return redo ? LAB_WORKBOOK_NOTHING_TO_REDO : LAB_WORKBOOK_NOTHING_TO_UNDO;
    index = redo ? workbook->history_cursor : workbook->history_cursor - 1;
    entry = &workbook->history[index];
    *sheet_index = workbook_find_sheet(workbook, entry->sheet);
    if (*sheet_index == 0xffffffffUL) return LAB_WORKBOOK_OUT_OF_RANGE;
    result = lab_sheet_set_input(entry->sheet, entry->row, entry->column,
                                 redo ? entry->after : entry->before);
    if (result != LAB_MODEL_OK) return result;
    if (redo) ++workbook->history_cursor;
    else --workbook->history_cursor;
    workbook->active = *sheet_index;
    workbook->worksheets[*sheet_index].active_row = entry->row;
    workbook->worksheets[*sheet_index].active_column = entry->column;
    *row = entry->row;
    *column = entry->column;
    return LAB_MODEL_OK;
}

int lab_workbook_undo(LabWorkbook *workbook, LabU32 *sheet_index,
                      LabU32 *row, LabU16 *column)
{
    return history_apply(workbook, 0, sheet_index, row, column);
}

int lab_workbook_redo(LabWorkbook *workbook, LabU32 *sheet_index,
                      LabU32 *row, LabU16 *column)
{
    return history_apply(workbook, 1, sheet_index, row, column);
}

int lab_workbook_can_undo(const LabWorkbook *workbook)
{
    return workbook != NULL && workbook->history_cursor != 0;
}

int lab_workbook_can_redo(const LabWorkbook *workbook)
{
    return workbook != NULL && workbook->history_cursor < workbook->history_count;
}

int lab_workbook_set_sheet_selection(LabWorkbook *workbook, LabU32 sheet_index,
                                     LabU32 row, LabU16 column)
{
    if (workbook == NULL) return LAB_WORKBOOK_BAD_ARGUMENT;
    if (sheet_index >= workbook->count) return LAB_WORKBOOK_OUT_OF_RANGE;
    if (row >= LAB_MAX_ROWS || column >= LAB_MAX_COLUMNS) return LAB_MODEL_OUT_OF_RANGE;
    workbook->worksheets[sheet_index].active_row = row;
    workbook->worksheets[sheet_index].active_column = column;
    return LAB_MODEL_OK;
}

int lab_workbook_get_sheet_selection(const LabWorkbook *workbook, LabU32 sheet_index,
                                     LabU32 *row, LabU16 *column)
{
    if (workbook == NULL || row == NULL || column == NULL)
        return LAB_WORKBOOK_BAD_ARGUMENT;
    if (sheet_index >= workbook->count) return LAB_WORKBOOK_OUT_OF_RANGE;
    *row = workbook->worksheets[sheet_index].active_row;
    *column = workbook->worksheets[sheet_index].active_column;
    return LAB_MODEL_OK;
}

LabU32 lab_sheet_count(const LabSheet *sheet)
{
    return sheet != NULL ? sheet->count : 0;
}

const LabCell *lab_sheet_get(const LabSheet *sheet, LabU32 row, LabU16 column)
{
    LabU32 slot;
    int found;
    if (sheet == NULL || row >= LAB_MAX_ROWS || column >= LAB_MAX_COLUMNS ||
        !locate_slot(sheet, row, column, &slot, &found) || !found) return NULL;
    return &sheet->cells[slot];
}

LabCell *lab_sheet_get_mutable(LabSheet *sheet, LabU32 row, LabU16 column)
{
    LabU32 slot;
    int found;
    if (sheet == NULL || row >= LAB_MAX_ROWS || column >= LAB_MAX_COLUMNS ||
        !locate_slot(sheet, row, column, &slot, &found) || !found) return NULL;
    return &sheet->cells[slot];
}

const LabCell *lab_sheet_at(const LabSheet *sheet, LabU32 ordinal)
{
    LabU32 i;
    if (sheet == NULL || ordinal >= sheet->count) return NULL;
    for (i = 0; i < sheet->capacity; ++i) {
        if (sheet->states[i] == SLOT_USED) {
            if (ordinal == 0) return &sheet->cells[i];
            --ordinal;
        }
    }
    return NULL;
}

int lab_sheet_visit(const LabSheet *sheet, LabCellVisitor visitor, void *context)
{
    LabU32 i;
    if (sheet == NULL || visitor == NULL) return 0;
    for (i = 0; i < sheet->capacity; ++i) {
        if (sheet->states[i] == SLOT_USED &&
            !visitor(&sheet->cells[i], context)) return 0;
    }
    return 1;
}

static int sheet_set_value(LabSheet *sheet, LabU32 row, LabU16 column,
                           const char *input, int force_text)
{
    LabU32 slot;
    int found;
    size_t bytes;
    char *copy = NULL;
    double number = 0.0;
    LabU8 kind;
    if (sheet == NULL || input == NULL) return LAB_MODEL_BAD_ARGUMENT;
    if (row >= LAB_MAX_ROWS || column >= LAB_MAX_COLUMNS) return LAB_MODEL_OUT_OF_RANGE;
    bytes = strlen(input);
    if (bytes > LAB_MAX_CELL_INPUT) return LAB_MODEL_INPUT_TOO_LONG;
    if (!locate_slot(sheet, row, column, &slot, &found)) return LAB_MODEL_NO_MEMORY;
    if (bytes == 0) {
        if (!found) return LAB_MODEL_OK;
        cell_release(&sheet->cells[slot]);
        sheet->states[slot] = SLOT_DELETED;
        --sheet->count;
        ++sheet->deleted;
        invalidate_formula_cache(sheet);
        return LAB_MODEL_OK;
    }
    copy = (char *)malloc(bytes + 1);
    if (copy == NULL) return LAB_MODEL_NO_MEMORY;
    memcpy(copy, input, bytes + 1);
    if (force_text) kind = LAB_CELL_TEXT;
    else if (input[0] == '=') kind = LAB_CELL_FORMULA;
    else if (number_from_text(input, &number)) kind = LAB_CELL_NUMBER;
    else kind = LAB_CELL_TEXT;

    if (!found && sheet->count + sheet->deleted + 1 >= (sheet->capacity * 7) / 10) {
        LabU32 next_capacity = sheet->capacity << 1;
        if (next_capacity < sheet->capacity || !sheet_rehash(sheet, next_capacity)) {
            free(copy);
            return LAB_MODEL_NO_MEMORY;
        }
        if (!locate_slot(sheet, row, column, &slot, &found)) {
            free(copy);
            return LAB_MODEL_NO_MEMORY;
        }
    }
    if (found) {
        if (sheet->cells[slot].input != NULL) free(sheet->cells[slot].input);
        if (sheet->cells[slot].display != NULL) {
            free(sheet->cells[slot].display);
            sheet->cells[slot].display = NULL;
        }
    } else {
        if (sheet->states[slot] == SLOT_DELETED) --sheet->deleted;
        sheet->states[slot] = SLOT_USED;
        sheet->cells[slot].row = row;
        sheet->cells[slot].column = column;
        ++sheet->count;
    }
    sheet->cells[slot].kind = kind;
    sheet->cells[slot].calc_state = 0;
    sheet->cells[slot].calc_error = LAB_CALC_NONE;
    sheet->cells[slot].input = copy;
    sheet->cells[slot].number = kind == LAB_CELL_NUMBER ? number : 0.0;
    invalidate_formula_cache(sheet);
    return LAB_MODEL_OK;
}

int lab_sheet_set_input(LabSheet *sheet, LabU32 row, LabU16 column,
                        const char *input)
{
    return sheet_set_value(sheet, row, column, input, 0);
}

int lab_sheet_set_text(LabSheet *sheet, LabU32 row, LabU16 column,
                       const char *text)
{
    return sheet_set_value(sheet, row, column, text, 1);
}

const char *lab_cell_display(const LabCell *cell)
{
    if (cell == NULL) return "";
    if (cell->kind == LAB_CELL_FORMULA && cell->calc_state == 2 && cell->display != NULL)
        return cell->display;
    return cell->input != NULL ? cell->input : "";
}

int lab_column_parse(const char *letters, LabU16 *column, unsigned *consumed)
{
    LabU32 value = 0;
    unsigned count = 0;
    if (letters == NULL || column == NULL) return 0;
    while ((letters[count] >= 'A' && letters[count] <= 'Z') ||
           (letters[count] >= 'a' && letters[count] <= 'z')) {
        unsigned char c = (unsigned char)letters[count];
        unsigned digit = (c >= 'a' && c <= 'z') ? c - 'a' + 1 : c - 'A' + 1;
        if (value > (LAB_MAX_COLUMNS - digit) / 26) return 0;
        value = value * 26 + digit;
        ++count;
    }
    if (count == 0 || value == 0 || value > LAB_MAX_COLUMNS) return 0;
    *column = (LabU16)(value - 1);
    if (consumed != NULL) *consumed = count;
    return 1;
}

int lab_column_format(LabU16 column, char *output, unsigned output_bytes)
{
    char reverse[4];
    unsigned count = 0;
    unsigned i;
    LabU32 value = (LabU32)column + 1;
    if (output == NULL || output_bytes == 0 || column >= LAB_MAX_COLUMNS) return 0;
    while (value != 0 && count < sizeof(reverse)) {
        LabU32 remainder = (value - 1) % 26;
        reverse[count++] = (char)('A' + remainder);
        value = (value - 1) / 26;
    }
    if (value != 0 || count + 1 > output_bytes) return 0;
    for (i = 0; i < count; ++i) output[i] = reverse[count - i - 1];
    output[count] = '\0';
    return 1;
}

int lab_cell_reference_parse(const char *text, LabU32 *row, LabU16 *column)
{
    LabU16 parsed_column;
    unsigned letters;
    const char *cursor;
    LabU32 parsed_row = 0;
    if (text == NULL || row == NULL || column == NULL ||
        !lab_column_parse(text, &parsed_column, &letters)) return 0;
    cursor = text + letters;
    if (*cursor == '\0') return 0;
    while (*cursor >= '0' && *cursor <= '9') {
        unsigned digit = (unsigned)(*cursor - '0');
        if (parsed_row > (LAB_MAX_ROWS - digit) / 10) return 0;
        parsed_row = parsed_row * 10 + digit;
        ++cursor;
    }
    if (*cursor != '\0' || parsed_row == 0 || parsed_row > LAB_MAX_ROWS) return 0;
    *row = parsed_row - 1;
    *column = parsed_column;
    return 1;
}

int lab_cell_reference_format(LabU32 row, LabU16 column,
                              char *output, unsigned output_bytes)
{
    char letters[4];
    char digits[12];
    unsigned digit_count = 0;
    unsigned letter_count;
    LabU32 value;
    unsigned i;
    if (output == NULL || row >= LAB_MAX_ROWS || column >= LAB_MAX_COLUMNS ||
        output_bytes == 0 || !lab_column_format(column, letters, sizeof(letters))) return 0;
    value = row + 1;
    do {
        digits[digit_count++] = (char)('0' + value % 10);
        value /= 10;
    } while (value != 0 && digit_count < sizeof(digits));
    letter_count = (unsigned)strlen(letters);
    if (letter_count + digit_count + 1 > output_bytes) return 0;
    memcpy(output, letters, letter_count);
    for (i = 0; i < digit_count; ++i) output[letter_count + i] = digits[digit_count - i - 1];
    output[letter_count + digit_count] = '\0';
    return 1;
}
