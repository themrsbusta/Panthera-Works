#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lab_model.h"

#define LAB_FORMULA_MAX_DEPTH 64
#define LAB_FORMULA_MAX_ARGS 32

typedef struct {
    double number;
    LabU32 first_row;
    LabU32 last_row;
    LabU16 first_column;
    LabU16 last_column;
    LabU8 is_range;
    LabU8 is_text;
} FormulaValue;

typedef struct {
    LabSheet *sheet;
    const char *cursor;
    LabU8 error;
    unsigned depth;
} FormulaParser;

static void skip_spaces(FormulaParser *parser)
{
    while (*parser->cursor == ' ' || *parser->cursor == '\t' ||
           *parser->cursor == '\r' || *parser->cursor == '\n') ++parser->cursor;
}

static FormulaValue value_number(double number)
{
    FormulaValue value;
    memset(&value, 0, sizeof(value));
    value.number = number;
    return value;
}

static FormulaValue value_range(LabU32 row_a, LabU16 col_a,
                                LabU32 row_b, LabU16 col_b)
{
    FormulaValue value;
    memset(&value, 0, sizeof(value));
    value.is_range = 1;
    value.first_row = row_a < row_b ? row_a : row_b;
    value.last_row = row_a > row_b ? row_a : row_b;
    value.first_column = col_a < col_b ? col_a : col_b;
    value.last_column = col_a > col_b ? col_a : col_b;
    return value;
}

static int read_cell_reference(FormulaParser *parser, LabU32 *row, LabU16 *column)
{
    char reference[20];
    unsigned count = 0;
    unsigned letters = 0;
    const char *start = parser->cursor;
    const char *cursor = start;
    if (*cursor == '$') ++cursor;
    while ((cursor[letters] >= 'A' && cursor[letters] <= 'Z') ||
           (cursor[letters] >= 'a' && cursor[letters] <= 'z')) ++letters;
    if (letters == 0 || letters > 3) return 0;
    cursor += letters;
    if (*cursor == '$') ++cursor;
    if (*cursor < '0' || *cursor > '9') return 0;
    while (*cursor >= '0' && *cursor <= '9') ++cursor;
    count = (unsigned)(cursor - start);
    if (count >= sizeof(reference)) return 0;
    memcpy(reference, start, count);
    reference[count] = '\0';
    if (reference[0] == '$') memmove(reference, reference + 1, strlen(reference));
    {
        char *row_digits = reference;
        while ((*row_digits >= 'A' && *row_digits <= 'Z') ||
               (*row_digits >= 'a' && *row_digits <= 'z')) ++row_digits;
        if (*row_digits == '$') memmove(row_digits, row_digits + 1, strlen(row_digits));
    }
    if (!lab_cell_reference_parse(reference, row, column)) return 0;
    parser->cursor = cursor;
    return 1;
}

static LabU8 evaluate_cell(LabSheet *sheet, LabU32 row, LabU16 column,
                           unsigned depth, double *number);

static FormulaValue read_reference_value(FormulaParser *parser,
                                         LabU32 row, LabU16 column)
{
    FormulaValue value = value_number(0.0);
    const LabCell *cell = lab_sheet_get(parser->sheet, row, column);
    if (cell == NULL) return value;
    if (cell->kind == LAB_CELL_TEXT) {
        value.is_text = 1;
        return value;
    }
    if (cell->kind == LAB_CELL_NUMBER) {
        value.number = cell->number;
        return value;
    }
    if (cell->kind == LAB_CELL_FORMULA) {
        LabU8 error = evaluate_cell(parser->sheet, row, column,
                                    parser->depth + 1, &value.number);
        if (error != LAB_CALC_NONE) parser->error = error;
    }
    return value;
}

