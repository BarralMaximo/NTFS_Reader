/*
 * Layer 4: file content — locate a $DATA stream in a record and
 * materialize it into memory.
 */
#ifndef FILE_H
#define FILE_H

#include <stdint.h>

#include "ntfs.h"

/* `record` is the file's MFT record (already read and fixed up), `recno`
 * its number. `stream` selects a named $DATA stream; NULL means the
 * default unnamed one. Buffer is malloc'd; free(*data) when done. */
int file_read(const ntfs_volume *vol, const uint8_t *record, uint64_t recno,
              const char *stream, uint8_t **data, uint64_t *size);

#endif /* FILE_H */
