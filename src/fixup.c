#include "fixup.h"

#include <string.h>

#include "ntfs.h"

int fixup_apply(uint8_t *buf, size_t size)
{
    if (size < 8)
        return NTFS_ERR_FORMAT;

    uint16_t usa_offset, usa_count;
    memcpy(&usa_offset, buf + 4, 2);
    memcpy(&usa_count, buf + 6, 2);

    /* usa_count is 1 (the sequence number) + one slot per stride, so the
     * stride is recoverable from the structure itself. */
    if (usa_count < 2)
        return NTFS_ERR_FORMAT;
    size_t strides = (size_t)usa_count - 1;
    if (size % strides != 0)
        return NTFS_ERR_FORMAT;
    size_t stride = size / strides;
    if (stride < 512 || stride % 512 != 0)
        return NTFS_ERR_FORMAT;

    /* The array must sit in the first stride, before its own fixup slot. */
    if (usa_offset < 8 || (size_t)usa_offset + 2 * (size_t)usa_count > stride - 2)
        return NTFS_ERR_FORMAT;

    const uint8_t *usa = buf + usa_offset;
    for (size_t i = 0; i < strides; i++) {
        uint8_t *tail = buf + (i + 1) * stride - 2;
        /* A tail that doesn't carry the sequence number means the
         * multi-sector write it belongs to never completed. */
        if (memcmp(tail, usa, 2) != 0)
            return NTFS_ERR_FORMAT;
        memcpy(tail, usa + 2 * (i + 1), 2);
    }
    return NTFS_OK;
}
