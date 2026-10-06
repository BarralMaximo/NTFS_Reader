#include "attr.h"

#include <stdlib.h>
#include <string.h>

#include "volume.h"

/*
 * Data runs map an attribute's virtual clusters (VCNs) to logical clusters
 * on the volume (LCNs) with a variable-length encoding: each run starts
 * with a header byte whose low nibble says how many bytes encode the run
 * length and whose high nibble says how many encode the LCN offset. The
 * offset is *signed* and *relative to the previous run's LCN* — most runs
 * of a healthy file sit near each other, so one or two bytes usually
 * suffice no matter where on the disk the file lives. An offset size of
 * zero marks a sparse run: clusters that exist in the file's address
 * space but occupy no disk at all. A zero header byte terminates the list.
 */
int runlist_decode(const uint8_t *data, size_t size, struct runlist *out)
{
    struct ntfs_run *runs = NULL;
    size_t count = 0, cap = 0;
    size_t pos = 0;
    int64_t lcn = 0;

    out->runs = NULL;
    out->count = 0;

    while (pos < size && data[pos] != 0) {
        unsigned len_bytes = data[pos] & 0x0F;
        unsigned off_bytes = data[pos] >> 4;
        pos++;

        if (len_bytes == 0 || len_bytes > 8 || off_bytes > 8 ||
            len_bytes + off_bytes > size - pos)
            goto corrupt;

        uint64_t length = 0;
        for (unsigned i = 0; i < len_bytes; i++)
            length |= (uint64_t)data[pos + i] << (8 * i);
        pos += len_bytes;
        if (length == 0)
            goto corrupt;

        int sparse = (off_bytes == 0);
        if (!sparse) {
            uint64_t raw = 0;
            for (unsigned i = 0; i < off_bytes; i++)
                raw |= (uint64_t)data[pos + i] << (8 * i);
            pos += off_bytes;
            /* sign-extend from off_bytes*8 bits */
            if (off_bytes < 8 && (raw & (1ull << (8 * off_bytes - 1))))
                raw |= ~((1ull << (8 * off_bytes)) - 1);
            lcn += (int64_t)raw;
            if (lcn < 0)
                goto corrupt;
        }

        if (count == cap) {
            size_t ncap = cap ? cap * 2 : 8;
            struct ntfs_run *n = realloc(runs, ncap * sizeof(*runs));
            if (!n) {
                free(runs);
                return NTFS_ERR_NOMEM;
            }
            runs = n;
            cap = ncap;
        }
        runs[count].lcn = (uint64_t)lcn;
        runs[count].length = length;
        runs[count].sparse = sparse;
        count++;
    }

    if (pos >= size) /* ran off the end without a terminator */
        goto corrupt;

    out->runs = runs;
    out->count = count;
    return NTFS_OK;

corrupt:
    free(runs);
    return NTFS_ERR_FORMAT;
}

void runlist_free(struct runlist *rl)
{
    free(rl->runs);
    rl->runs = NULL;
    rl->count = 0;
}

int runlist_pread(const ntfs_volume *vol, const struct runlist *rl,
                  uint64_t off, uint64_t len, uint8_t *dst)
{
    uint64_t cs = vol->cluster_size;
    uint64_t skip = off;
    size_t i = 0;

    while (len > 0) {
        if (i >= rl->count) /* read past the mapped space */
            return NTFS_ERR_FORMAT;
        const struct ntfs_run *r = &rl->runs[i];
        if (r->length > UINT64_MAX / cs)
            return NTFS_ERR_FORMAT;
        uint64_t run_bytes = r->length * cs;
        if (skip >= run_bytes) {
            skip -= run_bytes;
            i++;
            continue;
        }
        uint64_t chunk = run_bytes - skip;
        if (chunk > len)
            chunk = len;
        if (r->sparse) {
            memset(dst, 0, chunk);
        } else {
            const uint8_t *src = volume_at(vol, r->lcn * cs + skip, chunk);
            if (!src)
                return NTFS_ERR_FORMAT;
            memcpy(dst, src, chunk);
        }
        dst += chunk;
        len -= chunk;
        skip = 0;
        i++;
    }
    return NTFS_OK;
}

