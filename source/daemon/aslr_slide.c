/*
 * aslr_slide.c — Determine ASLR slide for a remote process.
 *
 * Reads dyld_all_image_infos from the target task, extracts the main
 * executable's runtime base address, walks its Mach-O load commands to
 * find the __TEXT segment preferred vmaddr, and computes slide.
 *
 * All memory access is read-only via rm_read / rm_read_ptr.
 */

#include "aslr_slide.h"
#include "remote_memory.h"

#include <mach/mach.h>
#include <mach-o/loader.h>
#include <string.h>

typedef uint64_t mach_vm_address_t;
typedef uint64_t mach_vm_size_t;

/* ── dyld task info ─────────────────────────────────────────────────── */

#ifndef TASK_DYLD_INFO
#define TASK_DYLD_INFO          17
#define TASK_DYLD_INFO_COUNT    (sizeof(struct task_dyld_info) / sizeof(natural_t))
struct task_dyld_info {
    mach_vm_address_t all_image_info_addr;
    mach_vm_size_t    all_image_info_size;
    integer_t         all_image_info_format;
};
#endif

/* ── Remote struct field offsets (64-bit) ────────────────────────────
 *
 * dyld_all_image_infos:
 *   +0x00  uint32_t  version
 *   +0x04  uint32_t  infoArrayCount
 *   +0x08  uint64_t  infoArray          (pointer)
 *
 * dyld_image_info:
 *   +0x00  uint64_t  imageLoadAddress   (const struct mach_header*)
 *   +0x08  uint64_t  imageFilePath      (const char*)
 *   +0x10  uint64_t  imageFileModDate   (uintptr_t)
 *
 * We only need a handful of reads, so we use explicit offsets rather
 * than declaring the full structs.
 */

#define DYLD_AII_OFF_VERSION          0x00
#define DYLD_AII_OFF_INFO_ARRAY_COUNT 0x04
#define DYLD_AII_OFF_INFO_ARRAY       0x08

#define DYLD_IMG_OFF_LOAD_ADDR        0x00

/* Size of a single dyld_image_info entry (3 pointers on 64-bit). */
#define DYLD_IMAGE_INFO_SIZE          24

static const aslr_result_t kFailed = { .found = false, .base = 0, .slide = 0 };

/* ── Implementation ─────────────────────────────────────────────────── */

aslr_result_t aslr_get_slide(mach_port_t task)
{
    if (!MACH_PORT_VALID(task))
        return kFailed;

    /* 1. Obtain dyld_all_image_infos address via task_info. */
    struct task_dyld_info dyld_info;
    mach_msg_type_number_t count = TASK_DYLD_INFO_COUNT;

    kern_return_t kr = task_info(task,
                                TASK_DYLD_INFO,
                                (task_info_t)&dyld_info,
                                &count);
    if (kr != KERN_SUCCESS)
        return kFailed;

    uint64_t all_image_info_addr = dyld_info.all_image_info_addr;
    if (all_image_info_addr == 0)
        return kFailed;

    /* 2. Read infoArrayCount (uint32 at +0x04). */
    uint32_t info_array_count = 0;
    if (!rm_read(task,
                 all_image_info_addr + DYLD_AII_OFF_INFO_ARRAY_COUNT,
                 &info_array_count,
                 sizeof(info_array_count)))
        return kFailed;

    if (info_array_count == 0)
        return kFailed;

    /* 3. Read infoArray pointer (uint64 at +0x08). */
    uint64_t info_array_ptr = rm_read_ptr(task,
                                          all_image_info_addr + DYLD_AII_OFF_INFO_ARRAY);
    if (info_array_ptr == 0)
        return kFailed;

    /* 4. Read the first entry's imageLoadAddress (pointer at +0x00). */
    uint64_t base_addr = rm_read_ptr(task,
                                     info_array_ptr + DYLD_IMG_OFF_LOAD_ADDR);
    if (base_addr == 0)
        return kFailed;

    /* 5. Read Mach-O header at the base address and verify magic. */
    struct mach_header_64 mh;
    if (!rm_read(task, base_addr, &mh, sizeof(mh)))
        return kFailed;

    if (mh.magic != MH_MAGIC_64)
        return kFailed;

    /* 6. Walk load commands to find __TEXT segment vmaddr. */
    uint64_t cmd_offset = base_addr + sizeof(struct mach_header_64);
    uint64_t text_vmaddr = 0;
    bool     text_found  = false;

    for (uint32_t i = 0; i < mh.ncmds; i++) {
        struct load_command lc;
        if (!rm_read(task, cmd_offset, &lc, sizeof(lc)))
            return kFailed;

        /* Sanity: cmdsize must be at least sizeof(load_command). */
        if (lc.cmdsize < sizeof(struct load_command))
            return kFailed;

        if (lc.cmd == LC_SEGMENT_64) {
            struct segment_command_64 seg;
            if (lc.cmdsize < sizeof(seg))
                return kFailed;

            if (!rm_read(task, cmd_offset, &seg, sizeof(seg)))
                return kFailed;

            if (strncmp(seg.segname, "__TEXT", 6) == 0) {
                text_vmaddr = seg.vmaddr;
                text_found  = true;
                break;
            }
        }

        cmd_offset += lc.cmdsize;
    }

    if (!text_found)
        return kFailed;

    /* 7. Compute slide. */
    uint64_t slide = base_addr - text_vmaddr;

    /* 8. Return result. */
    return (aslr_result_t){
        .found = true,
        .base  = base_addr,
        .slide = slide,
    };
}
