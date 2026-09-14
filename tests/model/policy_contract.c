/* Finite boundary/translation-invariance checks of REAL production fw_update.c.
 * This complements check_flash.py; it does not prove its refinement into C. */
#include "fw_update.h"
#include <assert.h>
#include <stdio.h>

_Static_assert(FW_UPDATE_RESPONSE_TIMEOUT_US == 100000u, "review retry contract");
_Static_assert(FW_UPDATE_STALL_TIMEOUT_US == 30000000u, "review progress contract");
_Static_assert(FW_UPDATE_PEER_TIMEOUT_US == 3500000u, "review peer contract");

int main(void) {
    const uint32_t ages[] = {0, 1, 99999, 100000, 100001, 3499999,
                            3500000, 29999999, 30000000, UINT32_MAX};
    const uint32_t origins[] = {0, 1, 100000, UINT32_MAX - 42};
    unsigned cases = 0;
    for (unsigned source = 0; source <= FW_UPDATE_SOURCE_DROP; source++)
    for (unsigned done = 0; done < 2; done++)
    for (unsigned dirty = 0; dirty < 2; dirty++)
    for (unsigned request = 0; request < sizeof ages / sizeof ages[0]; request++)
    for (unsigned progress = 0; progress < sizeof ages / sizeof ages[0]; progress++)
    for (unsigned peer = 0; peer < sizeof ages / sizeof ages[0]; peer++) {
        const uint32_t req_age = ages[request], progress_age = ages[progress], peer_age = ages[peer];
        fw_update_action_t expected;
        if (source != FW_UPDATE_SOURCE_PULL)
            expected = FW_UPDATE_WAIT;
        else if (progress_age >= 30000000)
            expected = !dirty ? FW_UPDATE_ABANDON : peer_age < 3500000 ? FW_UPDATE_RESTART : FW_UPDATE_PAUSE;
        else
            expected = done || req_age >= 100000 ? FW_UPDATE_REQUEST : FW_UPDATE_WAIT;
        for (unsigned o = 0; o < sizeof origins / sizeof origins[0]; o++) {
            const uint32_t now = origins[o];
            fw_update_action_t actual = fw_update_next_action((fw_update_source_t)source,
                done, dirty, now, now - req_age, now - progress_age, now - peer_age);
            assert(actual == expected);
            cases++;
        }
    }
    /* A response has no authority outside a pending request of the pull owner. */
    for (unsigned source = 0; source <= FW_UPDATE_SOURCE_DROP; source++)
    for (unsigned pending = 0; pending < 2; pending++)
    for (unsigned expected = 0; expected < 4; expected++)
    for (unsigned received = 0; received < 4; received++) {
        assert(fw_update_response_expected((fw_update_source_t)source, pending,
                                           expected * 4, received * 4)
               == (source == FW_UPDATE_SOURCE_PULL && pending && expected == received));
        cases++;
    }
    printf("production update policy: %u finite contract cases passed\n", cases);
}
