#ifndef PANTHERA_LAB_INFLATE_H
#define PANTHERA_LAB_INFLATE_H

#include "lab_model.h"

/* Decode one raw RFC 1951 stream into a caller-sized output buffer. */
int lab_inflate_raw(const LabU8 *input, LabU32 input_bytes,
                    LabU8 *output, LabU32 output_bytes);

#endif
