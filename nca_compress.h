#ifndef HACTOOL_NCA_COMPRESS_H
#define HACTOOL_NCA_COMPRESS_H

#include <stdint.h>
#include <stdio.h>
#include "types.h"

/* NCA FS header compression info (offset 0x178 within the FS header). */
typedef struct {
    uint64_t table_offset;
    uint64_t table_size;
    uint8_t  table_header[0x10]; /* BucketTree.Header */
    uint64_t reserved;
} nca_compression_info_t;

/* BucketTree (BKTR) constants. */
#define MAGIC_BKTR 0x52544B42 /* "BKTR" */
#define BKTR_NODE_SIZE 0x4000

/* BucketTree node header. */
typedef struct {
    int32_t  index;
    int32_t  entry_count;
    int64_t  offset_end;
} bktr_node_header_t;

/* CompressedStorage entry (LibHac CompressedStorage.Entry), size 0x18. */
typedef struct {
    uint64_t virtual_offset;
    uint64_t physical_offset;
    uint8_t  compression_type;
    int8_t   compression_level;
    uint32_t physical_size;
} nca_compress_entry_t;

enum nca_compress_type {
    COMPRESS_TYPE_NONE   = 0,
    COMPRESS_TYPE_ZEROED = 1,
    COMPRESS_TYPE_LZ4    = 3
};

/* Bucket tree used by an NCA compression layer. */
typedef struct {
    int is_initialized;

    /* BucketTree parameters. */
    uint64_t node_size;
    uint64_t entry_size;
    int32_t  entry_count;
    int32_t  offset_count;      /* (node_size - sizeof(node_header)) / 8 */
    int32_t  entry_set_count;   /* number of entry-set nodes */
    int32_t  node_l2_count;     /* number of L2 index nodes (0 if offsets fit in L1) */

    /* Offsets (relative to the compression table base) of the node/entry storages. */
    uint64_t node_storage_offset;
    uint64_t entry_storage_offset;

    /* Offset of the compression table itself, relative to the data layer base. */
    uint64_t base_offset;

    /* Virtual range. */
    uint64_t start_offset;
    uint64_t end_offset;

    /* Raw storage accessor for the compression table region. `offset` is
     * relative to the data layer base. */
    void     *storage_ctx;
    size_t  (*storage_read)(void *ctx, uint64_t offset, void *buffer, size_t count);
} bktr_tree_t;

/* A decompressed storage that decompresses `base_storage` (the IVFC data layer). */
typedef struct {
    int is_present;

    uint64_t data_offset;      /* Section-relative offset of the data layer base. */
    uint64_t virtual_size;     /* Decompressed size (tree end_offset). */

    bktr_tree_t tree;

    /* Raw accessor for the underlying (still-encrypted) section. Reads are
     * relative to the data layer base. */
    void     *base_ctx;
    size_t  (*base_read)(void *ctx, uint64_t offset, void *buffer, size_t count);

    /* Single-block cache. */
    void    *cache;
    size_t   cache_capacity;
    uint64_t cache_offset;     /* Virtual offset of the cached entry. */
    uint64_t cache_size;       /* Entry virtual size. */
    int      cache_valid;
} nca_compress_ctx_t;

void nca_compress_init(nca_compress_ctx_t *ctx);
void nca_compress_free(nca_compress_ctx_t *ctx);

/* Set up the bucket tree from the on-disk node/entry storages. Returns 0 on
 * success. */
int nca_compress_setup(nca_compress_ctx_t *ctx, const nca_compression_info_t *info);

/* Read `count` bytes at virtual `offset`. Returns the number of bytes read. */
size_t nca_compress_read(nca_compress_ctx_t *ctx, uint64_t offset, void *buffer, size_t count);

#endif
