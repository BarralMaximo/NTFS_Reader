/*
 * The Master File Table: reading records (with fixups), walking their
 * attributes, and following $ATTRIBUTE_LIST indirection when a file's
 * attributes overflow into extension records.
 */
#ifndef MFT_H
#define MFT_H

#include <stddef.h>
#include <stdint.h>

#include "attr.h"
#include "ntfs.h"
#include "volume.h"

/* Spec-defined record numbers. */
#define MFT_REC_MFT  0u
#define MFT_REC_ROOT 5u

/* mft_record_header.flags */
#define MFT_RECORD_IN_USE    0x0001u
#define MFT_RECORD_DIRECTORY 0x0002u

struct mft_record_header {
    char     signature[4]; /* "FILE" */
    uint16_t usa_offset;
    uint16_t usa_count;
    uint64_t lsn;
    uint16_t sequence;
    uint16_t hard_link_count;
    uint16_t attrs_offset;
    uint16_t flags;
    uint32_t bytes_in_use;
    uint32_t bytes_allocated;
    uint64_t base_record;
    uint16_t next_attr_id;
} __attribute__((packed));

/* File references pack a 48-bit record number with a 16-bit sequence. */
static inline uint64_t mft_ref_record(uint64_t ref)
{
    return ref & 0xFFFFFFFFFFFFull;
}

/* Bootstrap: locate record 0 via the boot sector, decode the $MFT's own
 * $DATA runlist out of it, and store it in the volume so every subsequent
 * record read can be mapped through it. */
int mft_init(ntfs_volume *vol);

/* Read record `recno` into a caller buffer of vol->mft_record_size bytes,
 * verify the FILE signature, apply fixups, sanity-check the header. */
int mft_read_record(const ntfs_volume *vol, uint64_t recno, uint8_t *buf);

/* Iterate the attributes of a record: start with *cursor = NULL; returns
 * NTFS_ERR_NOENT at the end marker. Every header handed out has been
 * bounds-checked against the record. */
int mft_next_attr(const ntfs_volume *vol, const uint8_t *record,
                  const struct attr_header **cursor);

/* Bytes available from attribute `a` to the end of the record's used area
 * (valid for cursors produced by mft_next_attr). */
static inline size_t mft_attr_limit(const uint8_t *record,
                                    const struct attr_header *a)
{
    const struct mft_record_header *h = (const void *)record;
    return h->bytes_in_use - (size_t)((const uint8_t *)a - record);
}

/* Find the first attribute of `type` whose name matches (NULL = unnamed).
 * Searches `record`, then follows an eventual $ATTRIBUTE_LIST into
 * extension records. On success *out points into `record` or into
 * *ext_record, a malloc'd buffer the caller must free (NULL if unused). */
int mft_find_attr(const ntfs_volume *vol, const uint8_t *record,
                  uint64_t recno, uint32_t type, const char *name,
                  const struct attr_header **out, uint8_t **ext_record);

#endif /* MFT_H */
