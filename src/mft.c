#include "mft.h"

#include <stdlib.h>
#include <string.h>

#include "fixup.h"

/*
 * The MFT is a file that describes every file on the volume — including
 * itself, as record 0. That is circular: reading any record means mapping
 * through the $MFT's data runs, which live in an attribute of record 0,
 * which we cannot read yet. The boot sector breaks the cycle by storing
 * the LCN of the MFT's first extent; record 0 is right there, and its
 * unnamed $DATA runlist then covers every record read after it.
 */
int mft_init(ntfs_volume *vol)
{
    uint32_t rs = vol->mft_record_size;
    const uint8_t *src =
        volume_at(vol, vol->mft_lcn * vol->cluster_size, rs);
    if (!src)
        return NTFS_ERR_FORMAT;

    uint8_t *rec = malloc(rs);
    if (!rec)
        return NTFS_ERR_NOMEM;
    memcpy(rec, src, rs);

    int err = NTFS_ERR_FORMAT;
    if (memcmp(rec, "FILE", 4) != 0)
        goto out;
    if (fixup_apply(rec, rs) != NTFS_OK)
        goto out;

    const struct mft_record_header *h = (const void *)rec;
    if (h->bytes_in_use > rs || h->attrs_offset >= h->bytes_in_use)
        goto out;

    const struct attr_header *a = NULL;
    while (mft_next_attr(vol, rec, &a) == NTFS_OK) {
        if (a->type != ATTR_TYPE_DATA ||
            !attr_name_matches(a, mft_attr_limit(rec, a), NULL))
            continue;
        if (!a->non_resident) /* the MFT itself is never resident */
            goto out;
        uint32_t ro = a->u.nonres.runlist_offset;
        if (ro < ATTR_HEADER_NONRESIDENT_SIZE || ro > a->length)
            goto out;
        err = runlist_decode((const uint8_t *)a + ro, a->length - ro,
                             &vol->mft_runs);
        if (err == NTFS_OK)
            vol->mft_size = a->u.nonres.data_size;
        goto out;
    }
    /* no unnamed $DATA in record 0 */

out:
    free(rec);
    return err;
}

int mft_read_record(const ntfs_volume *vol, uint64_t recno, uint8_t *buf)
{
    uint32_t rs = vol->mft_record_size;
    if (recno > (vol->mft_size - rs) / rs || vol->mft_size < rs)
        return NTFS_ERR_NOENT;

    int err = runlist_pread(vol, &vol->mft_runs, recno * rs, rs, buf);
    if (err)
        return err;

    if (memcmp(buf, "FILE", 4) != 0)
        return NTFS_ERR_FORMAT;
    err = fixup_apply(buf, rs);
    if (err)
        return err;

    const struct mft_record_header *h = (const void *)buf;
    if (!(h->flags & MFT_RECORD_IN_USE))
        return NTFS_ERR_NOENT;
    if (h->bytes_in_use > rs ||
        h->attrs_offset < sizeof(struct mft_record_header) ||
        h->attrs_offset >= h->bytes_in_use)
        return NTFS_ERR_FORMAT;
    return NTFS_OK;
}

int mft_next_attr(const ntfs_volume *vol, const uint8_t *record,
                  const struct attr_header **cursor)
{
    const struct mft_record_header *h = (const void *)record;
    uint32_t limit = h->bytes_in_use;
    if (limit > vol->mft_record_size)
        return NTFS_ERR_FORMAT;

    uint32_t off;
    if (*cursor == NULL) {
        off = h->attrs_offset;
    } else {
        const struct attr_header *prev = *cursor;
        off = (uint32_t)((const uint8_t *)prev - record) + prev->length;
    }

    /* The end marker is a bare 4-byte type code, so check for it before
     * requiring room for a whole header. */
    if (off < sizeof(struct mft_record_header) || off + 4 > limit)
        return NTFS_ERR_FORMAT;
    const struct attr_header *a = (const void *)(record + off);
    if (a->type == ATTR_TYPE_END)
        return NTFS_ERR_NOENT;
    if (off + 16 > limit)
        return NTFS_ERR_FORMAT;

    uint32_t min = a->non_resident ? ATTR_HEADER_NONRESIDENT_SIZE
                                   : ATTR_HEADER_RESIDENT_SIZE;
    if (a->length < min || a->length > limit - off)
        return NTFS_ERR_FORMAT;

    *cursor = a;
    return NTFS_OK;
}

