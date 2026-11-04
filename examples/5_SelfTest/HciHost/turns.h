/* The Arduino preprocessor hoists function prototypes above everything in the
 * sketch, so a type used in a signature has to arrive from a header. */
#ifndef DS_HOST_SELFTEST_TURNS_H
#define DS_HOST_SELFTEST_TURNS_H

#include <stdint.h>

typedef struct {
  const char *label;
  const uint8_t *feed;   /* event to inject, NULL to inject nothing */
  uint16_t feedLen;
  const uint8_t *want;   /* command expected out, NULL for none     */
  uint16_t wantLen;
} turn_t;

#endif
