#ifndef CRASHPAD_STORE_H
#define CRASHPAD_STORE_H
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include "crashpad/blockdev.h"
#include "crashpad/page.h"

typedef struct cp_store cp_store;
typedef struct { uint64_t key, value; } cp_kv;
typedef struct { uint64_t key, left_value, right_value; } cp_join_row;
typedef int (*cp_scan_fn)(uint64_t key, uint64_t value, void *context);

/* Open a store over a borrowed block device. The caller closes the device
 * after closing the store. `buffer_pages` sets its checkpoint buffer pool. */
cp_status cp_store_open(cp_blockdev *dev, size_t buffer_pages, cp_store **out);
void cp_store_close(cp_store *store);
cp_status cp_store_put(cp_store *store, uint64_t key, uint64_t value);
cp_status cp_store_get(cp_store *store, uint64_t key, uint64_t *value_out);
cp_status cp_store_scan(cp_store *store, uint64_t first, uint64_t last,
                        cp_scan_fn callback, void *context);
cp_status cp_store_checkpoint(cp_store *store);
cp_status cp_store_recover(cp_store *store);
uint64_t cp_store_operation_count(const cp_store *store);

/* Test/explorer hooks: trace simulated device operation on the store. */
cp_blockdev *cp_store_device(cp_store *store);

typedef struct cp_coordinator cp_coordinator;
cp_status cp_coordinator_open(const char *directory, size_t nshards,
                              size_t buffer_pages_per_shard,
                              cp_coordinator **out);
void cp_coordinator_close(cp_coordinator *coordinator);
cp_status cp_coordinator_put(cp_coordinator *, uint64_t key, uint64_t value);
cp_status cp_coordinator_get(cp_coordinator *, uint64_t key, uint64_t *value);
cp_status cp_coordinator_scan(cp_coordinator *, uint64_t first, uint64_t last,
                              cp_scan_fn callback, void *context);
cp_status cp_coordinator_join(cp_coordinator *, const cp_kv *left, size_t nleft,
                              const cp_kv *right, size_t nright,
                              cp_join_row **out_rows, size_t *out_count);
cp_status cp_coordinator_join_ex(cp_coordinator *, const cp_kv *left, size_t nleft,
                                 const cp_kv *right, size_t nright,
                                 int mitigate_hot_keys,
                                 cp_join_row **out_rows, size_t *out_count);
void cp_coordinator_explain(cp_coordinator *, const cp_kv *left, size_t nleft,
                           const cp_kv *right, size_t nright, FILE *out);
void cp_coordinator_explain_ex(cp_coordinator *, const cp_kv *left, size_t nleft,
                               const cp_kv *right, size_t nright,
                               int mitigation_enabled, FILE *out);
size_t cp_coordinator_shard_for(const cp_coordinator *, uint64_t key);
size_t cp_coordinator_shard_count(const cp_coordinator *);

#endif
