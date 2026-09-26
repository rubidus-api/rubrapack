#ifndef RUBRAPACK_PARTS_H
#define RUBRAPACK_PARTS_H

// include/rubrapack/parts.h - the helper custom-action DLL for each package architecture
// (resources/bin/rubrapack_ca-*.dll, embedded by nob; RFC-0001 9.6, 16 Q5). A part that was not
// built is empty (length 0).

#include <stddef.h>

extern const unsigned char rp_ca_x64[];
extern const size_t        rp_ca_x64_len;
extern const unsigned char rp_ca_x86[];
extern const size_t        rp_ca_x86_len;
extern const unsigned char rp_ca_arm64[];
extern const size_t        rp_ca_arm64_len;

#endif // RUBRAPACK_PARTS_H
