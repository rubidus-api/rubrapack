// src/setup/rp_result.h - how setup.exe adds up the results of several packages (the chain's removal,
// a patch removed from several products): the first failure stands; a restart request (3010) is kept
// over success, never over a failure. Plain C: tests/test_setup_result.c runs it on Linux.

#ifndef RUBRAPACK_RP_RESULT_H
#define RUBRAPACK_RP_RESULT_H

enum { RP_RESULT_RESTART = 3010 };      // ERROR_SUCCESS_REBOOT_REQUIRED

static inline unsigned rp_result_merge(unsigned so_far, unsigned r) {
    if (r == 0) return so_far;
    if (r == RP_RESULT_RESTART) return so_far == 0 ? r : so_far;
    return so_far == 0 || so_far == RP_RESULT_RESTART ? r : so_far;
}

#endif
