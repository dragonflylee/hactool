#include <stdlib.h>
#include <string.h>
#include "nca_compress.h"
#include "utils.h"

/* Bundled decompressors. */
#include "lz4.h"
#include "zstd-zbic/zstd.h"

#define COMPRESS_TYPE_ZSTD 2

/* Number of entries that fit in an entry-set node. */
static int compress_entry_count_per_node(uint64_t node_size, uint64_t entry_size) {
    return (int)((node_size - sizeof(bktr_node_header_t)) / entry_size);
}

static int compress_offset_count_per_node(uint64_t node_size) {
    return (int)((node_size - sizeof(bktr_node_header_t)) / sizeof(int64_t));
}

void nca_compress_init(nca_compress_ctx_t *ctx) {
    memset(ctx, 0, sizeof(*ctx));
}

void nca_compress_free(nca_compress_ctx_t *ctx) {
    free(ctx->cache);
    nca_compress_init(ctx);
}

/* Reads `count` bytes from the bucket-tree storage at `offset` (relative to the
 * compression table base). */
static size_t compress_tree_read(bktr_tree_t *tree, uint64_t offset, void *buffer, size_t count) {
    return tree->storage_read(tree->storage_ctx, tree->base_offset + offset, buffer, count);
}

/* Reads a full node from the node storage. Buffer must be at least
 * tree->node_size bytes. */
static int compress_read_node(bktr_tree_t *tree, uint64_t node_index, void *buffer) {
    return compress_tree_read(tree, tree->node_storage_offset + node_index * tree->node_size,
        buffer, tree->node_size) == tree->node_size;
}

/* Reads an entry set from the entry storage. */
static int compress_read_entry_set(bktr_tree_t *tree, int entry_set_index, void *buffer) {
    return compress_tree_read(tree, tree->entry_storage_offset + (uint64_t)entry_set_index * tree->node_size,
        buffer, tree->node_size) == tree->node_size;
}

/* Binary search over an array of uint64 sorted offsets, returning the index of
 * the last element <= value. Returns -1 if none. */
