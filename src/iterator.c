// SPDX-License-Identifier: BSD-3-Clause
/* Copyright 2021-2022, Intel Corporation */

#include "iterator.h"
#include "common/util.h"
#include "libpmemstream_internal.h"
#include "region.h"

#include <assert.h>
#include <stdbool.h>
#include <string.h>

int pmemstream_region_iterator_new(struct pmemstream_region_iterator **iterator, struct pmemstream *stream)
{
	struct pmemstream_region_iterator *iter = malloc(sizeof(*iter));
	if (!iter) {
		return -1;
	}

	*(struct pmemstream **)&iter->stream = stream;
	iter->region.offset = 0;

	*iterator = iter;

	return 0;
}

int pmemstream_region_iterator_next(struct pmemstream_region_iterator *it, struct pmemstream_region *region)
{
	while (it->region.offset < it->stream->usable_size) {
		struct span_base *span_base = span_offset_to_span_ptr(&it->stream->data, it->region.offset);

		if (span_get_type(span_base) == SPAN_REGION) {
			*region = it->region;
			it->region.offset += span_get_total_size(span_base);
			return 0;
		}

		/* if there are no more regions we should expect an empty span */
		assert(srt.type == span_get_type(span_base));
		it->region.offset += span_get_total_size(span_base);
	}

	return -1;
}

void pmemstream_region_iterator_delete(struct pmemstream_region_iterator **iterator)
{
	struct pmemstream_region_iterator *iter = *iterator;

	free(iter);
	*iterator = NULL;
}

int entry_iterator_initialize(struct pmemstream_entry_iterator *iterator, struct pmemstream *stream,
			      struct pmemstream_region region,
			      region_runtime_initialize_fn_type region_runtime_initialize_fn)
{
	struct span_region *span_region = (struct span_region*) span_offset_to_span_ptr(&it->stream->data, it->region.offset);
	assert(span_get_type(&span_region->base) == SPAN_REGION);

	struct pmemstream_region_runtime *region_rt;
	int ret = region_runtimes_map_get_or_create(stream->region_runtimes_map, region, &region_rt);
	if (ret) {
		return ret;
	}

	struct pmemstream_entry_iterator iter = {.stream = stream,
						 .offset = region.offset + offsetof(span_region, data),
						 .region = region,
						 .region_runtime = region_rt,
						 .region_runtime_initialize_fn = region_runtime_initialize_fn};
	memcpy(iterator, &iter, sizeof(struct pmemstream_entry_iterator));

	return 0;
}

int pmemstream_entry_iterator_new(struct pmemstream_entry_iterator **iterator, struct pmemstream *stream,
				  struct pmemstream_region region)
{
	struct pmemstream_entry_iterator *iter = malloc(sizeof(*iter));
	if (!iter) {
		return -1;
	}

	int ret = entry_iterator_initialize(iter, stream, region, &region_runtime_initialize_dirty_locked);
	if (ret) {
		goto err;
	}

	*iterator = iter;

	return 0;

err:
	free(iter);
	return ret;
}

static int validate_entry(const struct pmemstream *stream, struct pmemstream_entry entry)
{
	/* XXX: reading this span metadata is potentially dangerous. It might happen so that
	 * before calling this function region_runtime is in UNINITIALIZED state but some other thread
	 * changes it to CLEAR while span metadata is read. We might fix this using Optimistic Concurrency
	 * Control (using region_runtime state). */
	struct span_entry *span_entry = (struct span_entry*) span_offset_to_span_ptr(&it->stream->data, it->region.offset);
	const void *entry_data = span_entry->data;
	if (span_get_type(&span_entry->base) == SPAN_ENTRY && util_popcount_memory(entry_data, span_get_size(&span_entry->base)) == span_entry->popcount) {
		return 0;
	}
	return -1;
}

