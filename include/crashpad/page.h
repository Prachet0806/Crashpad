/*
 * crashpad/page.h -- 4 KiB slotted page.
 *
 *   offset 0                                                     offset 4096
 *   +--------+-------------------+ . . . free . . . +----------------------+
 *   | header | slot array  -->   |                  |  <-- cell heap       |
 *   | 48 B   | (4 B per slot)    |                  |  (cells grow down)   |
 *   +--------+-------------------+------------------+----------------------+
 *            ^ free_lower                            ^ free_upper
 *
 * Slots stay sorted by key (the B+-tree owns that ordering); a slot is
 * {offset, length} of its cell. Deleting a cell removes the slot and leaves a
 * hole in the heap that cp_page_compact() reclaims.
 *
 * Integrity: `checksum` is CRC32C over the whole page with the checksum field
 * zeroed. A page whose checksum fails is treated as torn/corrupt: recovery
 * restores it from a full-page image in the WAL rather than trusting it.
 *
 * All multi-byte fields are little-endian on disk. M1 may assume a
 * little-endian host; add explicit conversions if that ever changes.
 */
#ifndef CRASHPAD_PAGE_H
#define CRASHPAD_PAGE_H

#include <stddef.h>
#include <stdint.h>

#include "crashpad/blockdev.h"

#define CP_PAGE_MAGIC 0x44475043u /* "CPGD" little-endian */
#define CP_PAGE_VERSION 1u

typedef enum {
    CP_PAGE_FREE = 0,
    CP_PAGE_META = 1,
    CP_PAGE_BTREE_INTERNAL = 2,
    CP_PAGE_BTREE_LEAF = 3
} cp_page_type;

typedef struct {
    uint32_t magic;      /*  0 */
    uint16_t version;    /*  4 */
    uint16_t type;       /*  6  cp_page_type */
    uint64_t page_id;    /*  8  must match the page's position in the file */
    uint64_t lsn;        /* 16  LSN of the last WAL record applied to this page */
    uint64_t aux;        /* 24  leaf: right sibling page id; internal: leftmost child */
    uint32_t checksum;   /* 32  CRC32C, computed with this field zeroed */
    uint16_t nslots;     /* 36 */
    uint16_t free_lower; /* 38  end of slot array */
    uint16_t free_upper; /* 40  start of cell heap */
    uint16_t flags;      /* 42 */
    uint32_t reserved;   /* 44  must be zero */
} cp_page_header;        /* 48 bytes */

_Static_assert(sizeof(cp_page_header) == 48, "page header must be 48 bytes");

typedef struct {
    uint16_t off; /* cell offset from page start */
    uint16_t len; /* cell length in bytes */
} cp_slot;

_Static_assert(sizeof(cp_slot) == 4, "slot must be 4 bytes");

#define CP_PAGE_HEADER_SIZE ((uint16_t)sizeof(cp_page_header))

/* A page is just CP_PAGE_SIZE bytes; this alias documents intent and keeps
 * alignment suitable for the header's 8-byte fields. */
typedef struct {
    _Alignas(8) uint8_t bytes[CP_PAGE_SIZE];
} cp_page;

/* ---- integrity --------------------------------------------------------- */

uint32_t cp_crc32c(const void *data, size_t len, uint32_t seed);

/* Compute and store the checksum. Call immediately before writing to disk. */
void cp_page_seal(cp_page *p);

/* CP_OK if magic/version/checksum are valid; CP_E_CORRUPT otherwise.
 * Also pass expected_page_id so a misdirected write is caught. */
cp_status cp_page_verify(const cp_page *p, uint64_t expected_page_id);

/* ---- layout and cells -------------------------------------------------- */

void cp_page_init(cp_page *p, uint64_t page_id, cp_page_type type);

static inline cp_page_header *cp_page_hdr(cp_page *p) { return (cp_page_header *)p->bytes; }
static inline const cp_page_header *cp_page_chdr(const cp_page *p) {
    return (const cp_page_header *)p->bytes;
}

/* Contiguous free bytes between the slot array and the cell heap. */
uint16_t cp_page_free_space(const cp_page *p);

/* Free bytes if the heap were compacted (contiguous + holes). */
uint16_t cp_page_reclaimable_space(const cp_page *p);

/* Insert a cell so it becomes slot `index` (0..nslots). Returns CP_E_RANGE if
 * index is out of range or the cell does not fit even after compaction. */
cp_status cp_page_insert_cell(cp_page *p, uint16_t index, const void *cell, uint16_t len);

/* Remove slot `index`; the cell's bytes become a hole until compaction. */
cp_status cp_page_delete_cell(cp_page *p, uint16_t index);

/* Borrow a pointer to slot `index`'s cell. Valid until the page is modified. */
cp_status cp_page_get_cell(const cp_page *p, uint16_t index, const void **cell, uint16_t *len);

/* Squeeze out holes. Slot order and cell contents are unchanged. */
void cp_page_compact(cp_page *p);

/* ---- page-sized I/O helpers (page_id -> byte offset = page_id * CP_PAGE_SIZE) */

cp_status cp_page_read(cp_blockdev *dev, uint64_t page_id, cp_page *out);  /* verifies */
cp_status cp_page_write(cp_blockdev *dev, uint64_t page_id, cp_page *p);   /* seals, then writes */

#endif /* CRASHPAD_PAGE_H */
