// SPDX-License-Identifier: BSD-3-Clause
/* Copyright 2021-2022, Intel Corporation */

/* Internal Header */

#ifndef LIBPMEMSTREAM_SPAN_H
#define LIBPMEMSTREAM_SPAN_H

#include "libpmemstream.h"

#include <stdint.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Span is a contiguous sequence of bytes, which are located on peristent storage.
 *
 * Span is always 8-bytes aligned. Its first 8 bytes hold information about size and type of a span. Type can be one
 * of the following:
 * - region
 * - entry
 * - empty space
 */
enum span_type {
	SPAN_EMPTY = 0b00ULL << 62,
	SPAN_REGION = 0b11ULL << 62,
	SPAN_ENTRY = 0b10ULL << 62,
	SPAN_UNKNOWN = 0b01ULL << 62
};

#define SPAN_TYPE_MASK (11ULL << 62)
#define SPAN_EXTRA_MASK (~SPAN_TYPE_MASK)

struct span_base {
    uint64_t size_and_type;
};

struct span_region {
    struct span_base span_base;
    uint64_t padding[7]; /* XXX: CACHELINE_SIZE - sizeof(span_base) */
    uint64_t data[];
};

struct span_entry {
    struct span_base span_base;
    uint64_t popcount;
    uint64_t data[];
};

struct span_empty {
    struct span_base span_base;
};

typedef uint64_t span_bytes;

struct span_base span_base_create(uint64_t size, enum span_type type);

/* Returns size of the span's data - excluding size of the span structure itself. */
size_t span_get_size(const span_base *span);

/* Returns value of span_get_size(span) + size of the span structure. */
size_t span_get_total_size(const span_base *span);

enum span_type span_get_type(const span_base *span);

#ifdef __cplusplus
} /* end extern "C" */
#endif
#endif /* LIBPMEMSTREAM_SPAN_H */
