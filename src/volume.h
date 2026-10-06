/*
 * Layer 1: raw volume access.
 *
 * Boot sector parsing (including filesystem detection via the OEM ID) and
 * bounds-checked reads from the memory-mapped image. The image is treated
 * as untrusted input: every pointer the upper layers touch comes out of
 * volume_at(), which is the single bounds check everything relies on.
 */
#ifndef VOLUME_H
#define VOLUME_H

#include <stddef.h>
#include <stdint.h>

#include "attr.h"
#include "ntfs.h"

struct ntfs_volume {
    uint8_t *image; /* whole image, mmap'd read-only */
    size_t   image_size;

    uint32_t bytes_per_sector;
    uint32_t sectors_per_cluster;
    uint32_t cluster_size;
    uint32_t mft_record_size;
    uint32_t index_block_size;
    uint64_t total_sectors;
    uint64_t mft_lcn;
    uint64_t serial;

    /* Where $MFT's content lives, recovered by mft_init() from record 0
     * itself (see mft.c for the bootstrap). */
    struct runlist mft_runs;
    uint64_t       mft_size; /* bytes of $MFT $DATA */
};

int  volume_open(const char *path, ntfs_volume **out);
void volume_close(ntfs_volume *vol);

/* Pointer into the image iff [off, off+len) lies inside it, else NULL. */
const uint8_t *volume_at(const ntfs_volume *vol, uint64_t off, uint64_t len);

#endif /* VOLUME_H */
