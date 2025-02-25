// SPDX-License-Identifier: BSD-3-Clause
/* Copyright 2021-2022, Intel Corporation */

/* Implementation of public C API */

#include "common/util.h"
#include "libpmemstream_internal.h"

#include <assert.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

static int pmemstream_is_initialized(struct pmemstream *stream)
{
	if (strcmp(stream->header->signature, PMEMSTREAM_SIGNATURE) != 0) {
		return -1;
	}
	if (stream->header->block_size != stream->block_size) {
		return -1; // todo: fail with incorrect args or something
	}
	if (stream->header->stream_size != stream->stream_size) {
		return -1; // todo: fail with incorrect args or something
	}

	return 0;
}

static void pmemstream_init(struct pmemstream *stream)
{
	stream->data.memset(stream->header->signature, 0, PMEMSTREAM_SIGNATURE_SIZE,
			    PMEM2_F_MEM_NONTEMPORAL | PMEM2_F_MEM_NODRAIN);

	span_create_empty(&stream->data, 0, stream->usable_size - sizeof(struct entry));

	stream->header->stream_size = stream->stream_size;
	stream->header->block_size = stream->block_size;
	stream->data.persist(stream->header, sizeof(struct pmemstream_header));

	stream->data.memcpy(stream->header->signature, PMEMSTREAM_SIGNATURE, strlen(PMEMSTREAM_SIGNATURE),
			    PMEM2_F_MEM_NONTEMPORAL);
}

static size_t pmemstream_header_size_aligned(size_t block_size)
{
	return ALIGN_UP(sizeof(struct pmemstream_header), block_size);
}

static size_t pmemstream_usable_size(size_t stream_size, size_t block_size)
{
	assert(stream_size >= pmemstream_header_size_aligned(block_size));
	assert(stream_size - pmemstream_header_size_aligned(block_size) >= block_size);
	return ALIGN_DOWN(stream_size - pmemstream_header_size_aligned(block_size), block_size);
}

static int pmemstream_validate_sizes(size_t block_size, struct pmem2_map *map)
{
	if (block_size == 0) {
		return -1;
	}

	/* XXX: change 64 to CACHELINE_SIZE */
	if (block_size % 64 != 0) {
		return -1;
	}

	if (!IS_POW2(block_size)) {
		return -1;
	}

	if (map == NULL) {
		return -1;
	}

	size_t stream_size = pmem2_map_get_size(map);
	if (stream_size > PTRDIFF_MAX) {
		return -1;
	}

	if (stream_size <= pmemstream_header_size_aligned(block_size)) {
		return -1;
	}

	if (pmemstream_usable_size(stream_size, block_size) <= sizeof(struct region)) {
		return -1;
	}

	if (pmemstream_usable_size(stream_size, block_size) < block_size) {
		return -1;
	}

	return 0;
}

int pmemstream_from_map(struct pmemstream **stream, size_t block_size, struct pmem2_map *map)
{
	if (pmemstream_validate_sizes(block_size, map)) {
		return -1;
	}

	struct pmemstream *s = malloc(sizeof(struct pmemstream));
	if (!s) {
		return -1;
	}

	size_t spans_offset = pmemstream_header_size_aligned(block_size);
	s->header = pmem2_map_get_address(map);
	s->stream_size = pmem2_map_get_size(map);
	s->usable_size = pmemstream_usable_size(s->stream_size, block_size);
	s->block_size = block_size;

	s->data.spans = (span_bytes *)(((uint8_t *)pmem2_map_get_address(map)) + spans_offset);
	s->data.memcpy = pmem2_get_memcpy_fn(map);
	s->data.memset = pmem2_get_memset_fn(map);
	s->data.persist = pmem2_get_persist_fn(map);
	s->data.flush = pmem2_get_flush_fn(map);
	s->data.drain = pmem2_get_drain_fn(map);

	if (pmemstream_is_initialized(s) != 0) {
		pmemstream_init(s);
	}

	s->region_runtimes_map = region_runtimes_map_new();
	if (!s->region_runtimes_map) {
		goto err;
	}

	*stream = s;
	return 0;

err:
	free(s);
	return -1;
}

void pmemstream_delete(struct pmemstream **stream)
{
	struct pmemstream *s = *stream;
	region_runtimes_map_destroy(s->region_runtimes_map);
	free(s);
	*stream = NULL;
}

// stream owns the region object - the user gets a reference, but it's not
// necessary to hold on to it and explicitly delete it.
/* XXX: add test for multiple regions, when supported */
int pmemstream_region_allocate(struct pmemstream *stream, size_t size, struct pmemstream_region *region)
{
	const uint64_t offset = 0;
	assert(offset % stream->block_size == 0);
	struct span_base *span_base = span_offset_to_span_ptr(&stream->data, offset);

	if (span_get_type(span_base) != SPAN_EMPTY) {
		return -1;
	}

	if (size == 0) {
		return -1;
	}

	struct span_region span_region = {
		.span_base = span_base_create(size, SPAN_REGION);
	};

	size_t total_size = ALIGN_UP(span_get_total_size(&span_region), stream->block_size);
	if (total_size > span_get_size(span_base) + sizeof(struct empty_span))
		return -1;

	stream->data->memcpy(span_base, &span_region, sizeof(span_region));
	region->offset = offset;

#ifndef NDEBUG
	struct span_region* stored_span_region = (struct span_region*) span_base;
	/* XXX: use CACHELINE_SIZE instead of 64 */
	assert(((uintptr_t)stored_span_region->data) % 64 == 0);
#endif

	return 0;
}

size_t pmemstream_region_size(struct pmemstream *stream, struct pmemstream_region region)
{
	struct span_base *span_region = span_offset_to_span_ptr(&stream->data, offset);
	assert(span_get_type(span_region) == SPAN_REGION);
	return span_get_size(span_region);
}

