#ifndef PANTHERA_LAB_DELIMITED_H
#define PANTHERA_LAB_DELIMITED_H

#include "lab_model.h"

enum {
    LAB_DELIMITED_OK = 0,
    LAB_DELIMITED_BAD_ARGUMENT = 1,
    LAB_DELIMITED_NO_MEMORY = 2,
    LAB_DELIMITED_IO_ERROR = 3,
    LAB_DELIMITED_BAD_FORMAT = 4
};

int lab_delimited_load(LabWorkbook **workbook, const char *path,
                       unsigned char delimiter);
int lab_delimited_save(const LabSheet *sheet, const char *path,
                       unsigned char delimiter);

#endif