static int read_number(FormulaParser *parser, double *number)
{
    char token[64];
    unsigned count = 0;
    const char *cursor = parser->cursor;
    int digits = 0;
    while (*cursor >= '0' && *cursor <= '9') { ++cursor; ++digits; }
    if (*cursor == '.') {
        ++cursor;
        while (*cursor >= '0' && *cursor <= '9') { ++cursor; ++digits; }
    }
    if (digits == 0) return 0;
    if (*cursor == 'e' || *cursor == 'E') {
        ++cursor;
        if (*cursor == '+' || *cursor == '-') ++cursor;
        if (*cursor < '0' || *cursor > '9') return 0;
        while (*cursor >= '0' && *cursor <= '9') ++cursor;
    }
    count = (unsigned)(cursor - parser->cursor);
    if (count >= sizeof(token)) { parser->error = LAB_CALC_LIMIT; return 0; }
    memcpy(token, parser->cursor, count);
    token[count] = '\0';
    *number = atof(token);
    parser->cursor = cursor;
    return 1;
}

static int function_name_is(const char *name, unsigned bytes, const char *expected)
{
    unsigned i;
    if (strlen(expected) != bytes) return 0;
    for (i = 0; i < bytes; ++i) {
        char left = name[i];
        char right = expected[i];
        if (left >= 'a' && left <= 'z') left = (char)(left - 'a' + 'A');
        if (right >= 'a' && right <= 'z') right = (char)(right - 'a' + 'A');
        if (left != right) return 0;
    }
    return 1;
}

static int aggregate_range(FormulaParser *parser, FormulaValue range,
                           double *sum, double *minimum, double *maximum,
                           LabU32 *count)
{
    LabU32 ordinal;
    for (ordinal = 0; ordinal < lab_sheet_count(parser->sheet); ++ordinal) {
        const LabCell *cell = lab_sheet_at(parser->sheet, ordinal);
        double number;
        LabU8 error;
        if (cell == NULL || cell->row < range.first_row || cell->row > range.last_row ||
            cell->column < range.first_column || cell->column > range.last_column) continue;
        if (cell->kind == LAB_CELL_TEXT) continue;
        if (cell->kind == LAB_CELL_NUMBER) number = cell->number;
        else if (cell->kind == LAB_CELL_FORMULA) {
            error = evaluate_cell(parser->sheet, cell->row, cell->column,
                                  parser->depth + 1, &number);
            if (error != LAB_CALC_NONE) { parser->error = error; return 0; }
        } else continue;
        *sum += number;
        if (*count == 0 || number < *minimum) *minimum = number;
        if (*count == 0 || number > *maximum) *maximum = number;
        ++*count;
    }
    return 1;
}

static FormulaValue parse_expression(FormulaParser *parser);
static FormulaValue parse_unary(FormulaParser *parser);

