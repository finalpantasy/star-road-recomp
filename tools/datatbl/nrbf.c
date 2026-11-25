/* Star Road Recomp lossless NRBF codec (task-25). Safe record parsing only; no BinaryFormatter runtime. */
#include "nrbf.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

enum {
    REC_HEADER = 0,
    REC_CLASS_WITH_ID = 1,
    REC_SYSTEM_CLASS_WITH_MEMBERS = 2,
    REC_CLASS_WITH_MEMBERS = 3,
    REC_SYSTEM_CLASS_WITH_MEMBERS_AND_TYPES = 4,
    REC_CLASS_WITH_MEMBERS_AND_TYPES = 5,
    REC_BINARY_OBJECT_STRING = 6,
    REC_BINARY_ARRAY = 7,
    REC_MEMBER_PRIMITIVE_TYPED = 8,
    REC_MEMBER_REFERENCE = 9,
    REC_OBJECT_NULL = 10,
    REC_MESSAGE_END = 11,
    REC_BINARY_LIBRARY = 12,
    REC_OBJECT_NULL_MULTIPLE_256 = 13,
    REC_OBJECT_NULL_MULTIPLE = 14,
    REC_ARRAY_SINGLE_PRIMITIVE = 15,
    REC_ARRAY_SINGLE_OBJECT = 16,
    REC_ARRAY_SINGLE_STRING = 17
};

enum { BT_PRIMITIVE = 0, BT_STRING = 1, BT_OBJECT = 2, BT_SYSTEM_CLASS = 3, BT_CLASS = 4,
       BT_OBJECT_ARRAY = 5, BT_STRING_ARRAY = 6, BT_PRIMITIVE_ARRAY = 7 };

typedef struct NrbfMember {
    const char *name;
    uint8_t binary_type;
    uint8_t primitive_type;
    int has_primitive;
    size_t value_offset;
    size_t value_size;
} NrbfMember;

struct NrbfObject {
    uint32_t id;
    const char *class_name;
    NrbfMember *members;
    size_t member_count;
    int is_array;
};

typedef struct NrbfSchema {
    uint32_t id;
    char *class_name;
    char **member_names;
    uint8_t *binary_types;
    uint8_t *primitive_types;
    size_t member_count;
} NrbfSchema;

struct NrbfDocument {
    uint8_t *data;
    size_t size;
    size_t cursor;
    NrbfSchema *schemas;
    size_t schema_count, schema_capacity;
    NrbfObject *objects;
    size_t object_count, object_capacity;
};

static int reserve(void **p, size_t *capacity, size_t need, size_t item_size) {
    size_t cap = *capacity ? *capacity : 16;
    if (need <= *capacity) return 1;
    while (cap < need) {
        if (cap > SIZE_MAX / 2) return 0;
        cap *= 2;
    }
    if (item_size && cap > SIZE_MAX / item_size) return 0;
    void *q = realloc(*p, cap * item_size);
    if (!q) return 0;
    *p = q;
    *capacity = cap;
    return 1;
}

static int take(NrbfDocument *d, size_t n, size_t *at) {
    if (n > d->size - d->cursor) return 0;
    if (at) *at = d->cursor;
    d->cursor += n;
    return 1;
}

static int read_u8(NrbfDocument *d, uint8_t *v) {
    size_t at;
    if (!take(d, 1, &at)) return 0;
    *v = d->data[at];
    return 1;
}

static int read_u32(NrbfDocument *d, uint32_t *v) {
    size_t at;
    if (!take(d, 4, &at)) return 0;
    *v = (uint32_t)d->data[at] | ((uint32_t)d->data[at + 1] << 8) |
         ((uint32_t)d->data[at + 2] << 16) | ((uint32_t)d->data[at + 3] << 24);
    return 1;
}

static int read_i32(NrbfDocument *d, int32_t *v) {
    uint32_t u;
    if (!read_u32(d, &u)) return 0;
    memcpy(v, &u, sizeof *v);
    return 1;
}

