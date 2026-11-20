#include "../src/lab_model.h"
#include "../src/lab_xlsx.h"
#include "../src/lab_delimited.h"

#include <stdio.h>
#include <string.h>

static int fail(const char *message)
{
    fprintf(stdout, "FAIL: %s\n", message);
    fprintf(stderr, "FAIL: %s\n", message);
    return 1;
}

static int test_delimited(void)
{
    FILE *file;
    LabWorkbook *workbook;
    LabWorkbook *roundtrip;
    LabWorkbook *original;
    LabSheet *sheet;
    const LabCell *cell;
    const char csv[] = "\xef\xbb\xbfName,Note,Formula\r\nJos\xc3\xa9,\"He said \"\"hi\"\"\nand left\",=1+2\r\n";
    const char malformed[] = "valid,\"unterminated\n";
    file = fopen("ROUNDTRI.CSV", "wb");
    if (file == NULL) return fail("create CSV import fixture");
    if (fwrite(csv, 1, sizeof(csv) - 1, file) != sizeof(csv) - 1 || fclose(file) != 0)
        return fail("write CSV import fixture");
    workbook = lab_workbook_create();
    if (workbook == NULL || lab_sheet_set_input(lab_workbook_active_sheet(workbook),
        0, 0, "OLD") != LAB_MODEL_OK) {
        lab_workbook_destroy(workbook);
        return fail("CSV initial workbook");
    }
    if (lab_delimited_load(&workbook, "ROUNDTRI.CSV", ',') != LAB_DELIMITED_OK) {
        lab_workbook_destroy(workbook);
        return fail("CSV UTF-8, BOM, quoting and multiline import");
    }
    sheet = lab_workbook_active_sheet(workbook);
    cell = lab_sheet_get(sheet, 1, 0);
    if (cell == NULL || strcmp(cell->input, "Jos\x82") != 0 ||
        strcmp(lab_sheet_get(sheet, 1, 1)->input, "He said \"hi\"\nand left") != 0 ||
        lab_sheet_get(sheet, 1, 2) == NULL ||
        lab_sheet_get(sheet, 1, 2)->kind != LAB_CELL_FORMULA ||
        lab_sheet_get(sheet, 1, 2)->number != 3.0 ||
        lab_sheet_get(sheet, 0, 0) == NULL ||
        strcmp(lab_sheet_get(sheet, 0, 0)->input, "Name") != 0) {
        lab_workbook_destroy(workbook);
        return fail("CSV imported field contents and formula evaluation");
    }
    if (lab_delimited_save(sheet, "ROUNDTRI.TSV", '\t') != LAB_DELIMITED_OK) {
        lab_workbook_destroy(workbook);
        return fail("TSV export");
    }
    roundtrip = lab_workbook_create();
    if (roundtrip == NULL ||
        lab_delimited_load(&roundtrip, "ROUNDTRI.TSV", '\t') != LAB_DELIMITED_OK) {
        lab_workbook_destroy(workbook);
        lab_workbook_destroy(roundtrip);
        return fail("TSV re-import");
    }
    cell = lab_sheet_get(lab_workbook_active_sheet(roundtrip), 1, 1);
    if (cell == NULL || strcmp(cell->input, "He said \"hi\"\nand left") != 0 ||
        lab_sheet_get(lab_workbook_active_sheet(roundtrip), 1, 2) == NULL ||
        lab_sheet_get(lab_workbook_active_sheet(roundtrip), 1, 2)->number != 3.0) {
        lab_workbook_destroy(workbook);
        lab_workbook_destroy(roundtrip);
        return fail("TSV round-trip content");
    }
    file = fopen("BROKEN.CSV", "wb");
    if (file == NULL || fwrite(malformed, 1, sizeof(malformed) - 1, file) != sizeof(malformed) - 1 ||
        fclose(file) != 0) {
        lab_workbook_destroy(workbook);
        lab_workbook_destroy(roundtrip);
        return fail("write malformed CSV fixture");
    }
    original = roundtrip;
    if (lab_delimited_load(&roundtrip, "BROKEN.CSV", ',') != LAB_DELIMITED_BAD_FORMAT ||
        roundtrip != original ||
        strcmp(lab_sheet_get(lab_workbook_active_sheet(roundtrip), 1, 0)->input, "Jos\x82") != 0) {
        lab_workbook_destroy(workbook);
        lab_workbook_destroy(roundtrip);
        return fail("malformed CSV rejection preserves existing workbook");
    }
    remove("ROUNDTRI.CSV");
    remove("ROUNDTRI.TSV");
    remove("ROUNDTRI.BAK");
    remove("BROKEN.CSV");
    lab_workbook_destroy(workbook);
    lab_workbook_destroy(roundtrip);
    return 0;
}

