#include "index.h"

#include <stdlib.h>
#include <string.h>

#include "attr.h"
#include "fixup.h"
#include "mft.h"
#include "volume.h"

/* Corrupt images could link index blocks into a cycle; a legitimate
 * directory tree is nowhere near this deep. */
#define INDEX_MAX_DEPTH 64

struct index_walk {
    const ntfs_volume *vol;

    uint32_t block_size;
    uint64_t vcn_unit;   /* bytes per VCN step inside `alloc` */
    uint8_t *alloc;      /* $INDEX_ALLOCATION content, or NULL */
    uint64_t alloc_size;
    uint8_t *fixed;      /* per-block "fixups already applied" flags */

    /* lookup mode (target != NULL) */
    const uint16_t *target;
    size_t          target_units;
    uint64_t        found_ref;
    uint32_t        found_attrs;
    int             found;

    /* enumerate mode */
    index_visit_fn visit;
    void          *ctx;

    int depth;
};

/* NTFS orders directory entries by upcased UTF-16 units (the volume's
 * $UpCase table); upcase_unit() is our documented ASCII approximation. */
static int name_collate(const uint16_t *a, size_t a_units,
                        const uint8_t *b_le, size_t b_units)
{
    size_t n = a_units < b_units ? a_units : b_units;
    for (size_t i = 0; i < n; i++) {
        uint16_t ca = upcase_unit(a[i]);
        uint16_t cb = upcase_unit(
            (uint16_t)(b_le[2 * i] | (b_le[2 * i + 1] << 8)));
        if (ca != cb)
            return ca < cb ? -1 : 1;
    }
    if (a_units == b_units)
        return 0;
    return a_units < b_units ? -1 : 1;
}

static int walk_block(struct index_walk *w, uint64_t vcn);

/* Process one node (the root's, or one block's). `base` points at its
 * index_node header, `avail` is how many bytes exist from there. */
static int walk_node(struct index_walk *w, const uint8_t *base, size_t avail)
{
    if (++w->depth > INDEX_MAX_DEPTH)
        return NTFS_ERR_FORMAT;

    struct index_node node;
    if (avail < sizeof(node)) {
        w->depth--;
        return NTFS_ERR_FORMAT;
    }
    memcpy(&node, base, sizeof(node));
    if (node.entries_offset < sizeof(node) || node.used_size > avail ||
        node.entries_offset > node.used_size) {
        w->depth--;
        return NTFS_ERR_FORMAT;
    }

    int err = NTFS_OK;
    size_t pos = node.entries_offset;
    while (pos + sizeof(struct index_entry) <= node.used_size) {
        const struct index_entry *e = (const void *)(base + pos);
        uint16_t elen = e->length;
        if (elen < sizeof(struct index_entry) ||
            elen > node.used_size - pos) {
            err = NTFS_ERR_FORMAT;
            break;
        }

        int last = e->flags & INDEX_ENTRY_END;
        int has_sub = e->flags & INDEX_ENTRY_NODE;
        uint64_t sub_vcn = 0;
        if (has_sub) {
            if (elen < sizeof(struct index_entry) + 8) {
                err = NTFS_ERR_FORMAT;
                break;
            }
            memcpy(&sub_vcn, base + pos + elen - 8, 8);
        }

        const struct file_name_key *key = NULL;
        const uint8_t *name = NULL;
        if (!last) {
            size_t key_off = sizeof(struct index_entry);
            if (e->key_length < sizeof(struct file_name_key) ||
                e->key_length > elen - key_off) {
                err = NTFS_ERR_FORMAT;
                break;
            }
            key = (const void *)(base + pos + key_off);
            if (sizeof(struct file_name_key) + 2u * key->name_units >
                e->key_length) {
                err = NTFS_ERR_FORMAT;
                break;
            }
            name = base + pos + key_off + sizeof(struct file_name_key);
        }

        if (w->target) {
            /* B+ descent: entries in a node are sorted, and the subtree
             * hanging off an entry holds only names that collate strictly
             * before it. One comparison per entry decides: match, descend,
             * or keep scanning. */
            int cmp = last ? -1
                           : name_collate(w->target, w->target_units,
                                          name, key->name_units);
            if (!last && cmp == 0) {
                w->found = 1;
                w->found_ref = e->file_ref;
                w->found_attrs = key->file_attributes;
                break;
            }
            if (cmp < 0) {
                if (has_sub)
                    err = walk_block(w, sub_vcn);
                break; /* not in this node; either found below or absent */
            }
        } else {
            /* In-order traversal: subtree first, then the entry itself,
             * which is what makes the enumeration come out sorted. */
            if (has_sub) {
                err = walk_block(w, sub_vcn);
                if (err)
                    break;
            }
            if (!last) {
                err = w->visit(w->ctx, key, name, e->file_ref);
                if (err)
                    break;
            }
        }

        if (last)
            break;
        pos += elen;
    }

    w->depth--;
    return err;
}

