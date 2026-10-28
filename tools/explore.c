#include "crashpad/store.h"
#include <stdio.h>
#include <string.h>

enum { EXPLORER_KEYS = 12, EXPLORER_WRITES = 16 };
enum { BROKEN_COMMIT = 2 };

static void put32le(uint8_t *p, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(value >> (i * 8));
}
static void put64le(uint8_t *p, uint64_t value) {
    for (unsigned i = 0; i < 8; ++i) p[i] = (uint8_t)(value >> (i * 8));
}
static void make_record(uint8_t *b, uint32_t type, uint64_t tx,
                        uint64_t key, uint64_t value) {
    memset(b, 0, CP_SECTOR_SIZE);
    put32le(b, UINT32_C(0x4c575043));
    put32le(b + 4, type);
    put64le(b + 8, tx);
    put64le(b + 16, key);
    put64le(b + 24, value);
    put32le(b + 32, cp_crc32c(b, 32, 0));
}

static int broken_wal_demo(void) {
    cp_sim_dev *sim = NULL;
    cp_blockdev *dev = NULL, *image = NULL;
    cp_store *store = NULL;
    uint8_t sector[CP_SECTOR_SIZE];
    if (cp_sim_dev_create(16u * 1024u * 1024u, &sim, &dev) != CP_OK) return 0;
    /* Deliberately violate WAL ordering: make the commit durable without prepare. */
    make_record(sector, BROKEN_COMMIT, 0, 42, 9001);
    cp_status status = cp_dev_write(dev, CP_PAGE_SIZE + CP_SECTOR_SIZE,
                                    sector, sizeof sector);
    if (status == CP_OK) status = cp_dev_sync(dev);
    cp_crash_spec spec = {(uint64_t)cp_sim_trace_len(sim), CP_UNSYNCED_DROP_ALL, 7, 0, 0};
    if (status == CP_OK) status = cp_sim_crash_image(sim, &spec, &image);
    if (status == CP_OK) status = cp_store_open(image, 8, &store);
    uint64_t value = 0;
    int caught = status == CP_OK && cp_store_get(store, 42, &value) != CP_OK;
    printf("broken WAL acknowledged-key violations=%d (expected 1)\n", caught ? 1 : 0);
    if (store) cp_store_close(store);
    if (image) cp_dev_close(image);
    cp_dev_close(dev);
    cp_sim_dev_destroy(sim);
    return caught;
}

static int check_state(cp_store *store, size_t point,
                       const uint64_t *put_ends,
                       const uint64_t *put_keys,
                       const uint64_t *put_values) {
    uint64_t required[EXPLORER_KEYS] = {0};
    unsigned char required_present[EXPLORER_KEYS] = {0};
    uint64_t maximum[EXPLORER_KEYS] = {0};
    for (size_t i = 0; i < EXPLORER_WRITES; ++i) {
        uint64_t key = put_keys[i];
        if (!required_present[key] || put_values[i] > maximum[key]) maximum[key] = put_values[i];
        if (put_ends[i] <= point) {
            required[key] = put_values[i];
            required_present[key] = 1;
        }
    }
    for (uint64_t key = 0; key < EXPLORER_KEYS; ++key) {
        uint64_t value = 0;
        cp_status status = cp_store_get(store, key, &value);
        if (status == CP_OK) {
            if (value > maximum[key] ||
                (required_present[key] && value < required[key])) return 0;
        } else if (status != CP_E_RANGE || required_present[key]) {
            return 0;
        }
    }
    return 1;
}

static void record_failure(const cp_sim_dev *sim, size_t *violations,
                           size_t point, cp_unsynced_policy policy,
                           unsigned seed, int tear) {
    if (*violations == 0)
    {
        printf("minimal failing schedule: point=%zu policy=%d seed=%u tear=%d\ntrace:\n",
               point, (int)policy, seed, tear);
        for (size_t i = 0; i < point; ++i) {
            const cp_trace_op *op = cp_sim_trace_at(sim, i);
            if (!op) continue;
            if (op->kind == CP_OP_WRITE)
                printf("  %zu WRITE off=%llu len=%u\n", i,
                       (unsigned long long)op->off, op->len);
            else
                printf("  %zu SYNC\n", i);
        }
    }
    ++*violations;
}

