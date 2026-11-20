#ifndef PANTHERA_LAB_XLSX_H
#define PANTHERA_LAB_XLSX_H

#include "lab_model.h"

enum {
    LAB_XLSX_OK = 0,
    LAB_XLSX_BAD_ARGUMENT = 1,
    LAB_XLSX_NO_MEMORY = 2,
    LAB_XLSX_IO_ERROR = 3,
    LAB_XLSX_PACKAGE_TOO_LARGE = 4,
    LAB_XLSX_UNSUPPORTED = 5,
    LAB_XLSX_BAD_PACKAGE = 6,
    LAB_XLSX_BAD_XML = 7,
    LAB_XLSX_BAD_EOCD = 8,
    LAB_XLSX_BAD_DIRECTORY = 9,
    LAB_XLSX_BAD_LOCAL_HEADER = 10,
    LAB_XLSX_BAD_CRC = 11,
    LAB_XLSX_BAD_LOCAL_SIGNATURE = 12,
    LAB_XLSX_BAD_LOCAL_FIELDS = 13,
    LAB_XLSX_LOCAL_SEEK_FAILED = 14,
    LAB_XLSX_LOCAL_READ_FAILED = 15,
    LAB_XLSX_LOCAL_SIGNATURE_MISMATCH = 16,
    LAB_XLSX_BAD_DEFLATE = 17,
    LAB_XLSX_ENTRY_NOT_FOUND = 18
};

int lab_xlsx_save(LabWorkbook *workbook, const char *path);
int lab_xlsx_load(LabWorkbook **workbook, const char *path);

#endif
