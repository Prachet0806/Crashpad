#include "crashpad/store.h"
#include <stdio.h>
#include <string.h>

enum { EXPLORER_WRITES = 8 };
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

static int check_state(cp_store *store, size_t acknowledged) {
    for (uint64_t key = 0; key < EXPLORER_WRITES; ++key) {
        uint64_t value = 0;
        cp_status status = cp_store_get(store, key, &value);
        if (status == CP_OK) {
            if (value != key * 17 + 5) return 0;
        } else if (status != CP_E_RANGE || key < acknowledged) {
            return 0;
        }
    }
    return 1;
}

static void record_failure(size_t *violations, size_t point,
                           cp_unsynced_policy policy, unsigned seed,
                           int tear, size_t acknowledged) {
    if (*violations == 0)
        printf("minimal failing schedule: point=%zu policy=%d seed=%u tear=%d acknowledged=%zu\n",
               point, (int)policy, seed, tear, acknowledged);
    ++*violations;
}

int main(int argc, char **argv) {
    int broken_demo = argc > 1 && strcmp(argv[1], "--broken-demo") == 0;
    cp_sim_dev *sim = NULL;
    cp_blockdev *dev = NULL;
    cp_store *store = NULL;
    if (cp_sim_dev_create(16u * 1024u * 1024u, &sim, &dev) != CP_OK ||
        cp_store_open(dev, 8, &store) != CP_OK) return 1;
    for (uint64_t key = 0; key < EXPLORER_WRITES; ++key) {
        if (cp_store_put(store, key, key * 17 + 5) != CP_OK) return 1;
    }
    if (cp_store_checkpoint(store) != CP_OK) return 1;

    size_t points = cp_sim_trace_len(sim), cases = 0, violations = 0;
    const cp_unsynced_policy policies[] = {
        CP_UNSYNCED_DROP_ALL, CP_UNSYNCED_KEEP_ALL, CP_UNSYNCED_SUBSET
    };
    for (size_t p = 0; p <= points; ++p) {
        size_t acknowledged = 0;
        for (size_t i = 0; i < p; ++i) {
            const cp_trace_op *op = cp_sim_trace_at(sim, i);
            if (op && op->kind == CP_OP_SYNC && acknowledged < EXPLORER_WRITES)
                acknowledged++;
        }
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
                    record_failure(&violations, p, policies[q], seed, tear, acknowledged);
                    continue;
                }
                cp_status status = cp_store_open(image, 8, &recovered);
                if (status != CP_OK) {
                    record_failure(&violations, p, policies[q], seed, tear, acknowledged);
                } else {
                    cases++;
                    if (!check_state(recovered, acknowledged) ||
                        cp_store_recover(recovered) != CP_OK ||
                        !check_state(recovered, acknowledged))
                        record_failure(&violations, p, policies[q], seed, tear, acknowledged);
                    cp_store_close(recovered);
                }
                cp_dev_close(image);
            }
        }
    }
    printf("crash points=%zu scenarios=%zu violations=%zu seeds=1..4\n",
           points, cases, violations);
    int broken_caught = !broken_demo || broken_wal_demo();
    cp_store_close(store);
    cp_dev_close(dev);
    cp_sim_dev_destroy(sim);
    return violations || !broken_caught ? 2 : 0;
}