static int read_string(NrbfDocument *d, char **out) {
    uint32_t n = 0;
    unsigned shift = 0;
    for (;;) {
        uint8_t b;
        if (!read_u8(d, &b) || shift >= 35) return 0;
        n |= (uint32_t)(b & 0x7f) << shift;
        if (!(b & 0x80)) break;
        shift += 7;
    }
    if (n > d->size - d->cursor || (size_t)n == SIZE_MAX) return 0;
    char *s = (char *)malloc((size_t)n + 1);
    if (!s) return 0;
    if (!take(d, n, NULL)) { free(s); return 0; }
    memcpy(s, d->data + d->cursor - n, n);
    s[n] = 0;
    *out = s;
    return 1;
}

static int skip_string(NrbfDocument *d) {
    uint32_t n = 0;
    unsigned shift = 0;
    for (;;) {
        uint8_t b;
        if (!read_u8(d, &b) || shift >= 35) return 0;
        n |= (uint32_t)(b & 0x7f) << shift;
        if (!(b & 0x80)) break;
        shift += 7;
    }
    return take(d, n, NULL);
}

static size_t primitive_size(uint8_t type) {
    switch (type) {
    case 1: case 2: case 10: return 1;                  /* Boolean, Byte, SByte */
    case 3: case 7: case 14: return 2;                 /* Char, Int16, UInt16 */
    case 8: case 11: case 15: return 4;                 /* Int32, Single, UInt32 */
    case 4: case 6: case 9: case 12: case 13: case 16: return 8; /* Currency, Double, Int64, TimeSpan, DateTime, UInt64 */
    case 5: return 16;                                  /* Decimal */
    default: return 0;
    }
}

static int skip_primitive(NrbfDocument *d, uint8_t type, size_t *at, size_t *width) {
    if (type == 17) {                                    /* Null has no payload */
        if (at) *at = d->cursor;
        if (width) *width = 0;
        return 1;
    }
    if (type == 18) {                                    /* String primitive value */
        size_t start = d->cursor;
        if (at) *at = start;
        if (!skip_string(d)) return 0;
        if (width) *width = d->cursor - start;
        return 1;
    }
    size_t n = primitive_size(type);
    if (!n || !take(d, n, at)) return 0;
    if (width) *width = n;
    return 1;
}

static NrbfSchema *find_schema(NrbfDocument *d, uint32_t id) {
    for (size_t i = 0; i < d->schema_count; i++) if (d->schemas[i].id == id) return &d->schemas[i];
    return NULL;
}

static int append_schema(NrbfDocument *d, NrbfSchema *schema) {
    if (!reserve((void **)&d->schemas, &d->schema_capacity, d->schema_count + 1, sizeof *d->schemas)) return 0;
    d->schemas[d->schema_count++] = *schema;
    memset(schema, 0, sizeof *schema);
    return 1;
}

static int append_object(NrbfDocument *d, uint32_t id, const char *class_name, size_t member_count, int is_array, size_t *index) {
    if (!reserve((void **)&d->objects, &d->object_capacity, d->object_count + 1, sizeof *d->objects)) return 0;
    NrbfObject *o = &d->objects[d->object_count];
    memset(o, 0, sizeof *o);
    o->id = id;
    o->class_name = class_name;
    o->member_count = member_count;
    o->is_array = is_array;
    if (member_count) {
        o->members = (NrbfMember *)calloc(member_count, sizeof *o->members);
        if (!o->members) return 0;
    }
    if (index) *index = d->object_count;
    d->object_count++;
    return 1;
}

static void free_schema(NrbfSchema *s) {
    if (!s) return;
    free(s->class_name);
    for (size_t i = 0; i < s->member_count; i++) free(s->member_names ? s->member_names[i] : NULL);
    free(s->member_names);
    free(s->binary_types);
    free(s->primitive_types);
    memset(s, 0, sizeof *s);
}

