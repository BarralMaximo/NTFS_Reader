/*
 * ntfsread — a read-only NTFS explorer built from the on-disk format.
 *
 * Public API: open a raw NTFS image, list directories, read files.
 * Functions return NTFS_OK (0) on success or a negative NTFS_ERR_* code;
 * ntfs_strerror() maps codes to messages.
 */
#ifndef NTFS_H
#define NTFS_H

#include <stddef.h>
#include <stdint.h>

enum {
    NTFS_OK = 0,
    NTFS_ERR_IO = -1, // Cannot open or read the image file
    NTFS_ERR_FORMAT = -2, // Not NTFS, or an on-disk structure is corrupt
    NTFS_ERR_NOENT = -3, // No such file or directory
    NTFS_ERR_NOTDIR = -4, // A path component is not a directory
    NTFS_ERR_ISDIR = -5, // Tried to read a directory as a file
    NTFS_ERR_NOMEM = -6, // Allocation failure
    NTFS_ERR_UNSUP = -7, // Valid NTFS, but uses a feature this reader lacks
};

const char *ntfs_strerror(int err);

typedef struct ntfs_volume ntfs_volume;

// NTFS names are at most 255 UTF-16 units; 4 UTF-8 bytes per unit is a safe upper bound for the converted form.
#define NTFS_MAX_NAME_UTF8 (255 * 4)

typedef struct {
    char name[NTFS_MAX_NAME_UTF8 + 1];
    uint64_t size; // bytes; 0 for directories
    uint64_t mft_record;
    int is_directory;
} ntfs_dirent;

typedef struct {
    uint32_t bytes_per_sector;
    uint32_t sectors_per_cluster;
    uint32_t cluster_size;
    uint32_t mft_record_size;
    uint32_t index_block_size;
    uint64_t total_sectors;
    uint64_t mft_lcn;
    uint64_t serial;
} ntfs_volume_info;

int  ntfs_open(const char *image_path, ntfs_volume **out);
void ntfs_close(ntfs_volume *vol);
void ntfs_get_info(const ntfs_volume *vol, ntfs_volume_info *out);

/* List a directory. Entries come out in NTFS index order (sorted by the volume's collation); free(*entries) when done. */
int ntfs_list(ntfs_volume *vol, const char *path, ntfs_dirent **entries, size_t *count);

/* Read a whole file into a malloc'd buffer; free(*data) when done.
 * "path:stream" addresses a named $DATA stream (an alternate data stream). */
int ntfs_read_file(ntfs_volume *vol, const char *path, uint8_t **data, uint64_t *size);

#endif
