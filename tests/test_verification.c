#include "verification.h"
#include <assert.h>
#include <stdio.h>

static verify_result_t good(void) {
    return (verify_result_t){.token=3, .remote=true, .transport=VERIFY_TRANSPORT_OK,
        .snapshot={.role=1, .major=0, .minor=101, .board_id={1,2,3,4,5,6,7,8},
          .boot_session=9, .image_crc_at_boot=0x11223344,
          .started_us=100000, .completed_us=1200000,
          .generation_start=11, .generation_end=11,
          .bytes_read=VERIFY_IMAGE_BYTES, .slot_crc32=0xdeadbeef,
          .metadata_magic=0xf00d, .metadata_version=201, .metadata_crc32=0x11223344,
          .start={.core_valid=3,.core_ticks={100,200}},
          .end={.core_valid=3,.core_ticks={1100,1200}},
          .outcome=VERIFY_SCAN_COMPLETE}};
}
static void check(verify_result_t result, verify_verdict_t verdict, verify_reason_t reason) {
    verify_assessment_t a=verification_assess(&result,201,0xdeadbeef);
    assert(a.verdict==verdict && a.reason==reason);
}
int main(void) {
    verify_result_t r=good(); check(r,VERIFY_PASS,VERIFY_REASON_MATCH);
    r.remote=false; check(r,VERIFY_PASS,VERIFY_REASON_MATCH);
    r=good();r.snapshot.minor=100;r.snapshot.metadata_version=200;
    check(r,VERIFY_FAIL,VERIFY_REASON_BUILD);
    r=good();r.snapshot.slot_crc32^=1;check(r,VERIFY_FAIL,VERIFY_REASON_CRC);
    r=good();r.snapshot.metadata_magic=0;check(r,VERIFY_FAIL,VERIFY_REASON_METADATA);
    r=good();r.snapshot.metadata_version=200;check(r,VERIFY_FAIL,VERIFY_REASON_METADATA);
    r=good();r.snapshot.metadata_crc32^=1;check(r,VERIFY_FAIL,VERIFY_REASON_METADATA);
    r=good();r.snapshot.metadata_reserved=1;check(r,VERIFY_FAIL,VERIFY_REASON_METADATA);
    r=good();r.snapshot.major=66;check(r,VERIFY_FAIL,VERIFY_REASON_METADATA);
    r=good();r.snapshot.minor=1000;check(r,VERIFY_UNVERIFIED,VERIFY_REASON_INVALID);
    r=good();r.snapshot.role=2;check(r,VERIFY_UNVERIFIED,VERIFY_REASON_INVALID);
    r=good();r.snapshot.bytes_read--;check(r,VERIFY_UNVERIFIED,VERIFY_REASON_INVALID);
    r=good();r.snapshot.completed_us=r.snapshot.started_us;check(r,VERIFY_UNVERIFIED,VERIFY_REASON_INVALID);
    r=good();r.snapshot.completed_us=r.snapshot.started_us-1;check(r,VERIFY_UNVERIFIED,VERIFY_REASON_INVALID);
    r=good();r.snapshot.completed_us=r.snapshot.started_us+VERIFY_TIMEOUT_US+1;check(r,VERIFY_UNVERIFIED,VERIFY_REASON_INVALID);
    r=good();r.snapshot.generation_end++;check(r,VERIFY_UNVERIFIED,VERIFY_REASON_CHANGED);
    r=good();r.snapshot.end.update_attempt++;check(r,VERIFY_UNVERIFIED,VERIFY_REASON_CHANGED);
    r=good();r.snapshot.end.phase=DIAGNOSTIC_UPDATE_RECEIVING;check(r,VERIFY_UNVERIFIED,VERIFY_REASON_CHANGED);
    r=good();r.snapshot.end.source=DIAGNOSTIC_SOURCE_PEER;check(r,VERIFY_UNVERIFIED,VERIFY_REASON_CHANGED);
    // A completed, abandoned no-write attempt can predate a fresh stable scan.
    r=good();r.snapshot.start.phase=r.snapshot.end.phase=DIAGNOSTIC_UPDATE_ABANDONED;
    r.snapshot.start.update_attempt=r.snapshot.end.update_attempt=7;
    check(r,VERIFY_PASS,VERIFY_REASON_MATCH);
    for(unsigned core=0;core<2;++core) {
        r=good();r.snapshot.start.core_valid &= ~(1u<<core);check(r,VERIFY_UNVERIFIED,VERIFY_REASON_CORE_UNAVAILABLE);
        r=good();r.snapshot.end.core_valid &= ~(1u<<core);check(r,VERIFY_UNVERIFIED,VERIFY_REASON_CORE_UNAVAILABLE);
        r=good();r.snapshot.end.core_ticks[core]=r.snapshot.start.core_ticks[core];check(r,VERIFY_UNVERIFIED,VERIFY_REASON_CORE_PROGRESS);
        r=good();r.snapshot.end.core_ticks[core]=r.snapshot.start.core_ticks[core]-1;check(r,VERIFY_UNVERIFIED,VERIFY_REASON_CORE_PROGRESS);
        r=good();r.snapshot.end.core_age_ms[core]=101;check(r,VERIFY_UNVERIFIED,VERIFY_REASON_CORE_PROGRESS);
        r=good();r.snapshot.end.core_age_ms[core]=100;check(r,VERIFY_PASS,VERIFY_REASON_MATCH);
        r=good();r.snapshot.start.core_ticks[core]=UINT32_MAX-2;r.snapshot.end.core_ticks[core]=997;check(r,VERIFY_PASS,VERIFY_REASON_MATCH);
    }
    const verify_reason_t scan_reasons[]={VERIFY_REASON_MATCH,VERIFY_REASON_UPDATE_ACTIVE,
        VERIFY_REASON_CHANGED,VERIFY_REASON_BUSY,VERIFY_REASON_TIMEOUT,VERIFY_REASON_INVALID};
    for(unsigned outcome=1;outcome<=5;++outcome) {
        r=good();r.snapshot.outcome=(verify_scan_outcome_t)outcome;
        r.snapshot.bytes_read=123;r.snapshot.slot_crc32=0; // partial data can never FAIL a CRC expectation
        check(r,VERIFY_UNVERIFIED,scan_reasons[outcome]);
    }
    const verify_reason_t transport_reasons[]={VERIFY_REASON_MATCH,VERIFY_REASON_TIMEOUT,
        VERIFY_REASON_INVALID,VERIFY_REASON_BUSY,VERIFY_REASON_INVALID};
    for(unsigned outcome=1;outcome<=4;++outcome) {
        r=good();r.transport=(verify_transport_t)outcome;
        check(r,VERIFY_UNVERIFIED,transport_reasons[outcome]);
    }
    r=good();r.snapshot.slot_crc32=0;
    assert(verification_assess(&r,201,0).verdict==VERIFY_PASS); // zero is a valid CRC
    puts("verification assessment tests passed");
}