static int read_type_info(NrbfDocument *d, uint8_t binary_type, uint8_t *primitive_type) {
    uint32_t lib_id;
    char *unused = NULL;
    switch (binary_type) {
    case BT_PRIMITIVE:
    case BT_PRIMITIVE_ARRAY:
        return read_u8(d, primitive_type) && (primitive_size(*primitive_type) != 0 || *primitive_type == 17 || *primitive_type == 18);
    case BT_SYSTEM_CLASS:
        if (!read_string(d, &unused)) return 0;
        free(unused);
        return 1;
    case BT_CLASS:
        if (!read_string(d, &unused)) return 0;
        free(unused);
        return read_u32(d, &lib_id);
    case BT_STRING:
    case BT_OBJECT:
    case BT_OBJECT_ARRAY:
    case BT_STRING_ARRAY:
        return 1;
    default:
        return 0;
    }
}

static int read_class_schema(NrbfDocument *d, uint8_t record, NrbfSchema *s) {
    int32_t count;
    uint32_t lib_id;
    if (!read_u32(d, &s->id) || !read_string(d, &s->class_name) || !read_i32(d, &count) || count < 0 || count > 65536) return 0;
    s->member_count = (size_t)count;
    if (s->member_count) {
        s->member_names = (char **)calloc(s->member_count, sizeof *s->member_names);
        s->binary_types = (uint8_t *)calloc(s->member_count, sizeof *s->binary_types);
        s->primitive_types = (uint8_t *)calloc(s->member_count, sizeof *s->primitive_types);
        if (!s->member_names || !s->binary_types || !s->primitive_types) return 0;
    }
    for (size_t i = 0; i < s->member_count; i++) if (!read_string(d, &s->member_names[i])) return 0;
    if (record == REC_SYSTEM_CLASS_WITH_MEMBERS_AND_TYPES || record == REC_CLASS_WITH_MEMBERS_AND_TYPES) {
        for (size_t i = 0; i < s->member_count; i++) if (!read_u8(d, &s->binary_types[i]) || s->binary_types[i] > BT_PRIMITIVE_ARRAY) return 0;
        for (size_t i = 0; i < s->member_count; i++) if (!read_type_info(d, s->binary_types[i], &s->primitive_types[i])) return 0;
    } else {
        for (size_t i = 0; i < s->member_count; i++) s->binary_types[i] = BT_OBJECT;
    }
    if (record == REC_CLASS_WITH_MEMBERS || record == REC_CLASS_WITH_MEMBERS_AND_TYPES) {
        if (!read_u32(d, &lib_id)) return 0;
    }
    return 1;
}

static int skip_library(NrbfDocument *d) {
    uint32_t id;
    char *name = NULL;
    if (!read_u32(d, &id) || !read_string(d, &name)) return 0;
    free(name);
    return 1;
}

static int read_value(NrbfDocument *d, unsigned depth, int allow_null_run, size_t *run);

static int read_array_values(NrbfDocument *d, unsigned depth, uint64_t count, int primitive, uint8_t primitive_type) {
    if (count > d->size) return 0;
    if (primitive) {
        size_t width = primitive_size(primitive_type);
        if (!width || count > (d->size - d->cursor) / width) return 0;
        return take(d, (size_t)count * width, NULL);
    }
    uint64_t done = 0;
    while (done < count) {
        size_t run = 1;
        if (!read_value(d, depth + 1, 1, &run) || !run || run > count - done) return 0;
        done += run;
    }
    return 1;
}

