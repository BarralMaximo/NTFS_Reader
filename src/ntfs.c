/*
 * Public API: glue between path strings and the layers below.
 */
#include "ntfs.h"

#include <stdlib.h>
#include <string.h>

#include "file.h"
#include "index.h"
#include "mft.h"
#include "volume.h"

const char *ntfs_strerror(int err)
{
    switch (err) {
    case NTFS_OK:         return "success";
    case NTFS_ERR_IO:     return "cannot read image file";
    case NTFS_ERR_FORMAT: return "not an NTFS volume or corrupt structure";
    case NTFS_ERR_NOENT:  return "no such file or directory";
    case NTFS_ERR_NOTDIR: return "not a directory";
    case NTFS_ERR_ISDIR:  return "is a directory";
    case NTFS_ERR_NOMEM:  return "out of memory";
    case NTFS_ERR_UNSUP:  return "unsupported NTFS feature";
    default:              return "unknown error";
    }
}

int ntfs_open(const char *image_path, ntfs_volume **out)
{
    int err = volume_open(image_path, out);
    if (err)
        return err;
    err = mft_init(*out);
    if (err) {
        volume_close(*out);
        *out = NULL;
    }
    return err;
}

void ntfs_close(ntfs_volume *vol)
{
    volume_close(vol);
}

void ntfs_get_info(const ntfs_volume *vol, ntfs_volume_info *out)
{
    out->bytes_per_sector = vol->bytes_per_sector;
    out->sectors_per_cluster = vol->sectors_per_cluster;
    out->cluster_size = vol->cluster_size;
    out->mft_record_size = vol->mft_record_size;
    out->index_block_size = vol->index_block_size;
    out->total_sectors = vol->total_sectors;
    out->mft_lcn = vol->mft_lcn;
    out->serial = vol->serial;
}

/*
 * Resolve `path` to an MFT record number, walking one directory index per
 * component. A trailing ":stream" on the last component names an alternate
 * $DATA stream and is handed back through *stream (pointing into `path`).
 */
static int resolve_path(ntfs_volume *vol, const char *path,
                        const char **stream, uint64_t *out_recno)
{
    uint8_t *rec = malloc(vol->mft_record_size);
    if (!rec)
        return NTFS_ERR_NOMEM;

    uint64_t cur = MFT_REC_ROOT;
    const char *p = path;
    int err = NTFS_OK;

    *stream = NULL;

    while (*p) {
        while (*p == '/')
            p++;
        if (!*p)
            break;
        const char *end = strchr(p, '/');
        if (!end)
            end = p + strlen(p);
        size_t clen = (size_t)(end - p);

        /* "name:stream" is only meaningful on the final component. */
        const char *colon = memchr(p, ':', clen);
        if (colon) {
            if (*end != '\0') {
                err = NTFS_ERR_NOENT;
                break;
            }
            *stream = colon + 1;
            clen = (size_t)(colon - p);
        }

        if (clen == 1 && p[0] == '.') {
            p = end;
            continue;
        }

        uint16_t name16[255];
        size_t units;
        err = utf8_to_utf16(p, clen, name16, 255, &units);
        if (err)
            break;
        if (units == 0) {
            err = NTFS_ERR_NOENT;
            break;
        }

        err = mft_read_record(vol, cur, rec);
        if (err)
            break;
        const struct mft_record_header *h = (const void *)rec;
        if (!(h->flags & MFT_RECORD_DIRECTORY)) {
            err = NTFS_ERR_NOTDIR;
            break;
        }

        uint64_t ref;
        uint32_t fattrs;
        err = index_lookup(vol, rec, cur, name16, units, &ref, &fattrs);
        if (err)
            break;
        cur = mft_ref_record(ref);
        p = end;
    }

    free(rec);
    if (err == NTFS_OK)
        *out_recno = cur;
    return err;
}

struct collect {
    ntfs_dirent *v;
    size_t       count, cap;
};

static int collect_visit(void *ctx, const struct file_name_key *key,
                         const uint8_t *name_utf16, uint64_t mft_ref)
{
    struct collect *c = ctx;

    /* DOS-namespace entries are 8.3 aliases of names already listed under
     * their Win32 form; emitting both would show phantom duplicates. */
    if (key->name_space == FN_NAMESPACE_DOS)
        return 0;

    if (c->count == c->cap) {
        size_t ncap = c->cap ? c->cap * 2 : 32;
        ntfs_dirent *n = realloc(c->v, ncap * sizeof(*n));
        if (!n)
            return NTFS_ERR_NOMEM;
        c->v = n;
        c->cap = ncap;
    }

    ntfs_dirent *d = &c->v[c->count++];
    utf16le_to_utf8(name_utf16, key->name_units, d->name, sizeof(d->name));
    d->is_directory = (key->file_attributes & FN_ATTR_DIRECTORY) != 0;
    d->size = d->is_directory ? 0 : key->data_size;
    d->mft_record = mft_ref_record(mft_ref);
    return 0;
}

int ntfs_list(ntfs_volume *vol, const char *path,
              ntfs_dirent **entries, size_t *count)
{
    const char *stream;
    uint64_t recno;

    *entries = NULL;
    *count = 0;

    int err = resolve_path(vol, path, &stream, &recno);
    if (err)
        return err;
    if (stream)
        return NTFS_ERR_NOTDIR;

    uint8_t *rec = malloc(vol->mft_record_size);
    if (!rec)
        return NTFS_ERR_NOMEM;
    err = mft_read_record(vol, recno, rec);
    if (err == NTFS_OK) {
        const struct mft_record_header *h = (const void *)rec;
        if (!(h->flags & MFT_RECORD_DIRECTORY))
            err = NTFS_ERR_NOTDIR;
    }

    struct collect c = {0};
    if (err == NTFS_OK)
        err = index_enumerate(vol, rec, recno, collect_visit, &c);
    free(rec);

    if (err) {
        free(c.v);
        return err;
    }
    *entries = c.v;
    *count = c.count;
    return NTFS_OK;
}

int ntfs_read_file(ntfs_volume *vol, const char *path,
                   uint8_t **data, uint64_t *size)
{
    const char *stream;
    uint64_t recno;

    *data = NULL;
    *size = 0;

    int err = resolve_path(vol, path, &stream, &recno);
    if (err)
        return err;

    uint8_t *rec = malloc(vol->mft_record_size);
    if (!rec)
        return NTFS_ERR_NOMEM;
    err = mft_read_record(vol, recno, rec);
    if (err == NTFS_OK) {
        const struct mft_record_header *h = (const void *)rec;
        /* Directories have no unnamed $DATA, but a named stream on a
         * directory is legal and readable. */
        if ((h->flags & MFT_RECORD_DIRECTORY) && !stream)
            err = NTFS_ERR_ISDIR;
    }
    if (err == NTFS_OK)
        err = file_read(vol, rec, recno, stream, data, size);
    free(rec);
    return err;
}
