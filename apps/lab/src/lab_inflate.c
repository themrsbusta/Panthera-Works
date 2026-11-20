/* Original bounded raw DEFLATE decoder for XLSX ZIP entries. */
#include <string.h>

#include "lab_inflate.h"

#define INFLATE_MAX_BITS 15
#define INFLATE_MAX_SYMBOLS 288

typedef struct {
    const LabU8 *input;
    LabU32 input_bytes;
    LabU32 bit_position;
    int failed;
} LabBitReader;

typedef struct {
    LabU16 counts[INFLATE_MAX_BITS + 1];
    LabU16 symbols[INFLATE_MAX_SYMBOLS];
    LabU16 symbol_count;
} LabHuffman;

static unsigned bits_read(LabBitReader *reader, unsigned count)
{
    unsigned value = 0;
    unsigned i;
    if (count > 16 || reader->failed) { reader->failed = 1; return 0; }
    for (i = 0; i < count; ++i) {
        LabU32 byte_index = reader->bit_position >> 3;
        unsigned bit_index = reader->bit_position & 7;
        if (byte_index >= reader->input_bytes) { reader->failed = 1; return 0; }
        value |= ((reader->input[byte_index] >> bit_index) & 1) << i;
        ++reader->bit_position;
    }
    return value;
}

static int huffman_build(LabHuffman *tree, const LabU8 *lengths,
                         unsigned symbols)
{
    LabU16 offsets[INFLATE_MAX_BITS + 1];
    unsigned i, length;
    int left = 1;
    memset(tree, 0, sizeof(*tree));
    if (symbols > INFLATE_MAX_SYMBOLS) return 0;
    for (i = 0; i < symbols; ++i) {
        if (lengths[i] > INFLATE_MAX_BITS) return 0;
        ++tree->counts[lengths[i]];
    }
    tree->counts[0] = 0;
    for (length = 1; length <= INFLATE_MAX_BITS; ++length) {
        left <<= 1;
        left -= tree->counts[length];
        if (left < 0) return 0;
    }
    offsets[1] = 0;
    for (length = 1; length < INFLATE_MAX_BITS; ++length)
        offsets[length + 1] = offsets[length] + tree->counts[length];
    for (i = 0; i < symbols; ++i)
        if (lengths[i] != 0) tree->symbols[offsets[lengths[i]]++] = (LabU16)i;
    tree->symbol_count = (LabU16)(symbols - tree->counts[0]);
    return tree->symbol_count != 0;
}

static int huffman_decode(LabBitReader *reader, const LabHuffman *tree)
{
    unsigned length;
    unsigned code = 0, first = 0, index = 0;
    for (length = 1; length <= INFLATE_MAX_BITS; ++length) {
        unsigned count;
        code |= bits_read(reader, 1);
        if (reader->failed) return -1;
        count = tree->counts[length];
        if (code >= first && code - first < count)
            return tree->symbols[index + code - first];
        index += count;
        first = (first + count) << 1;
        code <<= 1;
    }
    reader->failed = 1;
    return -1;
}

static int fixed_trees(LabHuffman *literal, LabHuffman *distance)
{
    LabU8 literal_lengths[288];
    LabU8 distance_lengths[32];
    unsigned i;
    for (i = 0; i <= 143; ++i) literal_lengths[i] = 8;
    for (; i <= 255; ++i) literal_lengths[i] = 9;
    for (; i <= 279; ++i) literal_lengths[i] = 7;
    for (; i <= 287; ++i) literal_lengths[i] = 8;
    for (i = 0; i < 32; ++i) distance_lengths[i] = 5;
    return huffman_build(literal, literal_lengths, 288) &&
           huffman_build(distance, distance_lengths, 32);
}

