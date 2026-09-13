/*
 * pattern_scan.c – Remote pattern scanner for ARM64 Mach-O processes.
 *
 * Reads remote memory in page-sized chunks via rm_read and matches
 * byte patterns with per-byte wildcard masks.  Supports scanning a
 * specific Mach-O segment by parsing load commands to resolve the
 * ASLR slide.
 *
 * All memory access is read-only.  No writes to the target process.
 */

#include "pattern_scan.h"
#include "remote_memory.h"

#include <mach-o/loader.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------ */
/*  Constants                                                               */
/* ------------------------------------------------------------------------ */

#define PAGE_SIZE_SCAN   4096u
#define MAX_SCAN_SIZE    (256u * 1024u * 1024u)   /* 256 MB cap */

/* ------------------------------------------------------------------------ */
/*  Internal helpers                                                        */
/* ------------------------------------------------------------------------ */

/*
 * match_at – Check whether `buf` at offset `pos` satisfies the pattern.
 *
 * mask[i] == 0x00  →  wildcard (any byte matches)
 * mask[i] == 0xFF  →  pattern[i] must match exactly
 */
static bool match_at(const uint8_t *buf, size_t pos,
                     const uint8_t *pattern, const uint8_t *mask,
                     size_t pat_len)
{
    for (size_t i = 0; i < pat_len; i++) {
        if (mask[i] != 0x00 && buf[pos + i] != pattern[i]) {
            return false;
        }
    }
    return true;
}

/* ------------------------------------------------------------------------ */
/*  pat_scan                                                                */
/* ------------------------------------------------------------------------ */

uint64_t pat_scan(mach_port_t task,
                  uint64_t    start,
                  uint64_t    size,
                  const uint8_t *pattern,
                  const uint8_t *mask,
                  size_t      pat_len)
{
    if (!pattern || !mask || pat_len == 0) {
        return 0;
    }

    /* Cap the scan region to avoid runaway reads. */
    if (size > MAX_SCAN_SIZE) {
        size = MAX_SCAN_SIZE;
    }

    /*
     * We keep an overlap buffer large enough for one full page plus the
     * tail of the previous page (pat_len - 1 bytes) so that matches
     * spanning a page boundary are detected.
     */
    const size_t overlap   = pat_len - 1;
    const size_t buf_alloc = overlap + PAGE_SIZE_SCAN;

    uint8_t *buf = (uint8_t *)malloc(buf_alloc);
    if (!buf) {
        return 0;
    }

    /*
     * `buf` layout:
     *   [ overlap zone | page data ]
     *    0..overlap-1    overlap..overlap+page_bytes-1
     *
     * On the first iteration the overlap zone is zeroed and we start
     * matching only from offset `overlap` (i.e. no phantom matches on
     * uninitialised data).
     */
    memset(buf, 0, buf_alloc);

    uint64_t offset    = 0;        /* bytes consumed from the region      */
    bool     first     = true;     /* first chunk has no valid overlap     */

    while (offset < size) {
        /* How many bytes remain in the scan region? */
        uint64_t remaining = size - offset;
        size_t   to_read   = (remaining < PAGE_SIZE_SCAN)
                                 ? (size_t)remaining
                                 : PAGE_SIZE_SCAN;

        /* Shift tail of previous page into the overlap zone. */
        if (!first && overlap > 0) {
            memmove(buf, buf + PAGE_SIZE_SCAN, overlap);
        }

        /* Read the next page from the remote process. */
        uint8_t *page_dst = buf + overlap;
        size_t   got      = rm_read(task,
                                    start + offset,
                                    page_dst,
                                    to_read);
        if (got == 0) {
            /* Unreadable page – skip it. */
            offset += to_read;
            first   = true;          /* overlap is now invalid */
            memset(buf, 0, overlap); /* clear stale overlap    */
            continue;
        }

        /*
         * Determine the searchable window inside `buf`.
         *
         * On the first chunk we start at `overlap` (no prior data).
         * On subsequent chunks we start at 0 so that cross-boundary
         * matches are tested.
         *
         * The last valid start position is `overlap + got - pat_len`.
         */
        size_t search_start = first ? overlap : 0;
        size_t usable       = overlap + got;

        if (usable < pat_len) {
            /* Not enough data to hold even one full pattern. */
            offset += to_read;
            first   = false;
            continue;
        }

        size_t search_end = usable - pat_len; /* inclusive */

        for (size_t i = search_start; i <= search_end; i++) {
            if (match_at(buf, i, pattern, mask, pat_len)) {
                /*
                 * Translate the buffer-relative position back to a
                 * remote address.
                 *
                 * `i` is relative to `buf`.
                 * Page data starts at buf[overlap] and corresponds to
                 * remote address `start + offset`.
                 *
                 * So the remote address is:
                 *   start + offset - overlap + i      (when i >= 0)
                 *
                 * But on the first chunk the overlap zone was empty, so
                 * the formula still holds because search_start == overlap.
                 */
                uint64_t match_addr = (start + offset) - overlap + i;
                free(buf);
                return match_addr;
            }
        }

        offset += to_read;
        first   = false;
    }

    free(buf);
    return 0;
}