static int compress_bsearch_offsets(const uint64_t *offsets, int count, uint64_t value) {
    int lo = 0, hi = count - 1, result = -1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        if (offsets[mid] <= value) {
            result = mid;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return result;
}

/* Finds the entry-set index that contains `virtual_offset`. */
static int compress_find_entry_set(bktr_tree_t *tree, uint64_t virtual_offset, uint64_t *out_start_offset) {
    unsigned char *node = malloc(tree->node_size);
    if (node == NULL) return -1;

    int result = -1;
    if (!compress_read_node(tree, 0, node)) {
        free(node);
        return -1;
    }

    bktr_node_header_t *header = (bktr_node_header_t *)node;
    uint64_t *offsets = (uint64_t *)(node + sizeof(bktr_node_header_t));
    int entry_set_index = -1;

    /* The L1 node stores the start offset of each entry set (or of each L2 node
     * when an L2 level exists). */
    int l2_count = tree->node_l2_count;
    if (l2_count == 0) {
        int index = compress_bsearch_offsets(offsets, header->entry_count, virtual_offset);
        if (index < 0) {
            free(node);
            return -1;
        }
        entry_set_index = index;
        if (out_start_offset != NULL) *out_start_offset = offsets[0];
    } else {
        /* An L2 index level exists. The L1 node stores L2 node offsets. */
        int index = compress_bsearch_offsets(offsets, header->entry_count, virtual_offset);
        if (index < 0) {
            free(node);
            return -1;
        }

        if (index >= tree->offset_count) {
            free(node);
            return -1;
        }

        if (!compress_read_node(tree, (uint64_t)(index + 1), node)) {
            free(node);
            return -1;
        }
        bktr_node_header_t *l2_header = (bktr_node_header_t *)node;
        uint64_t *l2_offsets = (uint64_t *)(node + sizeof(bktr_node_header_t));
        int l2_index = compress_bsearch_offsets(l2_offsets, l2_header->entry_count, virtual_offset);
        if (l2_index < 0) {
            free(node);
            return -1;
        }
        (void)l2_header;
        entry_set_index = (tree->offset_count - header->entry_count) + tree->offset_count * index + l2_index;
    }

    result = entry_set_index;
    free(node);
    return result;
}

int nca_compress_setup(nca_compress_ctx_t *ctx, const nca_compression_info_t *info) {
    uint32_t magic;
    int32_t entry_count;

    memcpy(&magic, info->table_header, 4);
    if (magic != MAGIC_BKTR) {
        return -1;
    }
    memcpy(&entry_count, info->table_header + 8, 4);
    if (entry_count <= 0) {
        return -1;
    }

    ctx->tree.node_size = BKTR_NODE_SIZE;
    ctx->tree.entry_size = sizeof(nca_compress_entry_t);
    ctx->tree.entry_count = entry_count;

    int entry_count_per_node = compress_entry_count_per_node(ctx->tree.node_size, ctx->tree.entry_size);
    ctx->tree.offset_count = compress_offset_count_per_node(ctx->tree.node_size);
    ctx->tree.entry_set_count = (entry_count + entry_count_per_node - 1) / entry_count_per_node;

    if (ctx->tree.entry_set_count <= ctx->tree.offset_count) {
        ctx->tree.node_l2_count = 0;
    } else {
        int offset_count_per_node = ctx->tree.offset_count;
        int node_l2_count = (ctx->tree.entry_set_count + offset_count_per_node - 1) / offset_count_per_node;
        ctx->tree.node_l2_count = (ctx->tree.entry_set_count -
            (offset_count_per_node - (node_l2_count - 1)) + offset_count_per_node - 1) / offset_count_per_node;
    }

    /* Node/entry storages immediately follow the compression table offset. */
    uint64_t node_storage_size = (uint64_t)(1 + ctx->tree.node_l2_count) * ctx->tree.node_size;
    uint64_t entry_storage_size = (uint64_t)ctx->tree.entry_set_count * ctx->tree.node_size;

    if (node_storage_size + entry_storage_size > info->table_size) {
        return -1;
    }

    ctx->tree.node_storage_offset = 0; /* Relative to the compression table base. */
    ctx->tree.entry_storage_offset = node_storage_size;
    ctx->tree.base_offset = info->table_offset;

    /* Determine the virtual size and start offset from the L1 node. */
    unsigned char *node = malloc(ctx->tree.node_size);
    if (node == NULL) {
        return -1;
    }
    if (!compress_read_node(&ctx->tree, 0, node)) {
        free(node);
        return -1;
    }
    bktr_node_header_t *header = (bktr_node_header_t *)node;
    uint64_t *offsets = (uint64_t *)(node + sizeof(bktr_node_header_t));

    ctx->virtual_size = (uint64_t)header->offset_end;
    ctx->tree.end_offset = (uint64_t)header->offset_end;
    if (ctx->tree.node_l2_count != 0 && header->entry_count < ctx->tree.offset_count) {
        /* L2 offsets partially stored on L1: start offset is after the index offsets. */
        ctx->tree.start_offset = offsets[header->entry_count];
    } else {
        ctx->tree.start_offset = offsets[0];
    }
    free(node);

    ctx->is_present = 1;
    ctx->cache_valid = 0;
    ctx->tree.is_initialized = 1;
    return 0;
}

/* Reads and decompresses the entry that contains `virtual_offset` into the
 * cache. Returns 0 on success. */
static int compress_fill_cache(nca_compress_ctx_t *ctx, uint64_t virtual_offset) {
    bktr_tree_t *tree = &ctx->tree;

    int entry_set_index = compress_find_entry_set(tree, virtual_offset, NULL);
    if (entry_set_index < 0) {
        return -1;
    }

    unsigned char *entry_set = malloc(tree->node_size);
    if (entry_set == NULL) {
        return -1;
    }
    if (!compress_read_entry_set(tree, entry_set_index, entry_set)) {
        free(entry_set);
        return -1;
    }

    bktr_node_header_t *header = (bktr_node_header_t *)entry_set;
    unsigned char *entries = entry_set + sizeof(bktr_node_header_t);
    int entry_count = header->entry_count;

    /* Build an array of virtual offsets for binary search. */
    uint64_t *v_offsets = malloc(sizeof(uint64_t) * (size_t)entry_count);
    if (v_offsets == NULL) {
        free(entry_set);
        return -1;
    }
    for (int i = 0; i < entry_count; i++) {
        nca_compress_entry_t *e = (nca_compress_entry_t *)(entries + (size_t)i * tree->entry_size);
        v_offsets[i] = e->virtual_offset;
    }
    int index = compress_bsearch_offsets(v_offsets, entry_count, virtual_offset);
    free(v_offsets);
    if (index < 0) {
        free(entry_set);
        return -1;
    }

    nca_compress_entry_t *entry = (nca_compress_entry_t *)(entries + (size_t)index * tree->entry_size);

    /* Determine the virtual size of this entry. */
    uint64_t entry_virtual_offset = entry->virtual_offset;
    uint64_t next_virtual_offset;
    if (index + 1 < entry_count) {
        nca_compress_entry_t *next = (nca_compress_entry_t *)(entries + (size_t)(index + 1) * tree->entry_size);
        next_virtual_offset = next->virtual_offset;
    } else if (entry_set_index + 1 < tree->entry_set_count) {
        unsigned char *next_set = malloc(tree->node_size);
        if (next_set == NULL || !compress_read_entry_set(tree, entry_set_index + 1, next_set)) {
            free(next_set);
            free(entry_set);
            return -1;
        }
        bktr_node_header_t *next_header = (bktr_node_header_t *)next_set;
        nca_compress_entry_t *next_entries = (nca_compress_entry_t *)(next_set + sizeof(bktr_node_header_t));
        (void)next_header;
        next_virtual_offset = next_entries[0].virtual_offset;
        free(next_set);
    } else {
        next_virtual_offset = tree->end_offset;
    }

    uint64_t entry_virtual_size = next_virtual_offset - entry_virtual_offset;

    if (entry_virtual_size > ctx->cache_capacity) {
        void *new_cache = realloc(ctx->cache, (size_t)entry_virtual_size);
        if (new_cache == NULL) {
            free(entry_set);
            return -1;
        }
        ctx->cache = new_cache;
        ctx->cache_capacity = (size_t)entry_virtual_size;
    }

    uint8_t compression_type = entry->compression_type;
    uint32_t physical_size = entry->physical_size;

    int ok = 1;
    if (compression_type == COMPRESS_TYPE_NONE) {
        /* Stored verbatim. */
        if (ctx->base_read(ctx->base_ctx, entry->physical_offset, ctx->cache, entry_virtual_size) != entry_virtual_size) {
            ok = 0;
        }
    } else if (compression_type == COMPRESS_TYPE_ZEROED) {
        memset(ctx->cache, 0, (size_t)entry_virtual_size);
    } else if (compression_type == COMPRESS_TYPE_LZ4) {
        unsigned char *compressed = malloc(physical_size ? physical_size : 1);
        if (compressed == NULL ||
            ctx->base_read(ctx->base_ctx, entry->physical_offset, compressed, physical_size) != physical_size ||
            LZ4_decompress_safe((const char *)compressed, (char *)ctx->cache, (int)physical_size,
                (int)entry_virtual_size) != (int)entry_virtual_size) {
            ok = 0;
        }
        free(compressed);
    } else if (compression_type == COMPRESS_TYPE_ZSTD) {
        unsigned char *compressed = malloc(physical_size ? physical_size : 1);
        size_t result = 0;
        if (compressed != NULL &&
            ctx->base_read(ctx->base_ctx, entry->physical_offset, compressed, physical_size) == physical_size) {
            result = ZSTD_decompress(ctx->cache, (size_t)entry_virtual_size, compressed, physical_size);
        }
        if (result != (size_t)entry_virtual_size) {
            ok = 0;
        }
        free(compressed);
    } else {
        ok = 0;
    }

    free(entry_set);

    if (!ok) {
        return -1;
    }

    ctx->cache_offset = entry_virtual_offset;
    ctx->cache_size = entry_virtual_size;
    ctx->cache_valid = 1;
    return 0;
}

size_t nca_compress_read(nca_compress_ctx_t *ctx, uint64_t offset, void *buffer, size_t count) {
    if (!ctx->is_present) {
        return 0;
    }

    size_t total = 0;
    unsigned char *out = buffer;

    while (total < count) {
        uint64_t cur = offset + total;

        if (!ctx->cache_valid || cur < ctx->cache_offset ||
            cur >= ctx->cache_offset + ctx->cache_size) {
            if (compress_fill_cache(ctx, cur) != 0) {
                return total;
            }
        }

        uint64_t in_cache = cur - ctx->cache_offset;
        size_t available = (size_t)(ctx->cache_size - in_cache);
        size_t to_copy = count - total;
        if (to_copy > available) {
            to_copy = available;
        }

        memcpy(out + total, (unsigned char *)ctx->cache + in_cache, to_copy);
        total += to_copy;
    }

    return total;
}