static int parse_record(NrbfDocument *d, uint8_t record, unsigned depth) {
    if (depth > 128) return 0;
    uint32_t id, meta_id;
    int32_t count;
    size_t obj_index;
    switch (record) {
    case REC_CLASS_WITH_ID: {
        if (!read_u32(d, &id) || !read_u32(d, &meta_id)) return 0;
        NrbfSchema *found = find_schema(d, meta_id);
        if (!found) return 0;
        NrbfSchema schema = *found; /* recursive records can grow the schema vector */
        if (!append_object(d, id, schema.class_name, schema.member_count, 0, &obj_index)) return 0;
        NrbfMember *members = d->objects[obj_index].members; /* separately allocated and stable as objects grow */
        for (size_t i = 0; i < schema.member_count; i++) {
            NrbfMember *m = &members[i];
            m->name = schema.member_names[i];
            m->binary_type = schema.binary_types[i];
            m->primitive_type = schema.primitive_types[i];
            if (m->binary_type == BT_PRIMITIVE) {
                m->has_primitive = 1;
                if (!skip_primitive(d, m->primitive_type, &m->value_offset, &m->value_size)) return 0;
            } else if (!read_value(d, depth + 1, 0, NULL)) return 0;
        }
        return 1;
    }
    case REC_SYSTEM_CLASS_WITH_MEMBERS:
    case REC_CLASS_WITH_MEMBERS:
    case REC_SYSTEM_CLASS_WITH_MEMBERS_AND_TYPES:
    case REC_CLASS_WITH_MEMBERS_AND_TYPES: {
        NrbfSchema parsed;
        memset(&parsed, 0, sizeof parsed);
        if (!read_class_schema(d, record, &parsed)) { free_schema(&parsed); return 0; }
        uint32_t object_id = parsed.id;
        if (!append_schema(d, &parsed)) { free_schema(&parsed); return 0; }
        NrbfSchema schema = d->schemas[d->schema_count - 1]; /* recursive records can grow the schema vector */
        if (!append_object(d, object_id, schema.class_name, schema.member_count, 0, &obj_index)) return 0;
        NrbfMember *members = d->objects[obj_index].members;
        for (size_t i = 0; i < schema.member_count; i++) {
            NrbfMember *m = &members[i];
            m->name = schema.member_names[i];
            m->binary_type = schema.binary_types[i];
            m->primitive_type = schema.primitive_types[i];
            if (m->binary_type == BT_PRIMITIVE) {
                m->has_primitive = 1;
                if (!skip_primitive(d, m->primitive_type, &m->value_offset, &m->value_size)) return 0;
            } else if (!read_value(d, depth + 1, 0, NULL)) return 0;
        }
        return 1;
    }
    case REC_BINARY_OBJECT_STRING: {
        char *s = NULL;
        if (!read_u32(d, &id) || !read_string(d, &s)) return 0;
        free(s);
        return 1;
    }
    case REC_BINARY_ARRAY: {
        uint8_t array_type, bt, primitive = 0;
        int32_t rank;
        uint64_t elements = 1;
        if (!read_u32(d, &id) || !read_u8(d, &array_type) || array_type > 5 || !read_i32(d, &rank) || rank <= 0 || rank > 32) return 0;
        for (int32_t i = 0; i < rank; i++) {
            int32_t length;
            if (!read_i32(d, &length) || length < 0 || (uint64_t)length > d->size || (length && elements > UINT64_MAX / (uint32_t)length)) return 0;
            elements *= (uint32_t)length;
        }
        if (array_type >= 3) for (int32_t i = 0; i < rank; i++) { int32_t lower; if (!read_i32(d, &lower)) return 0; }
        if (!read_u8(d, &bt) || bt > BT_PRIMITIVE_ARRAY || !read_type_info(d, bt, &primitive)) return 0;
        if (!append_object(d, id, NULL, 0, 1, NULL)) return 0;
        return read_array_values(d, depth, elements, bt == BT_PRIMITIVE, primitive);
    }
    case REC_MEMBER_PRIMITIVE_TYPED: {
        uint8_t type;
        return read_u8(d, &type) && skip_primitive(d, type, NULL, NULL);
    }
    case REC_MEMBER_REFERENCE:
        return read_u32(d, &id);
    case REC_OBJECT_NULL:
        return 1;
    case REC_ARRAY_SINGLE_PRIMITIVE: {
        uint8_t type;
        if (!read_u32(d, &id) || !read_i32(d, &count) || count < 0 || !read_u8(d, &type)) return 0;
        return read_array_values(d, depth, (uint32_t)count, 1, type);
    }
    case REC_ARRAY_SINGLE_OBJECT:
    case REC_ARRAY_SINGLE_STRING:
        if (!read_u32(d, &id) || !read_i32(d, &count) || count < 0) return 0;
        if (!append_object(d, id, NULL, 0, 1, NULL)) return 0;
        return read_array_values(d, depth, (uint32_t)count, 0, 0);
    case REC_BINARY_LIBRARY:
        return skip_library(d);
    default:
        return 0;
    }
}