int utf16_matches_ascii(const uint8_t *utf16, size_t units, const char *ascii)
{
    if (units != strlen(ascii))
        return 0;
    for (size_t i = 0; i < units; i++) {
        uint16_t c = (uint16_t)(utf16[2 * i] | (utf16[2 * i + 1] << 8));
        if (upcase_unit(c) != upcase_unit((uint16_t)(unsigned char)ascii[i]))
            return 0;
    }
    return 1;
}

int attr_name_matches(const struct attr_header *a, size_t limit,
                      const char *name)
{
    if (name == NULL || name[0] == '\0')
        return a->name_length == 0;
    if (a->name_length == 0)
        return 0;
    size_t name_end = (size_t)a->name_offset + 2u * a->name_length;
    if (name_end > limit || name_end > a->length)
        return 0;
    return utf16_matches_ascii((const uint8_t *)a + a->name_offset,
                               a->name_length, name);
}

int attr_resident_value(const struct attr_header *a, size_t limit,
                        const uint8_t **value, uint32_t *length)
{
    if (a->non_resident)
        return NTFS_ERR_FORMAT;
    uint32_t off = a->u.res.value_offset;
    uint32_t len = a->u.res.value_length;
    if (off < ATTR_HEADER_RESIDENT_SIZE ||
        (uint64_t)off + len > a->length || a->length > limit)
        return NTFS_ERR_FORMAT;
    *value = (const uint8_t *)a + off;
    *length = len;
    return NTFS_OK;
}

/* Anything bigger than this is either a corrupt size field or outside the
 * scope of a tool that materializes whole files in memory. */
#define ATTR_MAX_CONTENT (1ull << 31)

int attr_read_all(const ntfs_volume *vol, const struct attr_header *a,
                  size_t limit, uint8_t **data, uint64_t *size)
{
    *data = NULL;
    *size = 0;

    if (!a->non_resident) {
        const uint8_t *value;
        uint32_t len;
        int err = attr_resident_value(a, limit, &value, &len);
        if (err)
            return err;
        uint8_t *buf = malloc(len ? len : 1);
        if (!buf)
            return NTFS_ERR_NOMEM;
        memcpy(buf, value, len);
        *data = buf;
        *size = len;
        return NTFS_OK;
    }

    if (a->flags & (ATTR_FLAG_COMPRESSION_MASK | ATTR_FLAG_ENCRYPTED))
        return NTFS_ERR_UNSUP;
    /* lowest_vcn != 0 would mean this is the second or later extent of an
     * attribute split across MFT records via $ATTRIBUTE_LIST. */
    if (a->u.nonres.lowest_vcn != 0)
        return NTFS_ERR_UNSUP;

    uint32_t ro = a->u.nonres.runlist_offset;
    if (ro < ATTR_HEADER_NONRESIDENT_SIZE || ro > a->length ||
        a->length > limit)
        return NTFS_ERR_FORMAT;

    struct runlist rl;
    int err = runlist_decode((const uint8_t *)a + ro, a->length - ro, &rl);
    if (err)
        return err;

    uint64_t total = a->u.nonres.data_size;
    uint64_t init = a->u.nonres.initialized_size;
    if (init > total)
        init = total;
    if (total > ATTR_MAX_CONTENT) {
        runlist_free(&rl);
        return NTFS_ERR_UNSUP;
    }

    uint8_t *buf = malloc(total ? total : 1);
    if (!buf) {
        runlist_free(&rl);
        return NTFS_ERR_NOMEM;
    }
    err = init ? runlist_pread(vol, &rl, 0, init, buf) : NTFS_OK;
    runlist_free(&rl);
    if (err) {
        free(buf);
        return err;
    }
    memset(buf + init, 0, total - init);

    *data = buf;
    *size = total;
    return NTFS_OK;
}