static bool pmemstream_entry_iterator_offset_is_inside_region(struct pmemstream_entry_iterator *iterator)
{
	struct span_base *span_region = span_offset_to_span_ptr(&it->stream->data, it->region.offset);
	uint64_t region_end_offset = iterator->region.offset + span_get_total_size(span_region);
	return iterator->offset >= iterator->region.offset && iterator->offset <= region_end_offset;
}

/* Precondition: region_runtime is initialized. */
static bool pmemstream_entry_iterator_offset_is_below_committed(struct pmemstream_entry_iterator *iterator)
{
	assert(region_runtime_get_state_acquire(iterator->region_runtime) != REGION_RUNTIME_STATE_UNINITIALIZED);

	/* Make sure that we didn't go beyond committed entries. */
	uint64_t committed_offset = region_runtime_get_committed_offset_acquire(iterator->region_runtime);
	if (iterator->offset >= committed_offset) {
		return false;
	}

#ifndef NDEBUG
	assert(pmemstream_entry_iterator_offset_is_inside_region(iterator));
	/* Region is already recovered, and we did not encounter end of the data yet.
	 * Span must be a valid entry. */
	struct pmemstream_entry entry = {.offset = iterator->offset};
	assert(validate_entry(iterator->stream, entry) == 0);
#endif

	return true;
}

static bool pmemstream_entry_iterator_offset_at_valid_entry(struct pmemstream_entry_iterator *iterator)
{
	assert(pmemstream_entry_iterator_offset_is_inside_region(iterator));

	struct span_base *span_region = span_offset_to_span_ptr(&it->stream->data, it->region.offset);
	uint64_t region_end_offset = iterator->region.offset + span_get_total_size(span_region);
	struct pmemstream_entry entry = {.offset = iterator->offset};

	return iterator->offset < region_end_offset && validate_entry(iterator->stream, entry) == 0;
}

static void pmemstream_entry_iterator_advance(struct pmemstream_entry_iterator *iterator)
{
	/* Verify that all metadata and data fits inside the region before and after iterator
	 * increment - those checks should not fail unless stream was corrupted. */
	assert(pmemstream_entry_iterator_offset_is_inside_region(iterator));

	struct span_base *span_entry = span_offset_to_span_ptr(&it->stream->data, it->region.offset);
	iterator->offset += span_get_total_size(span_entry);

	assert(pmemstream_entry_iterator_offset_is_inside_region(iterator));
}

static int pmemstream_entry_iterator_next_when_region_initialized(struct pmemstream_entry_iterator *iterator)
{
	if (pmemstream_entry_iterator_offset_is_below_committed(iterator)) {
		pmemstream_entry_iterator_advance(iterator);
		return 0;
	}

	return -1;
}

static int pmemstream_entry_iterator_next_when_region_not_initialized(struct pmemstream_entry_iterator *iterator)
{
	if (pmemstream_entry_iterator_offset_at_valid_entry(iterator)) {
		pmemstream_entry_iterator_advance(iterator);
		return 0;
	}

	/* Lazy (re-)initialization of region, when needed. */
	struct pmemstream_entry entry = {.offset = iterator->offset};
	iterator->region_runtime_initialize_fn(iterator->region_runtime, entry);
	return -1;
}

/* Advances entry iterator by one. Verifies entry integrity and initializes region runtime if end of data is found. */
int pmemstream_entry_iterator_next(struct pmemstream_entry_iterator *iterator, struct pmemstream_region *region,
				   struct pmemstream_entry *user_entry)
{
	// XXX: add test for NULL entry
	if (user_entry) {
		user_entry->offset = iterator->offset;
	}
	if (region) {
		*region = iterator->region;
	}

	if (region_runtime_get_state_acquire(iterator->region_runtime) != REGION_RUNTIME_STATE_UNINITIALIZED) {
		return pmemstream_entry_iterator_next_when_region_initialized(iterator);
	}

	return pmemstream_entry_iterator_next_when_region_not_initialized(iterator);
}

void pmemstream_entry_iterator_delete(struct pmemstream_entry_iterator **iterator)
{
	struct pmemstream_entry_iterator *iter = *iterator;

	free(iter);
	*iterator = NULL;
}
