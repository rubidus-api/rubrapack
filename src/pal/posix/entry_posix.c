// src/pal/posix/entry_posix.c - process entry on POSIX: argv is already UTF-8 bytes.
// rp_main checks that every argument is valid UTF-8.

#include "rubrapack/cli.h"

int main(int argc, char **argv) {
    return rp_main(argc, argv);
}
