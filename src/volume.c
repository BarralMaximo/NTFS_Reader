#define _POSIX_C_SOURCE 200809L

#include "volume.h"

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

/* On-disk boot sector (jump + BPB), from the public spec. */
struct boot_sector {
    uint8_t  jump[3];
    char     oem_id[8]; /* "NTFS    " */
    uint16_t bytes_per_sector;
    uint8_t  sectors_per_cluster; /* see decode_sectors_per_cluster() */
    uint16_t reserved_sectors;    /* 0 on NTFS */
    uint8_t  zero1[3];
    uint16_t zero2;
    uint8_t  media_descriptor;
    uint16_t zero3;
    uint16_t sectors_per_track;
    uint16_t heads;
    uint32_t hidden_sectors;
    uint32_t zero4;
    uint32_t signature;
    uint64_t total_sectors;
    uint64_t mft_lcn;
    uint64_t mft_mirror_lcn;
    int8_t   clusters_per_mft_record; /* negative n encodes 2^-n bytes */
    uint8_t  pad1[3];
    int8_t   clusters_per_index_block;
    uint8_t  pad2[3];
    uint64_t serial;
    uint32_t checksum;
} __attribute__((packed));

static int is_pow2(uint64_t v)
{
    return v != 0 && (v & (v - 1)) == 0;
}

/* Values above 0x80 encode 2^(256-n) sectors, used for clusters > 64K. */
static uint64_t decode_sectors_per_cluster(uint8_t v)
{
    if (v <= 0x80)
        return v;
    unsigned shift = 256u - v;
    return shift < 32 ? (1ull << shift) : 0;
}

/* clusters_per_mft_record and clusters_per_index_block: a positive value
 * counts clusters; a negative n means the unit is smaller than a cluster
 * and its size is 2^-n bytes. */
static uint64_t decode_record_size(int8_t v, uint64_t cluster_size)
{
    if (v > 0)
        return (uint64_t)v * cluster_size;
    if (v >= -31 && v < 0)
        return 1ull << -v;
    return 0;
}

const uint8_t *volume_at(const ntfs_volume *vol, uint64_t off, uint64_t len)
{
    if (len > vol->image_size || off > vol->image_size - len)
        return NULL;
    return vol->image + off;
}

int volume_open(const char *path, ntfs_volume **out)
{
    *out = NULL;

    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return NTFS_ERR_IO;

    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size < (off_t)sizeof(struct boot_sector)) {
        close(fd);
        return NTFS_ERR_IO;
    }

    void *image = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (image == MAP_FAILED)
        return NTFS_ERR_IO;

    ntfs_volume *vol = calloc(1, sizeof(*vol));
    if (!vol) {
        munmap(image, (size_t)st.st_size);
        return NTFS_ERR_NOMEM;
    }
    vol->image = image;
    vol->image_size = (size_t)st.st_size;

    const struct boot_sector *bs = (const void *)vol->image;

    /* Filesystem detection: NTFS pads the OEM ID to exactly this string. */
    if (memcmp(bs->oem_id, "NTFS    ", 8) != 0)
        goto bad_format;

    uint32_t bps = bs->bytes_per_sector;
    uint64_t spc = decode_sectors_per_cluster(bs->sectors_per_cluster);
    if (bps < 256 || bps > 4096 || !is_pow2(bps))
        goto bad_format;
    if (spc == 0 || !is_pow2(spc))
        goto bad_format;

    uint64_t cluster_size = (uint64_t)bps * spc;
    if (cluster_size > (1u << 21)) /* 2 MiB, the format's ceiling */
        goto bad_format;

    uint64_t mft_record_size = decode_record_size(bs->clusters_per_mft_record,
                                                  cluster_size);
    uint64_t index_block_size = decode_record_size(bs->clusters_per_index_block,
                                                   cluster_size);
    if (!is_pow2(mft_record_size) ||
        mft_record_size < 256 || mft_record_size > (1u << 16))
        goto bad_format;
    if (!is_pow2(index_block_size) ||
        index_block_size < 512 || index_block_size > (1u << 24))
        goto bad_format;

    if (bs->total_sectors == 0 ||
        bs->mft_lcn >= vol->image_size / cluster_size)
        goto bad_format;

    vol->bytes_per_sector = bps;
    vol->sectors_per_cluster = (uint32_t)spc;
    vol->cluster_size = (uint32_t)cluster_size;
    vol->mft_record_size = (uint32_t)mft_record_size;
    vol->index_block_size = (uint32_t)index_block_size;
    vol->total_sectors = bs->total_sectors;
    vol->mft_lcn = bs->mft_lcn;
    vol->serial = bs->serial;

    *out = vol;
    return NTFS_OK;

bad_format:
    volume_close(vol);
    return NTFS_ERR_FORMAT;
}

void volume_close(ntfs_volume *vol)
{
    if (!vol)
        return;
    runlist_free(&vol->mft_runs);
    if (vol->image)
        munmap(vol->image, vol->image_size);
    free(vol);
}
