// SPDX-License-Identifier: BSD-3-Clause
/* Copyright 2021, Intel Corporation */

#ifndef LIBPMEMSTREAM_H
#define LIBPMEMSTREAM_H 1

#include <libpmem2.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

struct pmemstream;
struct pmemstream_tx;
struct pmemstream_entry_iterator;
struct pmemstream_region_iterator;
struct pmemstream_region {
	uint64_t offset;
};
struct pmemstream_region_context;

struct pmemstream_entry {
	size_t offset;
};

// manages lifecycle of the stream. Can be based on top of a raw pmem2_map
// or a pmemset (TBD).
int pmemstream_from_map(struct pmemstream **stream, size_t block_size,
			struct pmem2_map *map);
void pmemstream_delete(struct pmemstream **stream);

// stream owns the region object - the user gets a reference, but it's not
// necessary to hold on to it and explicitly delete it.
int pmemstream_tx_region_allocate(struct pmemstream_tx *tx,
	struct pmemstream *stream, size_t size,
			       struct pmemstream_region *region);

int pmemstream_tx_region_free(struct pmemstream_tx *tx,
	struct pmemstream_region region);

// clearing a region is less expensive than freeing it
int pmemstream_region_clear(struct pmemstream *stream,
			    struct pmemstream_region region);

size_t pmemstream_region_size(struct pmemstream *stream,
			      struct pmemstream_region region);

// creates a new log transaction, this can be used to batch multiple
// stream inserts
int pmemstream_tx_new(struct pmemstream_tx **tx, struct pmemstream *stream);

// commits the written log to stable media
// synchronous transaction commit provide durable linearizability
void pmemstream_tx_commit(struct pmemstream_tx **tx);

// aborts a written log transaction, the consumed space will be reused
void pmemstream_tx_abort(struct pmemstream_tx **tx);

int pmemstream_region_context_new(struct pmemstream_region_context **rcontext,
				  struct pmemstream *stream,
				  struct pmemstream_region region);

void
pmemstream_region_context_delete(struct pmemstream_region_context **rcontext);

// synchronously appends data buffer to the end of the transaction log space
// fails if no space is available
int pmemstream_tx_append(struct pmemstream_tx *tx,
			 struct pmemstream_region_context *rcontext,
			 const void *buf, size_t count,
			 struct pmemstream_entry *entry);

// returns pointer to the data of the entry
void *pmemstream_entry_data(struct pmemstream *stream,
			    struct pmemstream_entry entry);

// returns the size of the entry
size_t pmemstream_entry_length(struct pmemstream *stream,
			       struct pmemstream_entry entry);

// an active pmemstream region or entry prevents the truncate function from
// removing its memory location.
// truncation can only affect regions.

int pmemstream_region_iterator_new(struct pmemstream_region_iterator **iterator,
				   struct pmemstream *stream);

int pmemstream_region_iterator_next(struct pmemstream_region_iterator *iter,
				    struct pmemstream_region *region);

void
pmemstream_region_iterator_delete(struct pmemstream_region_iterator **iterator);

int pmemstream_entry_iterator_new(struct pmemstream_entry_iterator **iterator,
				  struct pmemstream *stream,
				  struct pmemstream_region region);

int pmemstream_entry_iterator_next(struct pmemstream_entry_iterator *iter,
				   struct pmemstream_region *region,
				   struct pmemstream_entry *entry);

void
pmemstream_entry_iterator_delete(struct pmemstream_entry_iterator **iterator);

#endif