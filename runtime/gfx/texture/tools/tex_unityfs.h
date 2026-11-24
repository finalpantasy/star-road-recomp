/* Minimal read-only UnityFS bundle reader for the offline texture checker (tools only, not part of the runtime).
 * Supports LZ4 / LZ4HC / uncompressed blocks, which is all the Switch dump uses. Our own code. */
#ifndef TEX_UNITYFS_H
#define TEX_UNITYFS_H
#include <stdint.h>
#include <stdio.h>

typedef struct { uint32_t usize, csize; uint16_t flags; uint64_t uoff, coff; } ufs_block;
typedef struct { uint64_t off, size; uint32_t flags; char name[160]; } ufs_node;

typedef struct ufs {
    FILE* f;
    ufs_block* blocks; uint32_t nblocks;
    ufs_node* nodes; uint32_t nnodes;
    uint64_t data_start;
    int cache_idx; uint8_t* cache; size_t cache_cap;
    uint8_t* cbuf; size_t cbuf_cap;
} ufs;

int ufs_open(ufs* u, const char* path);                 /* 0 ok, negative on error */
int ufs_read(ufs* u, uint64_t off, uint64_t size, uint8_t* dst);   /* uncompressed stream range */
void ufs_close(ufs* u);
int lz4_decompress(const uint8_t* src, size_t sn, uint8_t* dst, size_t dn);   /* returns bytes written or -1 */
#endif