static FormulaValue parse_function(FormulaParser *parser, const char *name, unsigned name_bytes)
{
    double arguments[LAB_FORMULA_MAX_ARGS];
    unsigned argument_count = 0;
    double sum = 0.0;
    double minimum = 0.0;
    double maximum = 0.0;
    LabU32 aggregate_count = 0;
    int is_sum = function_name_is(name, name_bytes, "SUM");
    int is_average = function_name_is(name, name_bytes, "AVERAGE");
    int is_min = function_name_is(name, name_bytes, "MIN");
    int is_max = function_name_is(name, name_bytes, "MAX");
    int is_count = function_name_is(name, name_bytes, "COUNT");
    int is_abs = function_name_is(name, name_bytes, "ABS");
    int is_power = function_name_is(name, name_bytes, "POWER");
    int is_round = function_name_is(name, name_bytes, "ROUND");
    FormulaValue result = value_number(0.0);
    if (!is_sum && !is_average && !is_min && !is_max && !is_count &&
        !is_abs && !is_power && !is_round) {
        parser->error = LAB_CALC_NAME;
        return result;
    }
    skip_spaces(parser);
    if (*parser->cursor != '(') { parser->error = LAB_CALC_SYNTAX; return result; }
    ++parser->cursor;
    skip_spaces(parser);
    if (*parser->cursor != ')') {
        for (;;) {
            FormulaValue argument = parse_expression(parser);
            if (parser->error != LAB_CALC_NONE) return result;
            if (argument.is_range) {
                if (!is_sum && !is_average && !is_min && !is_max && !is_count) {
                    parser->error = LAB_CALC_VALUE;
                    return result;
                }
                if (!aggregate_range(parser, argument, &sum, &minimum, &maximum,
                                     &aggregate_count)) return result;
            } else if (!argument.is_text) {
                if (argument_count >= LAB_FORMULA_MAX_ARGS) {
                    parser->error = LAB_CALC_LIMIT;
                    return result;
                }
                arguments[argument_count++] = argument.number;
                sum += argument.number;
                if (aggregate_count == 0 || argument.number < minimum) minimum = argument.number;
                if (aggregate_count == 0 || argument.number > maximum) maximum = argument.number;
                ++aggregate_count;
            } else if (is_abs || is_power || is_round) {
                parser->error = LAB_CALC_VALUE;
                return result;
            }
            skip_spaces(parser);
            if (*parser->cursor == ',') { ++parser->cursor; continue; }
            break;
        }
    }
    skip_spaces(parser);
    if (*parser->cursor != ')') { parser->error = LAB_CALC_SYNTAX; return result; }
    ++parser->cursor;

    if (is_sum) result.number = sum;
    else if (is_average) {
        if (aggregate_count == 0) parser->error = LAB_CALC_DIV_ZERO;
        else result.number = sum / (double)aggregate_count;
    } else if (is_min) result.number = aggregate_count ? minimum : 0.0;
    else if (is_max) result.number = aggregate_count ? maximum : 0.0;
    else if (is_count) result.number = (double)aggregate_count;
    else if (is_abs) {
        if (argument_count != 1) parser->error = LAB_CALC_VALUE;
        else result.number = arguments[0] < 0.0 ? -arguments[0] : arguments[0];
    } else if (is_power) {
        long exponent;
        double factor;
        double power_result = 1.0;
        int negative_exponent;
        if (argument_count != 2 || arguments[1] < -1000.0 || arguments[1] > 1000.0 ||
            (double)(long)arguments[1] != arguments[1]) parser->error = LAB_CALC_VALUE;
        else {
            exponent = (long)arguments[1];
            negative_exponent = exponent < 0;
            if (negative_exponent) exponent = -exponent;
            factor = arguments[0];
            while (exponent-- > 0) power_result *= factor;
            if (negative_exponent) {
                if (power_result == 0.0) parser->error = LAB_CALC_DIV_ZERO;
                else power_result = 1.0 / power_result;
            }
            result.number = power_result;
        }
    } else if (is_round) {
        int digits;
        double scale = 1.0;
        double scaled;
        long rounded;
        unsigned i;
        if (argument_count != 2 || arguments[1] < -15.0 || arguments[1] > 15.0 ||
            (double)(int)arguments[1] != arguments[1]) parser->error = LAB_CALC_VALUE;
        else {
            digits = (int)arguments[1];
            for (i = 0; i < (unsigned)(digits < 0 ? -digits : digits); ++i) scale *= 10.0;
            scaled = digits < 0 ? arguments[0] / scale : arguments[0] * scale;
            rounded = (long)(scaled < 0.0 ? scaled - 0.5 : scaled + 0.5);
            result.number = digits < 0 ? (double)rounded * scale : (double)rounded / scale;
        }
    }
    return result;
}

