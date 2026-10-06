#include "file.h"

#include <stdlib.h>

#include "attr.h"
#include "mft.h"
#include "volume.h"

int file_read(const ntfs_volume *vol, const uint8_t *record, uint64_t recno,
              const char *stream, uint8_t **data, uint64_t *size)
{
    const struct attr_header *a;
    uint8_t *ext = NULL;

    int err = mft_find_attr(vol, record, recno, ATTR_TYPE_DATA, stream,
                            &a, &ext);
    if (err)
        return err;

    err = attr_read_all(vol, a,
                        ext ? vol->mft_record_size
                            : mft_attr_limit(record, a),
                        data, size);
    free(ext);
    return err;
}