int main(int argc, char **argv) {
    int broken_demo = argc > 1 && strcmp(argv[1], "--broken-demo") == 0;
    cp_sim_dev *sim = NULL;
    cp_blockdev *dev = NULL;
    cp_store *store = NULL;
    uint64_t put_ends[EXPLORER_WRITES], put_keys[EXPLORER_WRITES], put_values[EXPLORER_WRITES];
    if (cp_sim_dev_create(16u * 1024u * 1024u, &sim, &dev) != CP_OK ||
        cp_store_open(dev, 8, &store) != CP_OK) return 1;
    for (size_t i = 0; i < EXPLORER_WRITES; ++i) {
        uint64_t key, value;
        if (i < 8) { key = i; value = key * 17 + 5; }
        else if (i < 12) { key = (i - 8) % 2; value = 10000 + i; }
        else { key = i - 4; value = 20000 + i; }
        if (cp_store_put(store, key, value) != CP_OK) return 1;
        put_keys[i] = key;
        put_values[i] = value;
        put_ends[i] = cp_sim_trace_len(sim);
        if (i == 7 && cp_store_checkpoint(store) != CP_OK) return 1;
    }
    if (cp_store_checkpoint(store) != CP_OK) return 1;
    /* Exercise replacement of a valid checkpoint after later WAL updates. */
    if (cp_store_checkpoint(store) != CP_OK) return 1;

    size_t points = cp_sim_trace_len(sim), cases = 0, violations = 0;
    const cp_unsynced_policy policies[] = {
        CP_UNSYNCED_DROP_ALL, CP_UNSYNCED_KEEP_ALL, CP_UNSYNCED_SUBSET
    };
    for (size_t p = 0; p <= points; ++p) {
        for (size_t q = 0; q < sizeof(policies) / sizeof(policies[0]); ++q) {
            unsigned seeds = policies[q] == CP_UNSYNCED_SUBSET ? 4u : 1u;
            for (unsigned seed = 1; seed <= seeds; ++seed) {
                int tear = p > 0 && cp_sim_trace_at(sim, p - 1) &&
                           cp_sim_trace_at(sim, p - 1)->kind == CP_OP_WRITE;
                cp_crash_spec spec = {
                    (uint64_t)p, policies[q], seed, 4u, tear
                };
                cp_blockdev *image = NULL;
                cp_store *recovered = NULL;
                if (cp_sim_crash_image(sim, &spec, &image) != CP_OK) {
                    record_failure(sim, &violations, p, policies[q], seed, tear);
                    continue;
                }
                cp_status status = cp_store_open(image, 8, &recovered);
                if (status != CP_OK) {
                    record_failure(sim, &violations, p, policies[q], seed, tear);
                } else {
                    cases++;
                    if (!check_state(recovered, p, put_ends, put_keys, put_values) ||
                        cp_store_recover(recovered) != CP_OK ||
                        !check_state(recovered, p, put_ends, put_keys, put_values))
                        record_failure(sim, &violations, p, policies[q], seed, tear);
                    cp_store_close(recovered);
                }
                cp_dev_close(image);
            }
        }
    }
    printf("crash points=%zu scenarios=%zu violations=%zu workload_puts=%u repeated_updates=yes checkpoints=3 seeds=1..4\n",
           points, cases, violations, (unsigned)EXPLORER_WRITES);
    int broken_caught = !broken_demo || broken_wal_demo();
    cp_store_close(store);
    cp_dev_close(dev);
    cp_sim_dev_destroy(sim);
    return violations || !broken_caught ? 2 : 0;
}
