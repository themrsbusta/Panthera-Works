#ifndef PANTHERA_LAB_MODEL_H
#define PANTHERA_LAB_MODEL_H

typedef unsigned char LabU8;
typedef unsigned short LabU16;
typedef unsigned long LabU32;

enum {
    LAB_MAX_ROWS = 1048576UL,
    LAB_MAX_COLUMNS = 16384,
    LAB_MAX_CELL_INPUT = 32767
};

enum {
    LAB_CELL_EMPTY = 0,
    LAB_CELL_TEXT = 1,
    LAB_CELL_NUMBER = 2,
    LAB_CELL_FORMULA = 3
};

enum {
    LAB_MODEL_OK = 0,
    LAB_MODEL_BAD_ARGUMENT = 1,
    LAB_MODEL_OUT_OF_RANGE = 2,
    LAB_MODEL_NO_MEMORY = 3,
    LAB_MODEL_INPUT_TOO_LONG = 4
};

enum {
    LAB_WORKBOOK_BAD_ARGUMENT = 20,
    LAB_WORKBOOK_NO_MEMORY = 21,
    LAB_WORKBOOK_BAD_NAME = 22,
    LAB_WORKBOOK_DUPLICATE_NAME = 23,
    LAB_WORKBOOK_LAST_SHEET = 24,
    LAB_WORKBOOK_OUT_OF_RANGE = 25,
    LAB_WORKBOOK_NOTHING_TO_UNDO = 26,
    LAB_WORKBOOK_NOTHING_TO_REDO = 27
};

enum { LAB_WORKSHEET_NAME_BYTES = 31 };

enum {
    LAB_CALC_NONE = 0,
    LAB_CALC_VALUE = 1,
    LAB_CALC_REF = 2,
    LAB_CALC_DIV_ZERO = 3,
    LAB_CALC_CYCLE = 4,
    LAB_CALC_NAME = 5,
    LAB_CALC_SYNTAX = 6,
    LAB_CALC_LIMIT = 7
};

typedef struct {
    LabU32 row;
    LabU16 column;
    LabU8 kind;
    LabU8 calc_state;
    LabU8 calc_error;
    char *input;
    char *display;
    double number;
} LabCell;

typedef struct LabSheet LabSheet;
typedef struct LabWorkbook LabWorkbook;
typedef int (*LabCellVisitor)(const LabCell *cell, void *context);

LabSheet *lab_sheet_create(void);
void lab_sheet_destroy(LabSheet *sheet);
LabU32 lab_sheet_count(const LabSheet *sheet);
const LabCell *lab_sheet_get(const LabSheet *sheet, LabU32 row, LabU16 column);
LabCell *lab_sheet_get_mutable(LabSheet *sheet, LabU32 row, LabU16 column);
const LabCell *lab_sheet_at(const LabSheet *sheet, LabU32 ordinal);
int lab_sheet_visit(const LabSheet *sheet, LabCellVisitor visitor, void *context);
int lab_sheet_set_input(LabSheet *sheet, LabU32 row, LabU16 column,
                        const char *input);
int lab_sheet_set_text(LabSheet *sheet, LabU32 row, LabU16 column,
                       const char *text);
int lab_sheet_recalculate(LabSheet *sheet);
const char *lab_cell_display(const LabCell *cell);

LabWorkbook *lab_workbook_create(void);
LabWorkbook *lab_workbook_create_empty(void);
void lab_workbook_destroy(LabWorkbook *workbook);
LabU32 lab_workbook_sheet_count(const LabWorkbook *workbook);
LabU32 lab_workbook_active_index(const LabWorkbook *workbook);
LabSheet *lab_workbook_active_sheet(LabWorkbook *workbook);
LabSheet *lab_workbook_sheet(LabWorkbook *workbook, LabU32 index);
const char *lab_workbook_sheet_name(const LabWorkbook *workbook, LabU32 index);
int lab_workbook_set_active(LabWorkbook *workbook, LabU32 index);
int lab_workbook_add_sheet(LabWorkbook *workbook, const char *name, LabU32 *index);
int lab_workbook_rename_sheet(LabWorkbook *workbook, LabU32 index, const char *name);
int lab_workbook_remove_sheet(LabWorkbook *workbook, LabU32 index);
int lab_workbook_set_cell_input(LabWorkbook *workbook, LabU32 sheet_index,
                                LabU32 row, LabU16 column, const char *input);
int lab_workbook_undo(LabWorkbook *workbook, LabU32 *sheet_index,
                      LabU32 *row, LabU16 *column);
int lab_workbook_redo(LabWorkbook *workbook, LabU32 *sheet_index,
                      LabU32 *row, LabU16 *column);
int lab_workbook_can_undo(const LabWorkbook *workbook);
int lab_workbook_can_redo(const LabWorkbook *workbook);
int lab_workbook_set_sheet_selection(LabWorkbook *workbook, LabU32 sheet_index,
                                     LabU32 row, LabU16 column);
int lab_workbook_get_sheet_selection(const LabWorkbook *workbook, LabU32 sheet_index,
                                     LabU32 *row, LabU16 *column);

int lab_column_parse(const char *letters, LabU16 *column, unsigned *consumed);
int lab_column_format(LabU16 column, char *output, unsigned output_bytes);
int lab_cell_reference_parse(const char *text, LabU32 *row, LabU16 *column);
int lab_cell_reference_format(LabU32 row, LabU16 column,
                              char *output, unsigned output_bytes);

#endif
