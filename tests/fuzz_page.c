#include "crashpad/page.h"
#include <stdint.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    cp_page page;
    memset(&page, 0, sizeof(page));
    if (size > sizeof(page.bytes)) size = sizeof(page.bytes);
    if (size) memcpy(page.bytes, data, size);
    if (size == sizeof(page.bytes) &&
        cp_page_verify(&page, cp_page_chdr(&page)->page_id) == CP_OK) {
        for (uint16_t i = 0; i < cp_page_chdr(&page)->nslots; ++i) {
            const void *cell = NULL;
            uint16_t len = 0;
            if (cp_page_get_cell(&page, i, &cell, &len) != CP_OK || !cell || !len)
                return 0;
        }
        cp_page_compact(&page);
        (void)cp_page_verify(&page, cp_page_chdr(&page)->page_id);
    }
    return 0;
}

#ifdef CRASHPAD_STANDALONE_FUZZ
#include <stdio.h>
#include <stdlib.h>
int main(void) {
    uint8_t bytes[CP_PAGE_SIZE];
    uint32_t state = UINT32_C(0x91e10da5);
    cp_page seed_page;
    cp_page_init(&seed_page, 11, CP_PAGE_BTREE_LEAF);
    uint64_t seed_value = UINT64_C(0x123456789abcdef0);
    (void)cp_page_insert_cell(&seed_page, 0, &seed_value, sizeof(seed_value));
    cp_page_seal(&seed_page);
    (void)LLVMFuzzerTestOneInput(seed_page.bytes, sizeof(seed_page.bytes));
    for (size_t trial = 0; trial < 100000; ++trial) {
        memcpy(bytes, seed_page.bytes, sizeof(bytes));
        for (size_t changes = 0; changes < 1 + trial % 16; ++changes) {
            state ^= state << 13; state ^= state >> 17; state ^= state << 5;
            size_t offset = (size_t)state % sizeof(bytes);
            bytes[offset] ^= (uint8_t)(state >> 24);
        }
        (void)LLVMFuzzerTestOneInput(bytes, sizeof(bytes));
    }
    puts("page fuzz smoke passed (100000 deterministic inputs)");
    return 0;
}
#endif