static int walk_block(struct index_walk *w, uint64_t vcn)
{
    if (!w->alloc)
        return NTFS_ERR_FORMAT;
    if (vcn > w->alloc_size / w->vcn_unit)
        return NTFS_ERR_FORMAT;
    uint64_t off = vcn * w->vcn_unit;
    if (off > w->alloc_size || w->block_size > w->alloc_size - off)
        return NTFS_ERR_FORMAT;

    uint8_t *blk = w->alloc + off;
    size_t bidx = off / w->block_size;
    if (!w->fixed[bidx]) {
        if (memcmp(blk, "INDX", 4) != 0)
            return NTFS_ERR_FORMAT;
        int err = fixup_apply(blk, w->block_size);
        if (err)
            return err;
        w->fixed[bidx] = 1;
    }

    size_t node_off = offsetof(struct index_block, node);
    return walk_node(w, blk + node_off, w->block_size - node_off);
}

/* Set up the walk from the directory's $INDEX_ROOT (and, for large
 * directories, its $INDEX_ALLOCATION), then run it. */
static int index_walk_run(struct index_walk *w, const ntfs_volume *vol,
                          const uint8_t *dir_record, uint64_t dir_recno)
{
    const struct attr_header *a;
    uint8_t *ext_root = NULL, *ext_alloc = NULL;
    int err;

    w->vol = vol;

    err = mft_find_attr(vol, dir_record, dir_recno, ATTR_TYPE_INDEX_ROOT,
                        "$I30", &a, &ext_root);
    if (err)
        return err == NTFS_ERR_NOENT ? NTFS_ERR_NOTDIR : err;

    const uint8_t *value;
    uint32_t value_len;
    err = attr_resident_value(a, ext_root ? vol->mft_record_size
                                          : mft_attr_limit(dir_record, a),
                              &value, &value_len);
    if (err)
        goto out;

    struct index_root root;
    if (value_len < sizeof(root) + sizeof(struct index_node)) {
        err = NTFS_ERR_FORMAT;
        goto out;
    }
    memcpy(&root, value, sizeof(root));

    const uint8_t *root_node = value + sizeof(root);
    size_t root_avail = value_len - sizeof(root);

    struct index_node nh;
    memcpy(&nh, root_node, sizeof(nh));
    if (nh.flags & 0x01) {
        /* The tree spilled out of the MFT record: fetch every index block
         * up front and fix each one up lazily as the walk first visits it. */
        if (root.block_size < 512 || root.block_size > (1u << 24) ||
            (root.block_size & (root.block_size - 1)) != 0) {
            err = NTFS_ERR_FORMAT;
            goto out;
        }
        w->block_size = root.block_size;
        /* VCNs in an index count clusters, unless blocks are smaller than
         * one cluster — then they count 512-byte units. */
        w->vcn_unit = vol->cluster_size <= root.block_size
                          ? vol->cluster_size
                          : 512;

        const struct attr_header *aa;
        err = mft_find_attr(vol, dir_record, dir_recno,
                            ATTR_TYPE_INDEX_ALLOCATION, "$I30",
                            &aa, &ext_alloc);
        if (err) {
            if (err == NTFS_ERR_NOENT)
                err = NTFS_ERR_FORMAT; /* flag says blocks exist; none do */
            goto out;
        }
        err = attr_read_all(vol, aa,
                            ext_alloc ? vol->mft_record_size
                                      : mft_attr_limit(dir_record, aa),
                            &w->alloc, &w->alloc_size);
        if (err)
            goto out;
        if (w->alloc_size % w->block_size != 0) {
            err = NTFS_ERR_FORMAT;
            goto out;
        }
        w->fixed = calloc(w->alloc_size / w->block_size, 1);
        if (!w->fixed) {
            err = NTFS_ERR_NOMEM;
            goto out;
        }
    }

    err = walk_node(w, root_node, root_avail);

out:
    free(ext_root);
    free(ext_alloc);
    free(w->alloc);
    free(w->fixed);
    w->alloc = NULL;
    w->fixed = NULL;
    return err;
}

