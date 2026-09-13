/*
 * aslr_slide.h — Resolve the ASLR slide of a Mach-O image in a
 *                remote process.
 */

#ifndef ASLR_SLIDE_H
#define ASLR_SLIDE_H

#include <mach/mach.h>
#include <stdbool.h>
#include <stdint.h>

/* Result of slide resolution. */
typedef struct {
    bool     found;
    uint64_t base;   /* Runtime base address of the Mach-O header. */
    uint64_t slide;  /* ASLR slide (base - preferred load address). */
} aslr_result_t;

/* Find the main executable's base address and ASLR slide in the
 * remote task.  Uses TASK_DYLD_INFO to walk the loaded image list. */
aslr_result_t aslr_get_slide(mach_port_t task);

#endif /* ASLR_SLIDE_H */
