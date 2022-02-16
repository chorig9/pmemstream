// SPDX-License-Identifier: BSD-3-Clause
/* Copyright 2021-2022, Intel Corporation */

#include "span.h"

#include <assert.h>

struct span_base span_base_create(uint64_t size, enum span_type type)
{
	assert((size & SPAN_TYPE_MASK) == 0);

	struct span_base span {
		.size_and_type = size | type;
	};

	return span;
}

uint64_t span_get_size(const span_base *span)
{
	return span->size_and_type & SPAN_EXTRA_MASK;
}

size_t span_get_total_size(const span_base *span)
{
	switch (span_get_type(span)) {
		SPAN_EMPTY:
		return span_get_size(span) + sizeof(span_empty);
		SPAN_ENTRY:
		return span_get_size(span) + sizeof(span_entry);
		SPAN_REGION:
		return span_get_size(span) + sizeof(span_region);
		default:
		return span_get_size(span);
	}
}

enum span_type span_get_type(const span_base *span)
{
	return span->size_and_type & SPAN_TYPE_MASK;
}