static int read_value(NrbfDocument *d, unsigned depth, int allow_null_run, size_t *run) {
    for (;;) {
        uint8_t record;
        if (!read_u8(d, &record)) return 0;
        if (record == REC_BINARY_LIBRARY) { if (!skip_library(d)) return 0; continue; }
        if (record == REC_OBJECT_NULL_MULTIPLE_256 || record == REC_OBJECT_NULL_MULTIPLE) {
            uint32_t n;
            uint8_t n8;
            if (!allow_null_run) return 0;
            if (record == REC_OBJECT_NULL_MULTIPLE_256) { if (!read_u8(d, &n8)) return 0; n = n8; }
            else if (!read_u32(d, &n)) return 0;
            if (!n) return 0;
            if (run) *run = n;
            return 1;
        }
        if (record == REC_MESSAGE_END || record == REC_HEADER) return 0;
        if (!parse_record(d, record, depth)) return 0;
        if (run) *run = 1;
        return 1;
    }
}

int nrbf_decode(const uint8_t *data, size_t size, NrbfDocument **out) {
    if (out) *out = NULL;
    if (!data || !out || size < 17) return 0;
    NrbfDocument *d = (NrbfDocument *)calloc(1, sizeof *d);
    if (!d) return 0;
    d->data = (uint8_t *)malloc(size);
    if (!d->data) { free(d); return 0; }
    memcpy(d->data, data, size);
    d->size = size;
    uint8_t record;
    uint32_t root, header;
    int32_t major, minor;
    if (!read_u8(d, &record) || record != REC_HEADER || !read_u32(d, &root) || !read_u32(d, &header) ||
        !read_i32(d, &major) || !read_i32(d, &minor) || major != 1 || minor != 0) goto fail;
    int ended = 0;
    while (d->cursor < d->size) {
        if (!read_u8(d, &record)) goto fail;
        if (record == REC_MESSAGE_END) { ended = 1; break; }
        if (record == REC_BINARY_LIBRARY) { if (!skip_library(d)) goto fail; continue; }
        if (record == REC_HEADER || !parse_record(d, record, 0)) goto fail;
    }
    if (!ended || d->cursor != d->size) goto fail;
    *out = d;
    return 1;
fail:
    nrbf_document_free(d);
    return 0;
}

int nrbf_encode(const NrbfDocument *doc, uint8_t **out, size_t *out_size) {
    if (out) *out = NULL;
    if (out_size) *out_size = 0;
    if (!doc || !out || !out_size || !doc->data || !doc->size) return 0;
    uint8_t *copy = (uint8_t *)malloc(doc->size);
    if (!copy) return 0;
    memcpy(copy, doc->data, doc->size);
    *out = copy;
    *out_size = doc->size;
    return 1;
}

void nrbf_document_free(NrbfDocument *doc) {
    if (!doc) return;
    for (size_t i = 0; i < doc->object_count; i++) free(doc->objects[i].members);
    free(doc->objects);
    for (size_t i = 0; i < doc->schema_count; i++) free_schema(&doc->schemas[i]);
    free(doc->schemas);
    free(doc->data);
    free(doc);
}

size_t nrbf_document_object_count(const NrbfDocument *doc) { return doc ? doc->object_count : 0; }
const NrbfObject *nrbf_document_object_at(const NrbfDocument *doc, size_t index) { return doc && index < doc->object_count ? &doc->objects[index] : NULL; }
NrbfObject *nrbf_document_object_at_mut(NrbfDocument *doc, size_t index) { return doc && index < doc->object_count ? &doc->objects[index] : NULL; }

static const NrbfMember *find_member(const NrbfObject *object, const char *name) {
    if (!object || !name || object->is_array) return NULL;
    for (size_t i = 0; i < object->member_count; i++)
        if (object->members[i].name && strcmp(object->members[i].name, name) == 0) return &object->members[i];
    return NULL;
}

