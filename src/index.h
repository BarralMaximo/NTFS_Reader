/*
 * Layer 3: directory semantics.
 *
 * A directory is a file whose content is a B+ tree of file names, kept in
 * the "$I30" index: $INDEX_ROOT (always resident, holds the top node) and,
 * once the directory outgrows the MFT record, $INDEX_ALLOCATION
 * (non-resident index blocks holding the rest of the tree).
 *
 * Lookup descends the tree using the volume's collation; enumeration is an
 * in-order traversal, so entries come out sorted for free.
 */
#ifndef INDEX_H
#define INDEX_H

#include <stddef.h>
#include <stdint.h>

#include "ntfs.h"

struct index_root {
    uint32_t indexed_attr_type; /* $FILE_NAME for directory indexes */
    uint32_t collation_rule;
    uint32_t block_size;        /* bytes per $INDEX_ALLOCATION block */
    uint8_t  clusters_per_block;
    uint8_t  pad[3];
    /* an index_node follows */
} __attribute__((packed));

/* Node header; lives after index_root and inside every index block.
 * Both offsets are relative to the start of this header. */
struct index_node {
    uint32_t entries_offset;
    uint32_t used_size;
    uint32_t allocated_size;
    uint8_t  flags; /* 1 = this node has subnodes in $INDEX_ALLOCATION */
    uint8_t  pad[3];
} __attribute__((packed));

/* One $INDEX_ALLOCATION block ("INDX" record, fixup-protected). */
struct index_block {
    char     signature[4]; /* "INDX" */
    uint16_t usa_offset;
    uint16_t usa_count;
    uint64_t lsn;
    uint64_t vcn; /* which block of the index this is */
    struct index_node node;
} __attribute__((packed));

/* index_entry.flags */
#define INDEX_ENTRY_NODE 0x01u /* last 8 bytes hold the subnode's VCN */
#define INDEX_ENTRY_END  0x02u /* sentinel entry closing a node; no key */

struct index_entry {
    uint64_t file_ref;
    uint16_t length;
    uint16_t key_length;
    uint16_t flags;
    uint16_t pad;
    /* key_length bytes of key (a $FILE_NAME value) follow */
} __attribute__((packed));

/* The $FILE_NAME attribute value, used as the index key. */
struct file_name_key {
    uint64_t parent_ref;
    uint64_t creation_time;
    uint64_t modification_time;
    uint64_t mft_change_time;
    uint64_t access_time;
    uint64_t allocated_size;
    uint64_t data_size;
    uint32_t file_attributes;
    uint32_t reparse_tag;
    uint8_t  name_units; /* UTF-16 units */
    uint8_t  name_space;
    /* UTF-16LE name follows */
} __attribute__((packed));

#define FN_NAMESPACE_DOS  2u          /* 8.3 alias entries */
#define FN_ATTR_DIRECTORY 0x10000000u /* mirrors the MFT record's flag */

/* Visitor for enumeration; return non-zero to abort with that value. */
typedef int (*index_visit_fn)(void *ctx, const struct file_name_key *key,
                              const uint8_t *name_utf16, uint64_t mft_ref);

/* `dir_record` is the directory's MFT record (already read and fixed up);
 * `dir_recno` its number, needed to follow $ATTRIBUTE_LIST. */
int index_enumerate(const ntfs_volume *vol, const uint8_t *dir_record,
                    uint64_t dir_recno, index_visit_fn visit, void *ctx);

int index_lookup(const ntfs_volume *vol, const uint8_t *dir_record,
                 uint64_t dir_recno, const uint16_t *name, size_t name_units,
                 uint64_t *out_ref, uint32_t *out_file_attrs);

/* Name conversions (UTF-16LE names as raw bytes; handles surrogate pairs,
 * replacing invalid sequences with U+FFFD). Returns bytes written. */
size_t utf16le_to_utf8(const uint8_t *utf16, size_t units,
                       char *out, size_t cap);

/* Returns NTFS_OK and the unit count, or NTFS_ERR_NOENT for byte sequences
 * that cannot name an NTFS file (malformed UTF-8, too long). */
int utf8_to_utf16(const char *s, size_t len,
                  uint16_t *out, size_t cap, size_t *units);

#endif /* INDEX_H */
