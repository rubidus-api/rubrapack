#ifndef RUBRAPACK_PARTS_H
#define RUBRAPACK_PARTS_H

// include/rubrapack/parts.h - the helper custom-action DLL and the chain bootstrapper for each architecture
// (resources/bin/rubrapack_ca-*.dll, embedded by nob; RFC-0001 9.6, 16 Q5). A part that was not
// built is empty (length 0).

#include <stddef.h>

extern const unsigned char rp_ca_x64[];
extern const size_t        rp_ca_x64_len;
extern const unsigned char rp_ca_x86[];
extern const size_t        rp_ca_x86_len;
extern const unsigned char rp_ca_arm64[];
extern const size_t        rp_ca_arm64_len;

// The chain bootstrapper (src/setup/rubrapack_setup.c), per architecture; length 0 before
// `nob parts` has built it.
extern const unsigned char rp_setup_x64[];
extern const size_t        rp_setup_x64_len;
extern const unsigned char rp_setup_x86[];
extern const size_t        rp_setup_x86_len;
extern const unsigned char rp_setup_arm64[];
extern const size_t        rp_setup_arm64_len;

// The MSIX launcher (src/launch/rubrapack_launch.c, RFC-0019): a windows program (launch) and a
// console one (launchc), per architecture.
extern const unsigned char rp_launch_x64[];
extern const size_t        rp_launch_x64_len;
extern const unsigned char rp_launch_x86[];
extern const size_t        rp_launch_x86_len;
extern const unsigned char rp_launch_arm64[];
extern const size_t        rp_launch_arm64_len;
extern const unsigned char rp_launchc_x64[];
extern const size_t        rp_launchc_x64_len;
extern const unsigned char rp_launchc_x86[];
extern const size_t        rp_launchc_x86_len;
extern const unsigned char rp_launchc_arm64[];
extern const size_t        rp_launchc_arm64_len;

#endif // RUBRAPACK_PARTS_H
