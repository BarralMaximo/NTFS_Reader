#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ntfs.h"

static int usage(FILE *to)
{
    fputs("usage: ntfsread <image> <command> [args]\n"
          "\n"
          "commands:\n"
          "  info         print volume geometry\n"
          "  ls [PATH]    list a directory (default: /)\n"
          "  cat PATH     write a file's content to stdout;\n"
          "               PATH may be file:stream for alternate streams\n",
          to);
    return to == stderr ? 2 : 0;
}

static int cmd_info(ntfs_volume *vol)
{
    ntfs_volume_info i;
    ntfs_get_info(vol, &i);
    printf("bytes per sector    %" PRIu32 "\n", i.bytes_per_sector);
    printf("sectors per cluster %" PRIu32 "\n", i.sectors_per_cluster);
    printf("cluster size        %" PRIu32 "\n", i.cluster_size);
    printf("MFT record size     %" PRIu32 "\n", i.mft_record_size);
    printf("index block size    %" PRIu32 "\n", i.index_block_size);
    printf("total sectors       %" PRIu64 "\n", i.total_sectors);
    printf("$MFT first LCN      %" PRIu64 "\n", i.mft_lcn);
    printf("volume serial       %016" PRIx64 "\n", i.serial);
    return 0;
}

static int cmd_ls(ntfs_volume *vol, const char *path)
{
    ntfs_dirent *entries;
    size_t count;

    int err = ntfs_list(vol, path, &entries, &count);
    if (err) {
        fprintf(stderr, "ntfsread: %s: %s\n", path, ntfs_strerror(err));
        return 1;
    }
    for (size_t i = 0; i < count; i++) {
        printf("%c %10" PRIu64 "  %6" PRIu64 "  %s\n",
               entries[i].is_directory ? 'd' : '-',
               entries[i].size, entries[i].mft_record, entries[i].name);
    }
    free(entries);
    return 0;
}

static int cmd_cat(ntfs_volume *vol, const char *path)
{
    uint8_t *data;
    uint64_t size;

    int err = ntfs_read_file(vol, path, &data, &size);
    if (err) {
        fprintf(stderr, "ntfsread: %s: %s\n", path, ntfs_strerror(err));
        return 1;
    }
    size_t written = fwrite(data, 1, size, stdout);
    free(data);
    if (written != size) {
        fprintf(stderr, "ntfsread: short write to stdout\n");
        return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc >= 2 && (strcmp(argv[1], "-h") == 0 ||
                      strcmp(argv[1], "--help") == 0))
        return usage(stdout);
    if (argc < 3)
        return usage(stderr);

    const char *image = argv[1];
    const char *cmd = argv[2];

    ntfs_volume *vol;
    int err = ntfs_open(image, &vol);
    if (err) {
        fprintf(stderr, "ntfsread: %s: %s\n", image, ntfs_strerror(err));
        return 1;
    }

    int rc;
    if (strcmp(cmd, "info") == 0 && argc == 3)
        rc = cmd_info(vol);
    else if (strcmp(cmd, "ls") == 0 && argc <= 4)
        rc = cmd_ls(vol, argc == 4 ? argv[3] : "/");
    else if (strcmp(cmd, "cat") == 0 && argc == 4)
        rc = cmd_cat(vol, argv[3]);
    else
        rc = usage(stderr);

    ntfs_close(vol);
    return rc;
}