int pmemstream_region_free(struct pmemstream *stream, struct pmemstream_region region)
{
	struct span_region *span_region = (struct span_region*) span_offset_to_span_ptr(&stream->data, region.offset);

	if (span_get_type(&span_region->span_base) != SPAN_REGION)
		return -1;

	struct span_empty span_empty = {
		.span_base = span_base_create(stream->usable_size - sizeof(span_empty));
	};

	stream->data->memcpy(span_region, &span_empty, sizeof(span_empty));

	region_runtimes_map_remove(stream->region_runtimes_map, region);

	return 0;
}

// returns pointer to the data of the entry
const void *pmemstream_entry_data(struct pmemstream *stream, struct pmemstream_entry entry)
{
	struct span_entry *span_entry = (struct span_entry*) span_offset_to_span_ptr(&stream->data, region.offset);
	return span_entry->data;
}

// returns the size of the entry
size_t pmemstream_entry_length(struct pmemstream *stream, struct pmemstream_entry entry)
{
	struct span_entry *span_entry = (struct span_entry*) span_offset_to_span_ptr(&stream->data, region.offset);
	return span_get_size(&span_entry->span_base);
}

int pmemstream_region_runtime_initialize(struct pmemstream *stream, struct pmemstream_region region,
					 struct pmemstream_region_runtime **region_runtime)
{
	int ret = region_runtimes_map_get_or_create(stream->region_runtimes_map, region, region_runtime);
	if (ret) {
		return ret;
	}

	assert(*region_runtime);

	return region_runtime_initialize_clear_locked(stream, region, *region_runtime);
}

static size_t pmemstream_entry_total_size_aligned(struct span_entry *span_entry)
{
	return ALIGN_UP(span_get_total_size(span_entry), sizeof(span_bytes));
}

int pmemstream_reserve(struct pmemstream *stream, struct pmemstream_region region,
		       struct pmemstream_region_runtime *region_runtime, size_t size,
		       struct pmemstream_entry *reserved_entry, void **data_addr)
{
	size_t entry_total_size_span_aligned = pmemstream_entry_total_size_aligned(size);
	struct span_region *span_region = (struct span_region*) span_offset_to_span_ptr(&stream->data, region.offset);
	assert(span_get_type(&span_region->base) == SPAN_REGION);
	int ret = 0;

	if (!region_runtime) {
		ret = pmemstream_region_runtime_initialize(stream, region, &region_runtime);
		if (ret) {
			return ret;
		}
	}

	uint64_t offset = region_runtime_get_append_offset_acquire(region_runtime);
	assert(offset >= region.offset + sizeof(struct span_region));
	if (offset + entry_total_size_span_aligned > region.offset + span_get_total_size(&span_region->span_base)) {
		return -1;
	}

	region_runtime_increase_append_offset(region_runtime, entry_total_size_span_aligned);

	reserved_entry->offset = offset;
	/* data is right after the entry metadata */
	*data_addr = (void *)span_offset_to_span_ptr(&stream->data, offset + sizeof(span_entry));

	return ret;
}

int pmemstream_publish(struct pmemstream *stream, struct pmemstream_region region,
		       struct pmemstream_region_runtime *region_runtime, const void *data, size_t size,
		       struct pmemstream_entry *reserved_entry)
{
	if (!region_runtime) {
		int ret = pmemstream_region_runtime_initialize(stream, region, &region_runtime);
		if (ret) {
			return ret;
		}
	}

	struct span_entry span_entry = {
		.base = span_base_create(size, SPAN_ENTRY),
		.popcount = util_popcount_memory(data, size)
	};

	struct span_base *entry_destination = span_offset_to_span_ptr(&stream->data, reserved_entry->offset);
	stream->data->memcpy(entry_destination, &span_entry, sizeof(span_entry), PMEM2_F_MEM_NOFLUSH);
	/* 'data' is already copied - we only need to persist. */
	stream->data->persist(entry_destination, pmemstream_entry_total_size_aligned(&span_entry));

	region_runtime_increase_committed_offset(region_runtime, pmemstream_entry_total_size_aligned(&span_entry));

	return 0;
}

// synchronously appends data buffer to the end of the region
int pmemstream_append(struct pmemstream *stream, struct pmemstream_region region,
		      struct pmemstream_region_runtime *region_runtime, const void *data, size_t size,
		      struct pmemstream_entry *new_entry)
{
	if (!region_runtime) {
		int ret = pmemstream_region_runtime_initialize(stream, region, &region_runtime);
		if (ret) {
			return ret;
		}
	}

	struct pmemstream_entry reserved_entry;
	void *reserved_dest;
	int ret = pmemstream_reserve(stream, region, region_runtime, size, &reserved_entry, &reserved_dest);
	if (ret) {
		return ret;
	}

	struct span_entry span_entry = {
		.base = span_base_create(size, SPAN_ENTRY),
		.popcount = util_popcount_memory(data, size)
	};

	struct span_base *entry_destination = span_offset_to_span_ptr(&stream->data, reserved_entry->offset);
	stream->data->memcpy(entry_destination, &span_entry, sizeof(span_entry), PMEM2_F_MEM_NOFLUSH);
	stream->data->memcpy(entry_destination + sizeof(span_entry), data, size, PMEM2_F_MEM_NOFLUSH);
	stream->data->persist(entry_destination, pmemstream_entry_total_size_aligned(size));

	region_runtime_increase_committed_offset(region_runtime, pmemstream_entry_total_size_aligned(size));

	if (new_entry) {
		new_entry->offset = reserved_entry.offset;
	}

	return 0;
}
