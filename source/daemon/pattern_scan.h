/*
 * pattern_scan.h — Byte pattern scanner for remote process memory.
 */

#ifndef PATTERN_SCAN_H
#define PATTERN_SCAN_H

#include <mach/mach.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Scan remote memory in the range [start, start+size) for the given
 * byte pattern.  `mask` specifies which bytes in `pattern` must match
 * (0xFF = must match, 0x00 = wildcard).  Returns the address of the
 * first match, or 0 if not found. */
uint64_t pat_scan(mach_port_t task,
                  uint64_t    start,
                  uint64_t    size,
                  const uint8_t *pattern,
                  const uint8_t *mask,
                  size_t      pat_len);

/* Scan a specific Mach-O segment (by name, e.g. "__TEXT") for the
 * given pattern.  Returns 0 if the segment is not found or no match. */
uint64_t pat_scan_segment(mach_port_t task,
                          uint64_t    image_base,
                          const char *segment_name,
                          const uint8_t *pattern,
                          const uint8_t *mask,
                          size_t      pat_len);

#endif /* PATTERN_SCAN_H */