static int dynamic_trees(LabBitReader *reader, LabHuffman *literal,
                         LabHuffman *distance)
{
    static const LabU8 order[19] = {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
    LabU8 code_lengths[19];
    LabU8 lengths[318];
    LabHuffman code_tree;
    unsigned literal_count = bits_read(reader, 5) + 257;
    unsigned distance_count = bits_read(reader, 5) + 1;
    unsigned code_count = bits_read(reader, 4) + 4;
    unsigned i, total, used;
    memset(code_lengths, 0, sizeof(code_lengths));
    for (i = 0; i < code_count; ++i) code_lengths[order[i]] = (LabU8)bits_read(reader, 3);
    if (reader->failed || literal_count > 286 || distance_count > 32 ||
        !huffman_build(&code_tree, code_lengths, 19)) return 0;
    total = literal_count + distance_count;
    used = 0;
    while (used < total) {
        int symbol = huffman_decode(reader, &code_tree);
        unsigned repeat;
        if (symbol < 0) return 0;
        if (symbol <= 15) lengths[used++] = (LabU8)symbol;
        else if (symbol == 16) {
            LabU8 previous;
            if (used == 0) return 0;
            repeat = bits_read(reader, 2) + 3;
            previous = lengths[used - 1];
            if (repeat > total - used) return 0;
            while (repeat-- != 0) lengths[used++] = previous;
        } else if (symbol == 17) {
            repeat = bits_read(reader, 3) + 3;
            if (repeat > total - used) return 0;
            while (repeat-- != 0) lengths[used++] = 0;
        } else if (symbol == 18) {
            repeat = bits_read(reader, 7) + 11;
            if (repeat > total - used) return 0;
            while (repeat-- != 0) lengths[used++] = 0;
        } else return 0;
        if (reader->failed) return 0;
    }
    if (lengths[256] == 0) return 0;
    return huffman_build(literal, lengths, literal_count) &&
           huffman_build(distance, lengths + literal_count, distance_count);
}

static int inflate_codes(LabBitReader *reader, const LabHuffman *literal,
                         const LabHuffman *distance, LabU8 *output,
                         LabU32 output_bytes, LabU32 *used)
{
    static const LabU16 length_base[29] = {
        3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258
    };
    static const LabU8 length_extra[29] = {
        0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0
    };
    static const LabU16 distance_base[30] = {
        1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577
    };
    static const LabU8 distance_extra[30] = {
        0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13
    };
    for (;;) {
        int symbol = huffman_decode(reader, literal);
        if (symbol < 0) return 0;
        if (symbol < 256) {
            if (*used >= output_bytes) return 0;
            output[(*used)++] = (LabU8)symbol;
        } else if (symbol == 256) return 1;
        else {
            unsigned length, dist;
            int distance_symbol;
            if (symbol < 257 || symbol > 285) return 0;
            symbol -= 257;
            length = length_base[symbol] + bits_read(reader, length_extra[symbol]);
            distance_symbol = huffman_decode(reader, distance);
            if (reader->failed || distance_symbol < 0 || distance_symbol >= 30) return 0;
            dist = distance_base[distance_symbol] + bits_read(reader, distance_extra[distance_symbol]);
            if (reader->failed || dist == 0 || dist > *used || length > output_bytes - *used) return 0;
            while (length-- != 0) { output[*used] = output[*used - dist]; ++*used; }
        }
    }
}

int lab_inflate_raw(const LabU8 *input, LabU32 input_bytes,
                    LabU8 *output, LabU32 output_bytes)
{
    LabBitReader reader;
    LabU32 used = 0;
    unsigned final_block = 0;
    if (input == NULL || output == NULL || input_bytes == 0) return 0;
    reader.input = input; reader.input_bytes = input_bytes; reader.bit_position = 0; reader.failed = 0;
    while (!final_block) {
        unsigned block_type;
        final_block = bits_read(&reader, 1);
        block_type = bits_read(&reader, 2);
        if (reader.failed || block_type == 3) return 0;
        if (block_type == 0) {
            unsigned length, inverse;
            reader.bit_position = (reader.bit_position + 7) & ~7UL;
            length = bits_read(&reader, 16); inverse = bits_read(&reader, 16);
            if (reader.failed || (length ^ 0xffffU) != inverse || length > output_bytes - used) return 0;
            while (length-- != 0) output[used++] = (LabU8)bits_read(&reader, 8);
            if (reader.failed) return 0;
        } else {
            LabHuffman literal, distance;
            int ok = block_type == 1 ? fixed_trees(&literal, &distance) : dynamic_trees(&reader, &literal, &distance);
            if (!ok || !inflate_codes(&reader, &literal, &distance, output, output_bytes, &used)) return 0;
        }
    }
    return !reader.failed && used == output_bytes;
}