static FormulaValue parse_primary(FormulaParser *parser)
{
    FormulaValue value = value_number(0.0);
    LabU32 row_a;
    LabU32 row_b;
    LabU16 column_a;
    LabU16 column_b;
    const char *name_start;
    unsigned name_bytes;
    double number;
    skip_spaces(parser);
    if (*parser->cursor == '(') {
        ++parser->cursor;
        value = parse_expression(parser);
        skip_spaces(parser);
        if (*parser->cursor != ')') parser->error = LAB_CALC_SYNTAX;
        else ++parser->cursor;
        return value;
    }
    if ((*parser->cursor >= '0' && *parser->cursor <= '9') || *parser->cursor == '.') {
        if (!read_number(parser, &number)) {
            if (parser->error == LAB_CALC_NONE) parser->error = LAB_CALC_SYNTAX;
            return value;
        }
        return value_number(number);
    }
    if (*parser->cursor == '$' ||
        (*parser->cursor >= 'A' && *parser->cursor <= 'Z') ||
        (*parser->cursor >= 'a' && *parser->cursor <= 'z')) {
        const char *saved = parser->cursor;
        if (read_cell_reference(parser, &row_a, &column_a)) {
            skip_spaces(parser);
            if (*parser->cursor == ':') {
                ++parser->cursor;
                skip_spaces(parser);
                if (!read_cell_reference(parser, &row_b, &column_b)) {
                    parser->error = LAB_CALC_REF;
                    return value;
                }
                return value_range(row_a, column_a, row_b, column_b);
            }
            return read_reference_value(parser, row_a, column_a);
        }
        parser->cursor = saved;
        name_start = parser->cursor;
        if (!((*parser->cursor >= 'A' && *parser->cursor <= 'Z') ||
              (*parser->cursor >= 'a' && *parser->cursor <= 'z') || *parser->cursor == '_')) {
            parser->error = LAB_CALC_SYNTAX;
            return value;
        }
        while ((*parser->cursor >= 'A' && *parser->cursor <= 'Z') ||
               (*parser->cursor >= 'a' && *parser->cursor <= 'z') ||
               (*parser->cursor >= '0' && *parser->cursor <= '9') || *parser->cursor == '_')
            ++parser->cursor;
        name_bytes = (unsigned)(parser->cursor - name_start);
        return parse_function(parser, name_start, name_bytes);
    }
    parser->error = LAB_CALC_SYNTAX;
    return value;
}

static FormulaValue parse_power(FormulaParser *parser)
{
    FormulaValue left = parse_primary(parser);
    skip_spaces(parser);
    if (*parser->cursor == '^') {
        FormulaValue right;
        FormulaValue powered;
        long exponent;
        double base;
        double output = 1.0;
        int negative_exponent;
        ++parser->cursor;
        right = parse_unary(parser);
        if (parser->error != LAB_CALC_NONE) return left;
        if (left.is_range || left.is_text || right.is_range || right.is_text ||
            right.number < -1000.0 || right.number > 1000.0 ||
            (double)(long)right.number != right.number) {
            parser->error = LAB_CALC_VALUE;
            return left;
        }
        exponent = (long)right.number;
        negative_exponent = exponent < 0;
        if (negative_exponent) exponent = -exponent;
        base = left.number;
        while (exponent-- > 0) output *= base;
        if (negative_exponent) {
            if (output == 0.0) parser->error = LAB_CALC_DIV_ZERO;
            else output = 1.0 / output;
        }
        powered = value_number(output);
        return powered;
    }
    return left;
}

static FormulaValue parse_unary(FormulaParser *parser)
{
    FormulaValue value;
    skip_spaces(parser);
    if (*parser->cursor == '+') { ++parser->cursor; return parse_unary(parser); }
    if (*parser->cursor == '-') {
        ++parser->cursor;
        value = parse_unary(parser);
        if (!value.is_range && !value.is_text) value.number = -value.number;
        else parser->error = LAB_CALC_VALUE;
        return value;
    }
    return parse_power(parser);
}

static FormulaValue parse_term(FormulaParser *parser)
{
    FormulaValue left = parse_unary(parser);
    for (;;) {
        char operation;
        FormulaValue right;
        skip_spaces(parser);
        operation = *parser->cursor;
        if (operation != '*' && operation != '/') return left;
        ++parser->cursor;
        right = parse_unary(parser);
        if (parser->error != LAB_CALC_NONE) return left;
        if (left.is_range || left.is_text || right.is_range || right.is_text) {
            parser->error = LAB_CALC_VALUE;
            return left;
        }
        if (operation == '/' && right.number == 0.0) {
            parser->error = LAB_CALC_DIV_ZERO;
            return left;
        }
        left.number = operation == '*' ? left.number * right.number : left.number / right.number;
    }
}

static FormulaValue parse_expression(FormulaParser *parser)
{
    FormulaValue left = parse_term(parser);
    for (;;) {
        char operation;
        FormulaValue right;
        skip_spaces(parser);
        operation = *parser->cursor;
        if (operation != '+' && operation != '-') return left;
        ++parser->cursor;
        right = parse_term(parser);
        if (parser->error != LAB_CALC_NONE) return left;
        if (left.is_range || left.is_text || right.is_range || right.is_text) {
            parser->error = LAB_CALC_VALUE;
            return left;
        }
        left.number = operation == '+' ? left.number + right.number : left.number - right.number;
    }
}