/* One $ATTRIBUTE_LIST entry; the list is just these back to back. */
struct attr_list_entry {
    uint32_t type;
    uint16_t length;
    uint8_t  name_units;
    uint8_t  name_offset;
    uint64_t lowest_vcn;
    uint64_t mft_ref;
    uint16_t attr_id;
} __attribute__((packed));

static int find_in_record(const ntfs_volume *vol, const uint8_t *record,
                          uint32_t type, const char *name,
                          const struct attr_header **out)
{
    const struct attr_header *a = NULL;
    int err;
    while ((err = mft_next_attr(vol, record, &a)) == NTFS_OK) {
        if (a->type == type &&
            attr_name_matches(a, mft_attr_limit(record, a), name)) {
            *out = a;
            return NTFS_OK;
        }
    }
    return err == NTFS_ERR_NOENT ? NTFS_ERR_NOENT : err;
}

int mft_find_attr(const ntfs_volume *vol, const uint8_t *record,
                  uint64_t recno, uint32_t type, const char *name,
                  const struct attr_header **out, uint8_t **ext_record)
{
    *ext_record = NULL;

    int err = find_in_record(vol, record, type, name, out);
    if (err != NTFS_ERR_NOENT)
        return err;

    /* Not in the base record. When a file has more attributes than one
     * record can hold, the base record keeps an $ATTRIBUTE_LIST naming
     * which record each attribute actually lives in. */
    const struct attr_header *la;
    err = find_in_record(vol, record, ATTR_TYPE_ATTRIBUTE_LIST, NULL, &la);
    if (err)
        return err == NTFS_ERR_NOENT ? NTFS_ERR_NOENT : err;

    uint8_t *list;
    uint64_t list_size;
    err = attr_read_all(vol, la, mft_attr_limit(record, la),
                        &list, &list_size);
    if (err)
        return err;

    uint8_t *ext = malloc(vol->mft_record_size);
    if (!ext) {
        free(list);
        return NTFS_ERR_NOMEM;
    }

    err = NTFS_ERR_NOENT;
    uint64_t pos = 0;
    while (pos + sizeof(struct attr_list_entry) <= list_size) {
        const struct attr_list_entry *e = (const void *)(list + pos);
        if (e->length < sizeof(struct attr_list_entry) ||
            e->length > list_size - pos) {
            err = NTFS_ERR_FORMAT;
            break;
        }
        int name_ok;
        if (name == NULL || name[0] == '\0') {
            name_ok = (e->name_units == 0);
        } else if (e->name_units == 0 ||
                   (size_t)e->name_offset + 2u * e->name_units > e->length) {
            name_ok = 0;
        } else {
            name_ok = utf16_matches_ascii(list + pos + e->name_offset,
                                          e->name_units, name);
        }
        uint64_t target = mft_ref_record(e->mft_ref);
        /* lowest_vcn == 0 selects the extent that owns the attribute's
         * header (and, for non-resident ones, the start of its runlist);
         * later extents of a split attribute are not supported. */
        if (e->type == type && name_ok && e->lowest_vcn == 0 &&
            target != recno) {
            err = mft_read_record(vol, target, ext);
            if (err == NTFS_OK)
                err = find_in_record(vol, ext, type, name, out);
            if (err != NTFS_ERR_NOENT)
                break;
            err = NTFS_ERR_NOENT;
        }
        pos += e->length;
    }

    free(list);
    if (err == NTFS_OK)
        *ext_record = ext;
    else
        free(ext);
    return err;
}