static NrbfMember *find_member_mut(NrbfObject *object, const char *name) {
    if (!object || !name || object->is_array) return NULL;
    for (size_t i = 0; i < object->member_count; i++)
        if (object->members[i].name && strcmp(object->members[i].name, name) == 0) return &object->members[i];
    return NULL;
}

int nrbf_object_get_integer(const NrbfDocument *doc, const NrbfObject *object, const char *member, NrbfInteger *out) {
    if (!doc || !out) return 0;
    const NrbfMember *m = find_member(object, member);
    if (!m || !m->has_primitive || m->value_offset > doc->size || m->value_size > doc->size - m->value_offset) return 0;
    unsigned bits;
    int is_signed;
    switch (m->primitive_type) {
    case 2: bits = 8; is_signed = 0; break; /* Byte */
    case 10: bits = 8; is_signed = 1; break;/* SByte */
    case 7: bits = 16; is_signed = 1; break;
    case 14: bits = 16; is_signed = 0; break;
    case 8: bits = 32; is_signed = 1; break;
    case 15: bits = 32; is_signed = 0; break;
    case 9: bits = 64; is_signed = 1; break;
    case 16: bits = 64; is_signed = 0; break;
    default: return 0;
    }
    uint64_t value = 0;
    for (size_t i = 0; i < m->value_size; i++) value |= (uint64_t)doc->data[m->value_offset + i] << (8 * i);
    out->is_signed = is_signed;
    out->bits = bits;
    out->unsigned_value = value;
    if (is_signed) {
        if (bits == 64) memcpy(&out->signed_value, &value, sizeof value);
        else if (value & ((uint64_t)1 << (bits - 1))) out->signed_value = (int64_t)(value - ((uint64_t)1 << bits));
        else out->signed_value = (int64_t)value;
    } else out->signed_value = value > INT64_MAX ? INT64_MAX : (int64_t)value;
    return 1;
}

int nrbf_object_set_integer(NrbfDocument *doc, NrbfObject *object, const char *member, int64_t value) {
    NrbfInteger input;
    memset(&input, 0, sizeof input);
    input.is_signed = 1;
    input.signed_value = value;
    return nrbf_object_set_integer_value(doc, object, member, &input);
}

int nrbf_object_set_integer_value(NrbfDocument *doc, NrbfObject *object, const char *member, const NrbfInteger *input) {
    if (!doc || !doc->data || !input) return 0;
    NrbfMember *m = find_member_mut(object, member);
    if (!m || !m->has_primitive || m->value_offset > doc->size || m->value_size > doc->size - m->value_offset) return 0;
    unsigned bits;
    int is_signed;
    switch (m->primitive_type) {
    case 2: bits = 8; is_signed = 0; break;
    case 10: bits = 8; is_signed = 1; break;
    case 7: bits = 16; is_signed = 1; break;
    case 14: bits = 16; is_signed = 0; break;
    case 8: bits = 32; is_signed = 1; break;
    case 15: bits = 32; is_signed = 0; break;
    case 9: bits = 64; is_signed = 1; break;
    case 16: bits = 64; is_signed = 0; break;
    default: return 0;
    }
    uint64_t raw;
    if (is_signed) {
        int64_t min = bits == 64 ? INT64_MIN : -((int64_t)1 << (bits - 1));
        int64_t max = bits == 64 ? INT64_MAX : (((int64_t)1 << (bits - 1)) - 1);
        int64_t value = input->is_signed ? input->signed_value : (input->unsigned_value > INT64_MAX ? INT64_MAX : (int64_t)input->unsigned_value);
        if (value < min) value = min;
        if (value > max) value = max;
        raw = (uint64_t)value;
    } else {
        uint64_t value = input->is_signed ? (input->signed_value < 0 ? 0 : (uint64_t)input->signed_value) : input->unsigned_value;
        uint64_t max = bits == 64 ? UINT64_MAX : (((uint64_t)1 << bits) - 1);
        if (value > max) value = max;
        raw = value;
    }
    for (size_t i = 0; i < m->value_size; i++) doc->data[m->value_offset + i] = (uint8_t)(raw >> (8 * i));
    return 1;
}
