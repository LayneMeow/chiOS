#ifndef _MEMORY_H
#define _MEMORY_H

#include <limine.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct heap_stats {
    size_t total;
    size_t used;
    size_t overhead;
    size_t largest_free;
    size_t blocks;
    size_t free_blocks;
    size_t allocations;
    size_t frees;
};

bool memory_init(void);

void heap_get_stats(struct heap_stats *out);

struct limine_memmap_response *memory_map(void);

uint64_t hhdm_offset(void);

const char *memmap_type_name(uint64_t type);

void *dma_alloc(size_t size, size_t align, bool low, uint64_t *phys_out);

bool dma_reachable(const void *ptr, size_t size, bool low);

void *mmio_map(uint64_t phys, size_t size);

#endif
