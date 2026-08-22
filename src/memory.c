#include <limine.h>
#include <memory.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

__attribute__((used, section(".limine_requests")))
static volatile struct limine_memmap_request memmap_request = {
    .id = LIMINE_MEMMAP_REQUEST_ID,
    .revision = 0
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_hhdm_request hhdm_request = {
    .id = LIMINE_HHDM_REQUEST_ID,
    .revision = 0
};

struct limine_memmap_response *memory_map(void) {
    return memmap_request.response;
}

uint64_t hhdm_offset(void) {
    return hhdm_request.response != NULL ? hhdm_request.response->offset : 0;
}

const char *memmap_type_name(uint64_t type) {
    switch (type) {
    case LIMINE_MEMMAP_USABLE:                 return "usable";
    case LIMINE_MEMMAP_RESERVED:               return "reserved";
    case LIMINE_MEMMAP_ACPI_RECLAIMABLE:       return "ACPI reclaimable";
    case LIMINE_MEMMAP_ACPI_NVS:               return "ACPI NVS";
    case LIMINE_MEMMAP_BAD_MEMORY:             return "bad memory";
    case LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE: return "bootloader";
    case LIMINE_MEMMAP_EXECUTABLE_AND_MODULES: return "kernel/modules";
    case LIMINE_MEMMAP_FRAMEBUFFER:            return "framebuffer";
    case LIMINE_MEMMAP_RESERVED_MAPPED:        return "reserved (mapped)";
    default:                                   return "unknown";
    }
}

#define HEAP_ALIGN     16
#define BLOCK_MAGIC    0xA110C8EDu
#define MIN_HEAP_BYTES (64u * 1024u)

#define FALLBACK_HEAP_BYTES (256u * 1024u)
static uint8_t fallback_heap[FALLBACK_HEAP_BYTES] __attribute__((aligned(HEAP_ALIGN)));

struct block {
    size_t size;
    struct block *prev;
    struct block *next;
    uint32_t free;
    uint32_t magic;
};

static struct block *heap_head;
static size_t heap_bytes;
static size_t stat_used;
static size_t stat_allocs;
static size_t stat_frees;
static bool heap_ready;
static bool heap_is_fallback;

#define DMA_LOW_LIMIT (1ULL << 32)
static uint64_t low_next, low_end;

static uint64_t phys_limit;

static size_t align_up(size_t value, size_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

static void *block_payload(struct block *b) {
    return (uint8_t *)b + sizeof(struct block);
}

static struct block *payload_block(void *ptr) {
    return (struct block *)((uint8_t *)ptr - sizeof(struct block));
}

static void dma_low_init(struct limine_memmap_response *mm,
                         const struct limine_memmap_entry *heap_region) {
    if (mm == NULL || heap_region == NULL
     || heap_region->base + heap_region->length <= DMA_LOW_LIMIT) {
        return;
    }

    uint64_t best_len = 0;

    for (uint64_t i = 0; i < mm->entry_count; i++) {
        struct limine_memmap_entry *e = mm->entries[i];

        if (e->type != LIMINE_MEMMAP_USABLE || e == heap_region) {
            continue;
        }

        uint64_t start = e->base < 0x100000 ? 0x100000 : e->base;
        uint64_t end = e->base + e->length;

        if (end > DMA_LOW_LIMIT) {
            end = DMA_LOW_LIMIT;
        }
        if (end <= start || end - start <= best_len) {
            continue;
        }

        best_len = end - start;
        low_next = start;
        low_end = end;
    }
}

bool memory_init(void) {
    if (heap_ready) {
        return !heap_is_fallback;
    }

    uint8_t *base = NULL;
    size_t size = 0;
    struct limine_memmap_entry *heap_region = NULL;

    struct limine_memmap_response *mm = memory_map();
    if (mm != NULL) {
        struct limine_memmap_entry *best = NULL;

        for (uint64_t i = 0; i < mm->entry_count; i++) {
            struct limine_memmap_entry *e = mm->entries[i];

            if (e->base + e->length > phys_limit) {
                phys_limit = e->base + e->length;
            }
            if (e->type != LIMINE_MEMMAP_USABLE) {
                continue;
            }
            if (best == NULL || e->length > best->length) {
                best = e;
            }
        }

        if (best != NULL && best->length >= MIN_HEAP_BYTES) {

            base = (uint8_t *)(uintptr_t)(best->base + hhdm_offset());
            size = (size_t)best->length;
            heap_region = best;
        }
    }

    if (base == NULL) {
        base = fallback_heap;
        size = sizeof fallback_heap;
        heap_is_fallback = true;
    }

    uintptr_t start = align_up((uintptr_t)base, HEAP_ALIGN);
    size -= (size_t)(start - (uintptr_t)base);
    size &= ~(size_t)(HEAP_ALIGN - 1);

    heap_bytes = size;
    heap_head = (struct block *)start;
    heap_head->size = size - sizeof(struct block);
    heap_head->prev = NULL;
    heap_head->next = NULL;
    heap_head->free = 1;
    heap_head->magic = BLOCK_MAGIC;
    heap_ready = true;

    dma_low_init(mm, heap_region);

    return !heap_is_fallback;
}

static void split_block(struct block *b, size_t size) {
    if (b->size < size + sizeof(struct block) + HEAP_ALIGN) {
        return;
    }

    struct block *tail = (struct block *)((uint8_t *)block_payload(b) + size);

    tail->size = b->size - size - sizeof(struct block);
    tail->free = 1;
    tail->magic = BLOCK_MAGIC;
    tail->prev = b;
    tail->next = b->next;

    if (tail->next != NULL) {
        tail->next->prev = tail;
    }
    b->next = tail;
    b->size = size;
}

static void merge_with_next(struct block *b) {
    struct block *n = b->next;

    if (n == NULL || !n->free) {
        return;
    }

    b->size += sizeof(struct block) + n->size;
    b->next = n->next;
    if (n->next != NULL) {
        n->next->prev = b;
    }
    n->magic = 0;
}

void *malloc(size_t size) {
    if (!heap_ready) {
        memory_init();
    }

    if (size == 0) {
        size = 1;
    }
    if (size > SIZE_MAX - sizeof(struct block) - HEAP_ALIGN) {
        return NULL;
    }
    size = align_up(size, HEAP_ALIGN);

    for (struct block *b = heap_head; b != NULL; b = b->next) {
        if (!b->free || b->size < size) {
            continue;
        }

        split_block(b, size);
        b->free = 0;
        stat_used += b->size;
        stat_allocs++;

        return block_payload(b);
    }

    return NULL;
}

void free(void *ptr) {
    if (ptr == NULL) {
        return;
    }

    struct block *b = payload_block(ptr);

    if (b->magic != BLOCK_MAGIC) {
        printf("free(%p): not a heap block (magic %#x), ignoring\n", ptr, b->magic);
        return;
    }
    if (b->free) {
        printf("free(%p): double free, ignoring\n", ptr);
        return;
    }

    b->free = 1;
    stat_used -= b->size;
    stat_frees++;

    merge_with_next(b);
    if (b->prev != NULL && b->prev->free) {
        merge_with_next(b->prev);
    }
}

void *calloc(size_t nmemb, size_t size) {
    if (nmemb != 0 && size > SIZE_MAX / nmemb) {
        return NULL;
    }

    size_t total = nmemb * size;
    void *ptr = malloc(total);

    if (ptr == NULL) {
        return NULL;
    }

    return memset(ptr, 0, total);
}

void *realloc(void *ptr, size_t size) {
    if (ptr == NULL) {
        return malloc(size);
    }
    if (size == 0) {
        free(ptr);
        return NULL;
    }

    struct block *b = payload_block(ptr);

    if (b->magic != BLOCK_MAGIC) {
        printf("realloc(%p): not a heap block, ignoring\n", ptr);
        return NULL;
    }

    size_t want = align_up(size, HEAP_ALIGN);
    size_t old_size = b->size;

    if (b->size < want && b->next != NULL && b->next->free
        && b->size + sizeof(struct block) + b->next->size >= want) {
        merge_with_next(b);
    }

    if (b->size >= want) {
        split_block(b, want);

        if (b->size >= old_size) {
            stat_used += b->size - old_size;
        } else {
            stat_used -= old_size - b->size;
        }

        if (b->next != NULL && b->next->free) {
            merge_with_next(b->next);
        }

        return ptr;
    }

    void *fresh = malloc(size);

    if (fresh == NULL) {
        return NULL;
    }

    memcpy(fresh, ptr, old_size < size ? old_size : size);
    free(ptr);

    return fresh;
}

void heap_get_stats(struct heap_stats *out) {
    if (out == NULL) {
        return;
    }

    memset(out, 0, sizeof *out);

    if (!heap_ready) {
        return;
    }

    out->total = heap_bytes;
    out->used = stat_used;
    out->allocations = stat_allocs;
    out->frees = stat_frees;

    for (struct block *b = heap_head; b != NULL; b = b->next) {
        out->blocks++;
        out->overhead += sizeof(struct block);

        if (b->free) {
            out->free_blocks++;
            if (b->size > out->largest_free) {
                out->largest_free = b->size;
            }
        }
    }
}

void *dma_alloc(size_t size, size_t align, bool low, uint64_t *phys_out) {
    if (!heap_ready) {
        memory_init();
    }

    if (heap_is_fallback || hhdm_offset() == 0 || size == 0 || phys_out == NULL) {
        return NULL;
    }

    if (align < HEAP_ALIGN) {
        align = HEAP_ALIGN;
    }

    if (low && low_end != 0) {
        uint64_t phys = (low_next + align - 1) & ~(uint64_t)(align - 1);

        if (phys < low_next || phys + size > low_end) {
            return NULL;
        }

        low_next = phys + size;
        *phys_out = phys;

        return memset((void *)(uintptr_t)(phys + hhdm_offset()), 0, size);
    }

    void *block = malloc(size + align);

    if (block == NULL) {
        return NULL;
    }

    uintptr_t virt = align_up((uintptr_t)block, align);
    uint64_t phys = (uint64_t)virt - hhdm_offset();

    if (low && phys + size > DMA_LOW_LIMIT) {
        free(block);
        return NULL;
    }

    *phys_out = phys;

    return memset((void *)virt, 0, size);
}

bool dma_reachable(const void *ptr, size_t size, bool low) {
    uint64_t hhdm = hhdm_offset();
    uint64_t virt = (uint64_t)(uintptr_t)ptr;

    if (heap_is_fallback || hhdm == 0 || virt < hhdm) {
        return false;
    }

    uint64_t phys = virt - hhdm;

    if (phys + size > phys_limit || phys + size < phys) {
        return false;
    }

    return !low || phys + size <= DMA_LOW_LIMIT;
}

#define PAGE_SIZE   4096
#define PTE_PRESENT (1ULL << 0)
#define PTE_WRITE   (1ULL << 1)
#define PTE_USER    (1ULL << 2)
#define PTE_PWT     (1ULL << 3)
#define PTE_PCD     (1ULL << 4)
#define PTE_HUGE    (1ULL << 7)
#define PTE_PAT_4K  (1ULL << 7)
#define PTE_PAT_BIG (1ULL << 12)
#define PTE_ADDR    0x000FFFFFFFFFF000ULL

#define HUGE_1G     (1ULL << 30)
#define HUGE_2M     (1ULL << 21)

static uint64_t *table_at(uint64_t phys) {
    return (uint64_t *)(uintptr_t)(phys + hhdm_offset());
}

static uint64_t alloc_table(void) {
    void *block = malloc(2 * PAGE_SIZE);

    if (block == NULL) {
        return 0;
    }

    uintptr_t virt = align_up((uintptr_t)block, PAGE_SIZE);

    memset((void *)virt, 0, PAGE_SIZE);

    return (uint64_t)virt - hhdm_offset();
}

static uint64_t *split_huge(uint64_t *table, size_t index, uint64_t huge_size) {
    uint64_t entry = table[index];
    uint64_t child_phys = alloc_table();

    if (child_phys == 0) {
        return NULL;
    }

    uint64_t *child = table_at(child_phys);
    uint64_t base = entry & PTE_ADDR;
    uint64_t step = huge_size / 512;
    uint64_t flags = entry & ~(PTE_ADDR | PTE_HUGE | PTE_PAT_BIG);

    if (step == PAGE_SIZE) {

        if (entry & PTE_PAT_BIG) {
            flags |= PTE_PAT_4K;
        }
    } else {

        flags |= PTE_HUGE | (entry & PTE_PAT_BIG);
    }

    for (size_t i = 0; i < 512; i++) {
        child[i] = (base + i * step) | flags;
    }

    table[index] = child_phys | PTE_PRESENT | PTE_WRITE | (entry & PTE_USER);

    return child;
}

static uint64_t *next_level(uint64_t *table, size_t index, uint64_t huge_size) {
    uint64_t entry = table[index];

    if (entry & PTE_PRESENT) {
        return (entry & PTE_HUGE) ? split_huge(table, index, huge_size)
                                  : table_at(entry & PTE_ADDR);
    }

    uint64_t phys = alloc_table();

    if (phys == 0) {
        return NULL;
    }

    table[index] = phys | PTE_PRESENT | PTE_WRITE;

    return table_at(phys);
}

static bool map_page(uint64_t virt, uint64_t phys, uint64_t flags) {
    uint64_t cr3;

    asm volatile ("mov %%cr3, %0" : "=r" (cr3));

    uint64_t *pml4 = table_at(cr3 & PTE_ADDR);
    uint64_t *pdpt = next_level(pml4, (virt >> 39) & 0x1FF, 0);
    uint64_t *pd   = pdpt != NULL ? next_level(pdpt, (virt >> 30) & 0x1FF, HUGE_1G) : NULL;
    uint64_t *pt   = pd != NULL ? next_level(pd, (virt >> 21) & 0x1FF, HUGE_2M) : NULL;

    if (pt == NULL) {
        return false;
    }

    pt[(virt >> 12) & 0x1FF] = (phys & PTE_ADDR) | flags;

    asm volatile ("invlpg (%0)" : : "r" (virt) : "memory");

    return true;
}

void *mmio_map(uint64_t phys, size_t size) {

    if (hhdm_offset() == 0 || heap_is_fallback || size == 0) {
        return NULL;
    }

    uint64_t first = phys & ~(uint64_t)(PAGE_SIZE - 1);
    uint64_t last = (phys + size - 1) & ~(uint64_t)(PAGE_SIZE - 1);

    for (uint64_t page = first; page <= last; page += PAGE_SIZE) {
        if (!map_page(page + hhdm_offset(), page,
                      PTE_PRESENT | PTE_WRITE | PTE_PCD | PTE_PWT)) {
            return NULL;
        }
    }

    return (void *)(uintptr_t)(phys + hhdm_offset());
}