static const char *error_text(LabU8 error)
{
    switch (error) {
        case LAB_CALC_VALUE: return "#VALUE!";
        case LAB_CALC_REF: return "#REF!";
        case LAB_CALC_DIV_ZERO: return "#DIV/0!";
        case LAB_CALC_CYCLE: return "#CYCLE!";
        case LAB_CALC_NAME: return "#NAME?";
        case LAB_CALC_LIMIT: return "#LIMIT!";
        default: return "#SYNTAX!";
    }
}

static int set_formula_result(LabCell *cell, double number, LabU8 error)
{
    char buffer[64];
    const char *text;
    size_t bytes;
    char *copy;
    if (error == LAB_CALC_NONE) {
        sprintf(buffer, "%.15g", number);
        text = buffer;
    } else text = error_text(error);
    bytes = strlen(text);
    copy = (char *)malloc(bytes + 1);
    if (copy == NULL) return 0;
    memcpy(copy, text, bytes + 1);
    if (cell->display != NULL) free(cell->display);
    cell->display = copy;
    cell->number = number;
    cell->calc_error = error;
    cell->calc_state = 2;
    return 1;
}

static LabU8 evaluate_cell(LabSheet *sheet, LabU32 row, LabU16 column,
                           unsigned depth, double *number)
{
    LabCell *cell = lab_sheet_get_mutable(sheet, row, column);
    FormulaParser parser;
    FormulaValue result;
    LabU8 error;
    if (cell == NULL) { *number = 0.0; return LAB_CALC_NONE; }
    if (cell->kind == LAB_CELL_NUMBER) { *number = cell->number; return LAB_CALC_NONE; }
    if (cell->kind == LAB_CELL_TEXT || cell->kind == LAB_CELL_EMPTY) {
        *number = 0.0;
        return LAB_CALC_NONE;
    }
    if (cell->kind != LAB_CELL_FORMULA) return LAB_CALC_VALUE;
    if (cell->calc_state == 1) return LAB_CALC_CYCLE;
    if (cell->calc_state == 2) {
        if (cell->calc_error == LAB_CALC_NONE) *number = cell->number;
        return cell->calc_error;
    }
    if (depth >= LAB_FORMULA_MAX_DEPTH) {
        if (!set_formula_result(cell, 0.0, LAB_CALC_LIMIT)) return LAB_CALC_LIMIT;
        return LAB_CALC_LIMIT;
    }
    cell->calc_state = 1;
    memset(&parser, 0, sizeof(parser));
    parser.sheet = sheet;
    parser.cursor = cell->input != NULL && cell->input[0] == '=' ? cell->input + 1 : "";
    parser.depth = depth;
    result = parse_expression(&parser);
    skip_spaces(&parser);
    if (parser.error == LAB_CALC_NONE && *parser.cursor != '\0') parser.error = LAB_CALC_SYNTAX;
    if (parser.error == LAB_CALC_NONE && (result.is_range || result.is_text)) parser.error = LAB_CALC_VALUE;
    error = parser.error;
    if (!set_formula_result(cell, result.number, error)) {
        cell->calc_state = 0;
        cell->calc_error = LAB_CALC_NONE;
        return LAB_CALC_LIMIT;
    }
    if (error == LAB_CALC_NONE) *number = result.number;
    return error;
}

int lab_sheet_recalculate(LabSheet *sheet)
{
    LabU32 ordinal;
    if (sheet == NULL) return LAB_MODEL_BAD_ARGUMENT;
    for (ordinal = 0; ordinal < lab_sheet_count(sheet); ++ordinal) {
        const LabCell *read_cell = lab_sheet_at(sheet, ordinal);
        double result;
        if (read_cell != NULL && read_cell->kind == LAB_CELL_FORMULA) {
            LabU32 row = read_cell->row;
            LabU16 column = read_cell->column;
            (void)evaluate_cell(sheet, row, column, 0, &result);
        }
    }
    return LAB_MODEL_OK;
}
