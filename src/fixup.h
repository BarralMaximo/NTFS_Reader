/*
 * Multi-sector transfer protection ("fixups" / update sequence arrays).
 *
 * Every structure NTFS writes in multi-sector chunks — MFT records, index
 * blocks — stores the real last two bytes of each 512-byte stride in an
 * array in its header, and overwrites those on-disk positions with a
 * per-write sequence number. On read, each stride's tail must equal that
 * number (a mismatch means a torn write) and the real bytes must be put
 * back before the payload is parsed. A parser that skips this step reads
 * two garbage bytes at every stride boundary.
 */
#ifndef FIXUP_H
#define FIXUP_H

#include <stddef.h>
#include <stdint.h>

/* Verify and undo fixups in place. `size` is the full multi-sector
 * structure size; the stride is derived from the update sequence count,
 * as the layout is self-describing. */
int fixup_apply(uint8_t *buf, size_t size);

#endif /* FIXUP_H */
