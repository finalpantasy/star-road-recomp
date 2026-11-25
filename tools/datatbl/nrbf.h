/* Lossless, safe reader for BinaryFormatter NRBF records used by data tables. */
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NrbfDocument NrbfDocument;
typedef struct NrbfObject NrbfObject;

typedef struct NrbfInteger {
    int is_signed;
    unsigned bits;
    int64_t signed_value;
    uint64_t unsigned_value;
} NrbfInteger;

/* The parser never loads or executes serialized types. Unknown record payloads are preserved as bytes. */
int nrbf_decode(const uint8_t *data, size_t size, NrbfDocument **out);
int nrbf_encode(const NrbfDocument *doc, uint8_t **out, size_t *out_size);
void nrbf_document_free(NrbfDocument *doc);
size_t nrbf_document_object_count(const NrbfDocument *doc);
const NrbfObject *nrbf_document_object_at(const NrbfDocument *doc, size_t index);
NrbfObject *nrbf_document_object_at_mut(NrbfDocument *doc, size_t index);
int nrbf_object_get_integer(const NrbfDocument *doc, const NrbfObject *object, const char *member, NrbfInteger *out);
/* Writes an integer into the existing primitive field, clamped to that primitive's range. */
int nrbf_object_set_integer(NrbfDocument *doc, NrbfObject *object, const char *member, int64_t value);
int nrbf_object_set_integer_value(NrbfDocument *doc, NrbfObject *object, const char *member, const NrbfInteger *value);

#ifdef __cplusplus
}
#endif