int main(void)
{
    LabSheet *sheet = lab_sheet_create();
    LabWorkbook *workbook;
    LabWorkbook *loaded;
    LabSheet *data_sheet;
    const LabCell *cell;
    LabU32 row;
    LabU16 column;
    LabU16 parsed_column;
    unsigned consumed;
    char reference[20];
    char name[4];
    char value[40];
    unsigned i;
    if (sheet == NULL) return fail("sparse sheet allocation");

    if (!lab_column_parse("XFD1048576", &parsed_column, &consumed) ||
        parsed_column != 16383 || consumed != 3) {
        lab_sheet_destroy(sheet);
        return fail("column parser boundary");
    }
    if (!lab_cell_reference_parse("XFD1048576", &row, &column) ||
        row != 1048575UL || column != 16383) {
        lab_sheet_destroy(sheet);
        return fail("maximum cell reference parse");
    }
    if (!lab_cell_reference_format(row, column, reference, sizeof(reference)) ||
        strcmp(reference, "XFD1048576") != 0) {
        lab_sheet_destroy(sheet);
        return fail("maximum cell reference format");
    }
    if (lab_cell_reference_parse("XFE1", &row, &column) ||
        lab_cell_reference_parse("A1048577", &row, &column) ||
        lab_cell_reference_parse("A0", &row, &column) ||
        lab_cell_reference_parse("1A", &row, &column)) {
        lab_sheet_destroy(sheet);
        return fail("out-of-range or malformed reference rejection");
    }
    if (!lab_column_format(26, name, sizeof(name)) || strcmp(name, "AA") != 0) {
        lab_sheet_destroy(sheet);
        return fail("column base-26 formatting");
    }

    if (lab_sheet_set_input(sheet, 0, 0, "42.125") != LAB_MODEL_OK ||
        lab_sheet_set_input(sheet, 0, 1, "=A1*2") != LAB_MODEL_OK ||
        lab_sheet_set_input(sheet, 0, 2, "=SUM(A1:B1)") != LAB_MODEL_OK ||
        lab_sheet_set_input(sheet, 0, 3, "=AVERAGE(A1:B1)") != LAB_MODEL_OK ||
        lab_sheet_set_input(sheet, 0, 4, "=POWER(A1,2)") != LAB_MODEL_OK ||
        lab_sheet_set_input(sheet, 0, 5, "=2+3*4") != LAB_MODEL_OK ||
        lab_sheet_set_input(sheet, 0, 6, "=-(2+3)^2") != LAB_MODEL_OK ||
        lab_sheet_set_input(sheet, 0, 7, "=1/0") != LAB_MODEL_OK ||
        lab_sheet_set_input(sheet, 0, 8, "=MISSING(1)") != LAB_MODEL_OK ||
        lab_sheet_set_input(sheet, 0, 9, "=C4+1") != LAB_MODEL_OK ||
        lab_sheet_set_input(sheet, 0, 10, "=L1+1") != LAB_MODEL_OK ||
        lab_sheet_set_input(sheet, 0, 11, "=K1+1") != LAB_MODEL_OK ||
        lab_sheet_set_input(sheet, 3, 2, "Panthera") != LAB_MODEL_OK ||
        lab_sheet_recalculate(sheet) != LAB_MODEL_OK) {
        lab_sheet_destroy(sheet);
        return fail("cell insertion or formula recalc");
    }
    cell = lab_sheet_get(sheet, 0, 0);
    if (cell == NULL || cell->kind != LAB_CELL_NUMBER || cell->number < 42.1249 ||
        cell->number > 42.1251 || strcmp(cell->input, "42.125") != 0) {
        lab_sheet_destroy(sheet);
        return fail("numeric cell classification");
    }
    cell = lab_sheet_get(sheet, 0, 1);
    if (cell == NULL || cell->kind != LAB_CELL_FORMULA || strcmp(cell->input, "=A1*2") != 0 ||
        cell->calc_error != LAB_CALC_NONE || cell->number < 84.249 || cell->number > 84.251 ||
        strcmp(lab_cell_display(cell), "84.25") != 0) {
        lab_sheet_destroy(sheet);
        return fail("formula arithmetic and display");
    }
    cell = lab_sheet_get(sheet, 0, 2);
    if (cell == NULL || cell->number < 126.374 || cell->number > 126.376) {
        lab_sheet_destroy(sheet);
        return fail("range SUM");
    }
    cell = lab_sheet_get(sheet, 0, 3);
    if (cell == NULL || cell->number < 63.187 || cell->number > 63.188) {
        lab_sheet_destroy(sheet);
        return fail("range AVERAGE");
    }
    cell = lab_sheet_get(sheet, 0, 4);
    if (cell == NULL || cell->number < 1774.514 || cell->number > 1774.517) {
        lab_sheet_destroy(sheet);
        return fail("POWER function");
    }
    cell = lab_sheet_get(sheet, 0, 5);
    if (cell == NULL || cell->number != 14.0) {
        lab_sheet_destroy(sheet);
        return fail("operator precedence");
    }
    cell = lab_sheet_get(sheet, 0, 6);
    if (cell == NULL || cell->number != -25.0) {
        lab_sheet_destroy(sheet);
        return fail("unary/power precedence");
    }
    cell = lab_sheet_get(sheet, 0, 7);
    if (cell == NULL || cell->calc_error != LAB_CALC_DIV_ZERO ||
        strcmp(lab_cell_display(cell), "#DIV/0!") != 0) {
        lab_sheet_destroy(sheet);
        return fail("division-by-zero error");
    }
    cell = lab_sheet_get(sheet, 0, 8);
    if (cell == NULL || cell->calc_error != LAB_CALC_NAME) {
        lab_sheet_destroy(sheet);
        return fail("unknown function error");
    }
    cell = lab_sheet_get(sheet, 0, 9);
    if (cell == NULL || cell->calc_error != LAB_CALC_VALUE) {
        lab_sheet_destroy(sheet);
        return fail("text arithmetic error");
    }
    cell = lab_sheet_get(sheet, 0, 10);
    if (cell == NULL || cell->calc_error != LAB_CALC_CYCLE ||
        lab_sheet_get(sheet, 0, 11)->calc_error != LAB_CALC_CYCLE) {
        lab_sheet_destroy(sheet);
        return fail("circular reference detection");
    }
    cell = lab_sheet_get(sheet, 3, 2);
    if (cell == NULL || cell->kind != LAB_CELL_TEXT || strcmp(cell->input, "Panthera") != 0) {
        lab_sheet_destroy(sheet);
        return fail("text cell classification");
    }

    for (i = 0; i < 1000; ++i) {
        sprintf(value, "V%u", i);
        if (lab_sheet_set_input(sheet, (LabU32)(i + 20), (LabU16)(i % 50), value) != LAB_MODEL_OK) {
            lab_sheet_destroy(sheet);
            return fail("sparse table growth");
        }
    }
    if (lab_sheet_count(sheet) != 1013) {
        lab_sheet_destroy(sheet);
        return fail("sparse cell count after growth");
    }
    cell = lab_sheet_get(sheet, 1019, 49);
    if (cell == NULL || strcmp(cell->input, "V999") != 0) {
        lab_sheet_destroy(sheet);
        return fail("sparse cell retrieval after rehash");
    }
    if (lab_sheet_set_input(sheet, 0, 0, "100") != LAB_MODEL_OK ||
        lab_sheet_recalculate(sheet) != LAB_MODEL_OK) {
        lab_sheet_destroy(sheet);
        return fail("formula cache invalidation after edit");
    }
    cell = lab_sheet_get(sheet, 0, 1);
    if (cell == NULL || cell->number != 200.0) {
        lab_sheet_destroy(sheet);
        return fail("dependent formula recalculation");
    }
    cell = lab_sheet_get(sheet, 0, 2);
    if (cell == NULL || cell->number != 300.0) {
        lab_sheet_destroy(sheet);
        return fail("dependent range recalculation");
    }
    if (lab_sheet_set_input(sheet, 0, 0, "") != LAB_MODEL_OK ||
        lab_sheet_get(sheet, 0, 0) != NULL || lab_sheet_count(sheet) != 1012) {
        lab_sheet_destroy(sheet);
        return fail("cell deletion");
    }
    if (lab_sheet_set_input(sheet, LAB_MAX_ROWS, 0, "invalid") != LAB_MODEL_OUT_OF_RANGE ||
        lab_sheet_set_input(sheet, 0, LAB_MAX_COLUMNS, "invalid") != LAB_MODEL_OUT_OF_RANGE) {
        lab_sheet_destroy(sheet);
        return fail("sheet boundary rejection");
    }
    lab_sheet_destroy(sheet);

    workbook = lab_workbook_create();
    if (workbook == NULL || lab_workbook_sheet_count(workbook) != 1 ||
        strcmp(lab_workbook_sheet_name(workbook, 0), "Sheet1") != 0) {
        lab_workbook_destroy(workbook);
        return fail("default workbook worksheet");
    }
    if (lab_workbook_add_sheet(workbook, "Summary", NULL) != LAB_MODEL_OK ||
        lab_workbook_add_sheet(workbook, "summary", NULL) != LAB_WORKBOOK_DUPLICATE_NAME ||
        lab_workbook_add_sheet(workbook, "Bad/Name", NULL) != LAB_WORKBOOK_BAD_NAME ||
        lab_workbook_sheet_count(workbook) != 2 ||
        lab_workbook_active_index(workbook) != 1) {
        lab_workbook_destroy(workbook);
        return fail("worksheet add, name validation and active selection");
    }
    data_sheet = lab_workbook_active_sheet(workbook);
    if (data_sheet == NULL || lab_sheet_set_input(data_sheet, 0, 0, "42") != LAB_MODEL_OK ||
        lab_workbook_set_active(workbook, 0) != LAB_MODEL_OK ||
        lab_workbook_active_sheet(workbook) != lab_workbook_sheet(workbook, 0) ||
        lab_sheet_get(lab_workbook_active_sheet(workbook), 0, 0) != NULL ||
        lab_workbook_rename_sheet(workbook, 1, "Data Table") != LAB_MODEL_OK ||
        strcmp(lab_workbook_sheet_name(workbook, 1), "Data Table") != 0 ||
        lab_workbook_remove_sheet(workbook, 1) != LAB_MODEL_OK ||
        lab_workbook_sheet_count(workbook) != 1 ||
        lab_workbook_remove_sheet(workbook, 0) != LAB_WORKBOOK_LAST_SHEET) {
        lab_workbook_destroy(workbook);
        return fail("worksheet isolation, rename, deletion and last-sheet guard");
    }
    lab_workbook_destroy(workbook);
    workbook = lab_workbook_create();
    if (workbook == NULL) return fail("multi-sheet workbook allocation");
    if (lab_sheet_set_text(lab_workbook_active_sheet(workbook), 0, 0, "FIRST") != LAB_MODEL_OK) {
        lab_workbook_destroy(workbook);
        return fail("first sheet test cell");
    }
    if (lab_workbook_add_sheet(workbook, "Data Table", NULL) != LAB_MODEL_OK) {
        lab_workbook_destroy(workbook);
        return fail("second worksheet add");
    }
    if (lab_sheet_set_text(lab_workbook_active_sheet(workbook), 0, 0, "SECOND") != LAB_MODEL_OK ||
        lab_workbook_set_active(workbook, 0) != LAB_MODEL_OK ||
        lab_workbook_set_sheet_selection(workbook, 0, 8, 7) != LAB_MODEL_OK ||
        lab_workbook_set_sheet_selection(workbook, 1, 4, 3) != LAB_MODEL_OK) {
        lab_workbook_destroy(workbook);
        return fail("second sheet data or active tab");
    }
    {
        int xlsx_result = lab_xlsx_save(workbook, "MULTI.XLSX");
        if (xlsx_result != LAB_XLSX_OK) {
            lab_workbook_destroy(workbook);
            return fail("multi-worksheet XLSX export");
        }
    }
    loaded = lab_workbook_create();
    if (loaded == NULL) {
        lab_workbook_destroy(workbook);
        return fail("multi-sheet import workbook allocation");
    }
    {
        int xlsx_result = lab_xlsx_load(&loaded, "MULTI.XLSX");
        if (xlsx_result != LAB_XLSX_OK) {
            lab_workbook_destroy(workbook);
            lab_workbook_destroy(loaded);
            return fail("multi-worksheet XLSX import");
        }
    }
    if (lab_workbook_sheet_count(loaded) != 2 || lab_workbook_active_index(loaded) != 0 ||
        strcmp(lab_workbook_sheet_name(loaded, 0), "Sheet1") != 0 ||
        strcmp(lab_workbook_sheet_name(loaded, 1), "Data Table") != 0 ||
        lab_sheet_get(lab_workbook_sheet(loaded, 0), 0, 0) == NULL ||
        strcmp(lab_sheet_get(lab_workbook_sheet(loaded, 0), 0, 0)->input, "FIRST") != 0 ||
        lab_sheet_get(lab_workbook_sheet(loaded, 1), 0, 0) == NULL ||
        strcmp(lab_sheet_get(lab_workbook_sheet(loaded, 1), 0, 0)->input, "SECOND") != 0) {
        lab_workbook_destroy(workbook);
        lab_workbook_destroy(loaded);
        return fail("multi-worksheet XLSX import, names, active tab and isolated cells");
    }
    {
        LabU32 selected_row;
        LabU16 selected_column;
        if (lab_workbook_get_sheet_selection(loaded, 0, &selected_row, &selected_column) != LAB_MODEL_OK ||
            selected_row != 8 || selected_column != 7 ||
            lab_workbook_get_sheet_selection(loaded, 1, &selected_row, &selected_column) != LAB_MODEL_OK ||
            selected_row != 4 || selected_column != 3) {
            lab_workbook_destroy(workbook);
            lab_workbook_destroy(loaded);
            return fail("multi-worksheet XLSX active-cell save and restore");
        }
    }
    if (lab_workbook_can_undo(loaded) || lab_workbook_can_redo(loaded)) {
        lab_workbook_destroy(workbook);
        lab_workbook_destroy(loaded);
        return fail("XLSX import is not recorded as a user edit");
    }
    {
        LabU32 history_sheet;
        LabU32 history_row;
        LabU16 history_column;
        LabU32 saved_row;
        LabU16 saved_column;
        unsigned step;
        unsigned undo_count = 0;

        if (lab_workbook_set_sheet_selection(workbook, 0, 8, 7) != LAB_MODEL_OK ||
            lab_workbook_set_sheet_selection(workbook, 1, 4, 3) != LAB_MODEL_OK ||
            lab_workbook_get_sheet_selection(workbook, 0, &saved_row, &saved_column) != LAB_MODEL_OK ||
            saved_row != 8 || saved_column != 7 ||
            lab_workbook_get_sheet_selection(workbook, 1, &saved_row, &saved_column) != LAB_MODEL_OK ||
            saved_row != 4 || saved_column != 3) {
            lab_workbook_destroy(workbook);
            lab_workbook_destroy(loaded);
            return fail("per-worksheet active cell storage");
        }
        if (lab_workbook_set_cell_input(workbook, 0, 1, 1, "old") != LAB_MODEL_OK ||
            lab_workbook_set_cell_input(workbook, 1, 2, 2, "new") != LAB_MODEL_OK ||
            !lab_workbook_can_undo(workbook) || lab_workbook_can_redo(workbook)) {
            lab_workbook_destroy(workbook);
            lab_workbook_destroy(loaded);
            return fail("workbook edit history recording");
        }
        if (lab_workbook_undo(workbook, &history_sheet, &history_row, &history_column) != LAB_MODEL_OK ||
            history_sheet != 1 || history_row != 2 || history_column != 2 ||
            lab_sheet_get(lab_workbook_sheet(workbook, 1), 2, 2) != NULL ||
            lab_workbook_undo(workbook, &history_sheet, &history_row, &history_column) != LAB_MODEL_OK ||
            history_sheet != 0 || history_row != 1 || history_column != 1 ||
            lab_sheet_get(lab_workbook_sheet(workbook, 0), 1, 1) != NULL ||
            !lab_workbook_can_redo(workbook)) {
            lab_workbook_destroy(workbook);
            lab_workbook_destroy(loaded);
            return fail("cross-worksheet undo and active selection restoration");
        }
        if (lab_workbook_redo(workbook, &history_sheet, &history_row, &history_column) != LAB_MODEL_OK ||
            history_sheet != 0 || lab_sheet_get(lab_workbook_sheet(workbook, 0), 1, 1) == NULL ||
            strcmp(lab_sheet_get(lab_workbook_sheet(workbook, 0), 1, 1)->input, "old") != 0 ||
            lab_workbook_undo(workbook, &history_sheet, &history_row, &history_column) != LAB_MODEL_OK ||
            lab_workbook_set_cell_input(workbook, 0, 1, 1, "branch") != LAB_MODEL_OK ||
            lab_workbook_can_redo(workbook)) {
            lab_workbook_destroy(workbook);
            lab_workbook_destroy(loaded);
            return fail("redo and redo-branch invalidation");
        }
        if (lab_workbook_set_cell_input(workbook, 0, 0, 0, "10") != LAB_MODEL_OK ||
            lab_sheet_set_input(lab_workbook_sheet(workbook, 0), 0, 1, "=A1*2") != LAB_MODEL_OK ||
            lab_sheet_recalculate(lab_workbook_sheet(workbook, 0)) != LAB_MODEL_OK ||
            lab_workbook_set_cell_input(workbook, 0, 0, 0, "20") != LAB_MODEL_OK ||
            lab_sheet_recalculate(lab_workbook_sheet(workbook, 0)) != LAB_MODEL_OK ||
            lab_sheet_get(lab_workbook_sheet(workbook, 0), 0, 1)->number != 40.0 ||
            lab_workbook_undo(workbook, &history_sheet, &history_row, &history_column) != LAB_MODEL_OK ||
            lab_sheet_recalculate(lab_workbook_sheet(workbook, history_sheet)) != LAB_MODEL_OK ||
            lab_sheet_get(lab_workbook_sheet(workbook, 0), 0, 1)->number != 20.0 ||
            lab_workbook_redo(workbook, &history_sheet, &history_row, &history_column) != LAB_MODEL_OK ||
            lab_sheet_recalculate(lab_workbook_sheet(workbook, history_sheet)) != LAB_MODEL_OK ||
            lab_sheet_get(lab_workbook_sheet(workbook, 0), 0, 1)->number != 40.0) {
            lab_workbook_destroy(workbook);
            lab_workbook_destroy(loaded);
            return fail("formula dependency recalculation after undo and redo");
        }
        if (lab_workbook_set_cell_input(workbook, 1, 0, 0, "") != LAB_MODEL_OK ||
            lab_sheet_get(lab_workbook_sheet(workbook, 1), 0, 0) != NULL ||
            lab_workbook_undo(workbook, &history_sheet, &history_row, &history_column) != LAB_MODEL_OK ||
            lab_sheet_get(lab_workbook_sheet(workbook, 1), 0, 0) == NULL ||
            strcmp(lab_sheet_get(lab_workbook_sheet(workbook, 1), 0, 0)->input, "SECOND") != 0) {
            lab_workbook_destroy(workbook);
            lab_workbook_destroy(loaded);
            return fail("undoing a cell clear restores its previous input");
        }
        for (step = 0; step < 70; ++step) {
            sprintf(value, "%u", step);
            if (lab_workbook_set_cell_input(workbook, 1, 10, 10, value) != LAB_MODEL_OK) {
                lab_workbook_destroy(workbook);
                lab_workbook_destroy(loaded);
                return fail("bounded history setup");
            }
        }
        while (lab_workbook_can_undo(workbook)) {
            if (lab_workbook_undo(workbook, &history_sheet, &history_row,
                                  &history_column) != LAB_MODEL_OK) {
                lab_workbook_destroy(workbook);
                lab_workbook_destroy(loaded);
                return fail("bounded history undo execution");
            }
            ++undo_count;
        }
        cell = lab_sheet_get(lab_workbook_sheet(workbook, 1), 10, 10);
        if (undo_count != 64 || cell == NULL || strcmp(cell->input, "5") != 0 ||
            lab_workbook_undo(workbook, &history_sheet, &history_row,
                              &history_column) != LAB_WORKBOOK_NOTHING_TO_UNDO) {
            lab_workbook_destroy(workbook);
            lab_workbook_destroy(loaded);
            return fail("history entry cap and oldest-record eviction");
        }
    }
    lab_workbook_destroy(workbook);
    lab_workbook_destroy(loaded);
    if (test_delimited()) return 1;
    puts("PASS: sparse formulas, workbook edit history, per-sheet selection, multi-worksheet XLSX, and CSV/TSV interchange.");
    return 0;
}
