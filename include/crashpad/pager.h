#ifndef CRASHPAD_PAGER_H
#define CRASHPAD_PAGER_H
#include "crashpad/page.h"

typedef struct cp_buffer_pool cp_buffer_pool;
typedef cp_status (*cp_wal_flush_fn)(void *context, uint64_t through_lsn);

/* The device remains owned by the caller. A zero-sized pool is rejected. */
cp_status cp_buffer_pool_create(cp_blockdev *device, size_t frame_count,
                               cp_wal_flush_fn flush_wal, void *wal_context,
                               cp_buffer_pool **out);
/* Flush and release the pool; if flushing fails, the pool remains valid. */
cp_status cp_buffer_pool_close(cp_buffer_pool *pool);
/* Force-release without reporting or preserving dirty frames. */
void cp_buffer_pool_destroy(cp_buffer_pool *pool);
cp_status cp_buffer_pool_fetch(cp_buffer_pool *pool, uint64_t page_id,
                               cp_page **out_page);
cp_status cp_buffer_pool_new(cp_buffer_pool *pool, uint64_t page_id,
                             cp_page_type type, cp_page **out_page);
cp_status cp_buffer_pool_mark_dirty(cp_buffer_pool *pool, uint64_t page_id,
                                    uint64_t page_lsn);
cp_status cp_buffer_pool_unpin(cp_buffer_pool *pool, uint64_t page_id);
cp_status cp_buffer_pool_flush(cp_buffer_pool *pool);

#endif
