/*
 * Layer 2: attribute resolution.
 *
 * Owns the resident/non-resident split and the variable-length data run
 * ("runlist") encoding. Deliberately agnostic to attribute *type*: this
 * layer hands back bytes, the layers above give them meaning.
 *
 * On-disk layouts follow the public ATTRIBUTE_RECORD_HEADER spec
 * (learn.microsoft.com, "Master File Table") and the Linux-NTFS
 * documentation project.
 */
#ifndef ATTR_H
#define ATTR_H

#include <stddef.h>
#include <stdint.h>

#include "ntfs.h"

/* Attribute type codes (the subset this reader interprets). */
#define ATTR_TYPE_STANDARD_INFORMATION 0x10u
#define ATTR_TYPE_ATTRIBUTE_LIST       0x20u
#define ATTR_TYPE_FILE_NAME            0x30u
#define ATTR_TYPE_DATA                 0x80u
#define ATTR_TYPE_INDEX_ROOT           0x90u
#define ATTR_TYPE_INDEX_ALLOCATION     0xA0u
#define ATTR_TYPE_END                  0xFFFFFFFFu

/* attr_header.flags */
#define ATTR_FLAG_COMPRESSION_MASK 0x00FFu
#define ATTR_FLAG_ENCRYPTED        0x4000u
#define ATTR_FLAG_SPARSE           0x8000u

struct attr_header {
    uint32_t type;
    uint32_t length;      /* whole attribute record, this header included */
    uint8_t  non_resident;
    uint8_t  name_length; /* UTF-16 units */
    uint16_t name_offset; /* from the start of this header */
    uint16_t flags;
    uint16_t attr_id;
    union {
        struct {
            uint32_t value_length;
            uint16_t value_offset; /* from the start of this header */
            uint8_t  indexed;
            uint8_t  pad;
        } res;
        struct {
            uint64_t lowest_vcn;
            uint64_t highest_vcn;
            uint16_t runlist_offset; /* from the start of this header */
            uint16_t compression_unit;
            uint8_t  pad[4];
            uint64_t allocated_size;
            uint64_t data_size;        /* the size the user sees */
            uint64_t initialized_size; /* bytes written; the rest reads 0 */
        } nonres;
    } u;
} __attribute__((packed));

#define ATTR_HEADER_RESIDENT_SIZE    24u
#define ATTR_HEADER_NONRESIDENT_SIZE 64u

struct ntfs_run {
    uint64_t lcn;    /* absolute first cluster; meaningless when sparse */
    uint64_t length; /* clusters */
    int      sparse; /* no clusters on disk, content reads as zeros */
};

struct runlist {
    struct ntfs_run *runs;
    size_t           count;
};

int  runlist_decode(const uint8_t *data, size_t size, struct runlist *out);
void runlist_free(struct runlist *rl);

/* Copy `len` bytes starting at byte `off` of the space the runlist maps.
 * Sparse runs read as zeros. */
int runlist_pread(const ntfs_volume *vol, const struct runlist *rl,
                  uint64_t off, uint64_t len, uint8_t *dst);

/* Compare a UTF-16LE name (given as raw bytes) against an ASCII string,
 * case-insensitively over the ASCII range. */
int utf16_matches_ascii(const uint8_t *utf16, size_t units, const char *ascii);

/* True if the attribute's name matches `name` (NULL means unnamed).
 * `limit` is the number of bytes available from `a` onwards. */
int attr_name_matches(const struct attr_header *a, size_t limit,
                      const char *name);

/* Resident value access, bounds-checked. */
int attr_resident_value(const struct attr_header *a, size_t limit,
                        const uint8_t **value, uint32_t *length);

/* Materialize an attribute's whole content, whichever form it is in, into
 * a malloc'd buffer. Sparse runs come back zero-filled and bytes past
 * initialized_size are zeroed, matching what the driver would return. */
int attr_read_all(const ntfs_volume *vol, const struct attr_header *a,
                  size_t limit, uint8_t **data, uint64_t *size);

/* ASCII-range approximation of the volume's $UpCase table: exact for the
 * names mkfs.ntfs and Windows generate from ASCII; other code points
 * compare by value. */
static inline uint16_t upcase_unit(uint16_t c)
{
    return (c >= 'a' && c <= 'z') ? (uint16_t)(c - 'a' + 'A') : c;
}

#endif /* ATTR_H */
