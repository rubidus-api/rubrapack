#ifndef PROVEN_INTERNAL_BROTLI_H
#define PROVEN_INTERNAL_BROTLI_H

#include <stdbool.h>

/* Tests: true makes the Brotli decoder use its plain build even where the processor has BMI2;
 * false asks the processor again. Output is the same either way. */
void proven_brotli_decode_force_plain_(bool on);

#endif