int index_enumerate(const ntfs_volume *vol, const uint8_t *dir_record,
                    uint64_t dir_recno, index_visit_fn visit, void *ctx)
{
    struct index_walk w = {0};
    w.visit = visit;
    w.ctx = ctx;
    return index_walk_run(&w, vol, dir_record, dir_recno);
}

int index_lookup(const ntfs_volume *vol, const uint8_t *dir_record,
                 uint64_t dir_recno, const uint16_t *name, size_t name_units,
                 uint64_t *out_ref, uint32_t *out_file_attrs)
{
    struct index_walk w = {0};
    w.target = name;
    w.target_units = name_units;
    int err = index_walk_run(&w, vol, dir_record, dir_recno);
    if (err)
        return err;
    if (!w.found)
        return NTFS_ERR_NOENT;
    *out_ref = w.found_ref;
    *out_file_attrs = w.found_attrs;
    return NTFS_OK;
}

size_t utf16le_to_utf8(const uint8_t *utf16, size_t units,
                       char *out, size_t cap)
{
    size_t o = 0;
    for (size_t i = 0; i < units; i++) {
        uint32_t c = (uint32_t)(utf16[2 * i] | (utf16[2 * i + 1] << 8));
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < units) {
            uint32_t lo =
                (uint32_t)(utf16[2 * (i + 1)] | (utf16[2 * (i + 1) + 1] << 8));
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
                i++;
            } else {
                c = 0xFFFD;
            }
        } else if (c >= 0xDC00 && c <= 0xDFFF) {
            c = 0xFFFD;
        }

        char tmp[4];
        size_t n;
        if (c < 0x80) {
            tmp[0] = (char)c;
            n = 1;
        } else if (c < 0x800) {
            tmp[0] = (char)(0xC0 | (c >> 6));
            tmp[1] = (char)(0x80 | (c & 0x3F));
            n = 2;
        } else if (c < 0x10000) {
            tmp[0] = (char)(0xE0 | (c >> 12));
            tmp[1] = (char)(0x80 | ((c >> 6) & 0x3F));
            tmp[2] = (char)(0x80 | (c & 0x3F));
            n = 3;
        } else {
            tmp[0] = (char)(0xF0 | (c >> 18));
            tmp[1] = (char)(0x80 | ((c >> 12) & 0x3F));
            tmp[2] = (char)(0x80 | ((c >> 6) & 0x3F));
            tmp[3] = (char)(0x80 | (c & 0x3F));
            n = 4;
        }
        if (o + n >= cap)
            break;
        memcpy(out + o, tmp, n);
        o += n;
    }
    out[o] = '\0';
    return o;
}

int utf8_to_utf16(const char *s, size_t len,
                  uint16_t *out, size_t cap, size_t *units)
{
    size_t o = 0, i = 0;
    while (i < len) {
        uint32_t c;
        unsigned n;
        uint8_t b = (uint8_t)s[i];
        if (b < 0x80) {
            c = b;
            n = 1;
        } else if ((b & 0xE0) == 0xC0) {
            c = b & 0x1F;
            n = 2;
        } else if ((b & 0xF0) == 0xE0) {
            c = b & 0x0F;
            n = 3;
        } else if ((b & 0xF8) == 0xF0) {
            c = b & 0x07;
            n = 4;
        } else {
            return NTFS_ERR_NOENT;
        }
        if (i + n > len)
            return NTFS_ERR_NOENT;
        for (unsigned k = 1; k < n; k++) {
            if (((uint8_t)s[i + k] & 0xC0) != 0x80)
                return NTFS_ERR_NOENT;
            c = (c << 6) | ((uint8_t)s[i + k] & 0x3F);
        }
        i += n;

        if (c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF))
            return NTFS_ERR_NOENT;
        if (c >= 0x10000) {
            if (o + 2 > cap)
                return NTFS_ERR_NOENT;
            c -= 0x10000;
            out[o++] = (uint16_t)(0xD800 + (c >> 10));
            out[o++] = (uint16_t)(0xDC00 + (c & 0x3FF));
        } else {
            if (o + 1 > cap)
                return NTFS_ERR_NOENT;
            out[o++] = (uint16_t)c;
        }
    }
    *units = o;
    return NTFS_OK;
}
