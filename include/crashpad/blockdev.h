/*
 * crashpad/blockdev.h -- block device abstraction.
 *
 * Every byte the engine persists goes through this interface. Two backends:
 *   1. file backend: pread/pwrite/fsync on a real file (POSIX).
 *   2. simulated backend: records every write and sync into a trace so the
 *      crash explorer can rebuild "what the disk could look like" at any
 *      crash point.
 *
 * Rules (so both backends behave the same):
 *   - offsets and lengths are multiples of CP_SECTOR_SIZE
 *   - a write is NOT durable until a later sync() returns CP_OK
 *   - after a crash, an unsynced write may be lost, reordered with other
 *     unsynced writes, or torn at a sector boundary
 */
#ifndef CRASHPAD_BLOCKDEV_H
#define CRASHPAD_BLOCKDEV_H

#include <stddef.h>
#include <stdint.h>

#define CP_SECTOR_SIZE 512u
#define CP_PAGE_SIZE 4096u /* 8 sectors: a page write can tear */

typedef enum {
    CP_OK = 0,
    CP_E_IO = -1,
    CP_E_RANGE = -2,   /* misaligned or out-of-bounds request */
    CP_E_CORRUPT = -3, /* checksum / format violation */
    CP_E_NOMEM = -4,
    CP_E_CRASHED = -5  /* simulated device has "lost power" */
} cp_status;

typedef struct cp_blockdev cp_blockdev;

typedef struct cp_blockdev_ops {
    cp_status (*read)(cp_blockdev *dev, uint64_t off, void *buf, size_t len);
    cp_status (*write)(cp_blockdev *dev, uint64_t off, const void *buf, size_t len);
    cp_status (*sync)(cp_blockdev *dev);
    cp_status (*size)(cp_blockdev *dev, uint64_t *out_bytes);
    void (*close)(cp_blockdev *dev); /* frees dev and its context */
} cp_blockdev_ops;

struct cp_blockdev {
    const cp_blockdev_ops *ops;
    void *ctx;
};

/* Thin wrappers so call sites never touch the vtable directly. */
static inline cp_status cp_dev_read(cp_blockdev *d, uint64_t off, void *buf, size_t len) {
    return d->ops->read(d, off, buf, len);
}
static inline cp_status cp_dev_write(cp_blockdev *d, uint64_t off, const void *buf, size_t len) {
    return d->ops->write(d, off, buf, len);
}
static inline cp_status cp_dev_sync(cp_blockdev *d) { return d->ops->sync(d); }
static inline cp_status cp_dev_size(cp_blockdev *d, uint64_t *out) { return d->ops->size(d, out); }
static inline void cp_dev_close(cp_blockdev *d) {
    if (d) d->ops->close(d);
}

/* ---- file backend ------------------------------------------------------ */

/* create != 0 creates/truncates; otherwise the file must already exist. */
cp_status cp_file_dev_open(const char *path, int create, cp_blockdev **out);

/* ---- simulated backend + crash model ----------------------------------- */

typedef enum { CP_OP_WRITE = 1, CP_OP_SYNC = 2 } cp_op_kind;

typedef struct {
    cp_op_kind kind;
    uint64_t off;      /* WRITE only */
    uint32_t len;      /* WRITE only */
    uint64_t data_idx; /* WRITE only: index into the simulator's payload log */
} cp_trace_op;

/* What happens to writes issued since the last completed sync at crash time. */
typedef enum {
    CP_UNSYNCED_DROP_ALL = 0, /* none of them reached the platter */
    CP_UNSYNCED_KEEP_ALL,     /* all of them did */
    CP_UNSYNCED_SUBSET        /* a seeded subset did (models reordering) */
} cp_unsynced_policy;

typedef struct {
    uint64_t crash_point;      /* ops [0, crash_point) were issued before power loss */
    cp_unsynced_policy policy;
    uint64_t seed;             /* drives CP_UNSYNCED_SUBSET and tear choices; always print it */
    uint32_t reorder_window;   /* cap on unsynced writes considered for reordering (0 = no cap) */
    int tear_last_write;       /* if nonzero, the final surviving write lands only partially,
                                  cut at a sector boundary chosen from seed */
} cp_crash_spec;

typedef struct cp_sim_dev cp_sim_dev; /* opaque */

/* A simulated device that behaves like a normal cp_blockdev while recording. */
cp_status cp_sim_dev_create(uint64_t initial_bytes, cp_sim_dev **out_sim, cp_blockdev **out_dev);
void cp_sim_dev_destroy(cp_sim_dev *sim);

size_t cp_sim_trace_len(const cp_sim_dev *sim);
const cp_trace_op *cp_sim_trace_at(const cp_sim_dev *sim, size_t i);

/* Build a fresh device whose contents are the post-crash disk image for `spec`.
 * The recovery code under test then opens this device exactly as it would
 * open a real disk after a reboot. */
cp_status cp_sim_crash_image(const cp_sim_dev *sim, const cp_crash_spec *spec,
                             cp_blockdev **out_dev);

#endif /* CRASHPAD_BLOCKDEV_H */