/* ------------------------------------------------------------------------ */
/*  pat_scan_segment                                                        */
/* ------------------------------------------------------------------------ */

uint64_t pat_scan_segment(mach_port_t task,
                          uint64_t    image_base,
                          const char *segment_name,
                          const uint8_t *pattern,
                          const uint8_t *mask,
                          size_t      pat_len)
{
    if (!segment_name || !pattern || !mask || pat_len == 0) {
        return 0;
    }

    /* ------------------------------------------------------------------ */
    /*  1. Read the Mach-O header.                                        */
    /* ------------------------------------------------------------------ */

    struct mach_header_64 hdr;
    memset(&hdr, 0, sizeof(hdr));

    size_t got = rm_read(task, image_base, (uint8_t *)&hdr, sizeof(hdr));
    if (got < sizeof(hdr)) {
        return 0;
    }
    if (hdr.magic != MH_MAGIC_64) {      /* 0xFEEDFACF */
        return 0;
    }

    /* Sanity-check ncmds to prevent absurd allocations. */
    if (hdr.ncmds > 512 || hdr.sizeofcmds > (4u * 1024u * 1024u)) {
        return 0;
    }

    /* ------------------------------------------------------------------ */
    /*  2. Read all load commands in one shot.                            */
    /* ------------------------------------------------------------------ */

    uint8_t *cmds = (uint8_t *)malloc(hdr.sizeofcmds);
    if (!cmds) {
        return 0;
    }

    uint64_t cmds_addr = image_base + sizeof(struct mach_header_64);
    got = rm_read(task, cmds_addr, cmds, hdr.sizeofcmds);
    if (got < hdr.sizeofcmds) {
        free(cmds);
        return 0;
    }

    /* ------------------------------------------------------------------ */
    /*  3. First pass: find the __TEXT segment to compute ASLR slide.     */
    /* ------------------------------------------------------------------ */

    uint64_t text_vmaddr = 0;
    bool     text_found  = false;

    uint32_t cmd_offset = 0;

    for (uint32_t i = 0; i < hdr.ncmds; i++) {
        if (cmd_offset + sizeof(struct load_command) > hdr.sizeofcmds) {
            break;
        }

        const struct load_command *lc =
            (const struct load_command *)(cmds + cmd_offset);

        if (lc->cmdsize < sizeof(struct load_command) ||
            cmd_offset + lc->cmdsize > hdr.sizeofcmds) {
            break;
        }

        if (lc->cmd == LC_SEGMENT_64 &&
            lc->cmdsize >= sizeof(struct segment_command_64)) {

            const struct segment_command_64 *seg =
                (const struct segment_command_64 *)(cmds + cmd_offset);

            if (strncmp(seg->segname, SEG_TEXT, sizeof(seg->segname)) == 0) {
                text_vmaddr = seg->vmaddr;
                text_found  = true;
                break;
            }
        }

        cmd_offset += lc->cmdsize;
    }

    if (!text_found) {
        free(cmds);
        return 0;
    }

    int64_t slide = (int64_t)(image_base - text_vmaddr);

    /* ------------------------------------------------------------------ */
    /*  4. Second pass: find the target segment and scan it.              */
    /* ------------------------------------------------------------------ */

    uint64_t result    = 0;
    cmd_offset         = 0;

    for (uint32_t i = 0; i < hdr.ncmds; i++) {
        if (cmd_offset + sizeof(struct load_command) > hdr.sizeofcmds) {
            break;
        }

        const struct load_command *lc =
            (const struct load_command *)(cmds + cmd_offset);

        if (lc->cmdsize < sizeof(struct load_command) ||
            cmd_offset + lc->cmdsize > hdr.sizeofcmds) {
            break;
        }

        if (lc->cmd == LC_SEGMENT_64 &&
            lc->cmdsize >= sizeof(struct segment_command_64)) {

            const struct segment_command_64 *seg =
                (const struct segment_command_64 *)(cmds + cmd_offset);

            if (strncmp(seg->segname, segment_name,
                        sizeof(seg->segname)) == 0) {

                uint64_t runtime_addr = seg->vmaddr + (uint64_t)slide;
                uint64_t vmsize       = seg->vmsize;

                result = pat_scan(task, runtime_addr, vmsize,
                                  pattern, mask, pat_len);
                break;
            }
        }

        cmd_offset += lc->cmdsize;
    }

    free(cmds);
    return result;
}
