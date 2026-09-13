/*
 * remote_memory.c — Read-only remote process memory access via Mach VM APIs.
 *
 * All operations are strictly read-only.  We never write to the target
 * process address space.  Designed for jailbroken iOS / ARM64.
 */

#include "remote_memory.h"

#include <mach/mach.h>
#include <string.h>

typedef uint64_t mach_vm_address_t;
typedef uint64_t mach_vm_size_t;

extern kern_return_t mach_vm_read_overwrite(
    vm_map_t target_task,
    mach_vm_address_t address,
    mach_vm_size_t size,
    mach_vm_address_t data,
    mach_vm_size_t *outsize
);

/* ---------------------------------------------------------------------------
 * rm_task_acquire — Obtain a task port for the given PID.
 *
 * Returns MACH_PORT_NULL on any failure (permission denied, no such process,
 * etc.).  The caller must eventually pass the returned port to
 * rm_task_release().
 * ------------------------------------------------------------------------ */
mach_port_t
rm_task_acquire(pid_t pid)
{
    mach_port_t task = MACH_PORT_NULL;
    kern_return_t kr  = task_for_pid(mach_task_self(), pid, &task);

    if (kr != KERN_SUCCESS) {
        return MACH_PORT_NULL;
    }
    return task;
}

/* ---------------------------------------------------------------------------
 * rm_task_release — Release a previously-acquired task port.
 * ------------------------------------------------------------------------ */
void
rm_task_release(mach_port_t task)
{
    if (task != MACH_PORT_NULL) {
        mach_port_deallocate(mach_task_self(), task);
    }
}

/* ---------------------------------------------------------------------------
 * rm_read — Read `len` bytes from `addr` in the remote task into `buf`.
 *
 * Returns true only when the entire region was read successfully.
 * ------------------------------------------------------------------------ */
bool
rm_read(mach_port_t task, uint64_t addr, void *buf, size_t len)
{
    if (!buf || len == 0) {
        return false;
    }

    mach_vm_size_t outsize = 0;
    kern_return_t  kr      = mach_vm_read_overwrite(task,
                                                    (mach_vm_address_t)addr,
                                                    (mach_vm_size_t)len,
                                                    (mach_vm_address_t)buf,
                                                    &outsize);

    if (kr != KERN_SUCCESS) {
        return false;
    }
    if ((size_t)outsize != len) {
        return false;
    }
    return true;
}

/* ---------------------------------------------------------------------------
 * rm_read_ptr — Read an 8-byte pointer from the remote task.
 *
 * Returns 0 on failure.
 * ------------------------------------------------------------------------ */
uint64_t
rm_read_ptr(mach_port_t task, uint64_t addr)
{
    uint64_t value = 0;

    if (!rm_read(task, addr, &value, sizeof(value))) {
        return 0;
    }
    return value;
}

/* ---------------------------------------------------------------------------
 * rm_read_i32 — Read a 32-bit signed integer from the remote task.
 *
 * Sets *ok to indicate success/failure.  The caller may pass ok == NULL if
 * the status is not needed.
 * ------------------------------------------------------------------------ */
int32_t
rm_read_i32(mach_port_t task, uint64_t addr, bool *ok)
{
    int32_t value  = 0;
    bool    result = rm_read(task, addr, &value, sizeof(value));

    if (ok) {
        *ok = result;
    }
    return value;
}

/* ---------------------------------------------------------------------------
 * rm_read_u16 — Read a 16-bit unsigned integer from the remote task.
 *
 * Sets *ok to indicate success/failure.  The caller may pass ok == NULL if
 * the status is not needed.
 * ------------------------------------------------------------------------ */
uint16_t
rm_read_u16(mach_port_t task, uint64_t addr, bool *ok)
{
    uint16_t value  = 0;
    bool     result = rm_read(task, addr, &value, sizeof(value));

    if (ok) {
        *ok = result;
    }
    return value;
}

/* ---------------------------------------------------------------------------
 * rm_read_string — Read a NUL-terminated string from the remote task.
 *
 * Reads in 64-byte chunks, scanning each chunk for a NUL terminator.
 * The output buffer is always NUL-terminated (even on truncation).
 * Returns false if the very first read fails (address is unmapped, etc.).
 * ------------------------------------------------------------------------ */
bool
rm_read_string(mach_port_t task, uint64_t addr, char *buf, size_t max)
{
    if (!buf || max == 0) {
        return false;
    }

    /* Guarantee NUL-termination up front. */
    buf[0] = '\0';

    if (max == 1) {
        /* Only room for the terminator — nothing useful to read. */
        return true;
    }

    static const size_t CHUNK = 64;
    size_t              pos   = 0;  /* bytes written into buf so far */

    while (pos < max - 1) {
        char   tmp[64];
        size_t to_read = CHUNK;

        /* Don't read more than we can store (minus the terminator). */
        if (to_read > max - 1 - pos) {
            to_read = max - 1 - pos;
        }

        if (!rm_read(task, addr + pos, tmp, to_read)) {
            if (pos == 0) {
                /* First chunk failed — report failure. */
                return false;
            }
            /* Subsequent chunk failed; return what we have. */
            buf[pos] = '\0';
            return true;
        }

        /* Scan for NUL in the chunk we just read. */
        for (size_t i = 0; i < to_read; i++) {
            buf[pos] = tmp[i];
            if (tmp[i] == '\0') {
                /* Found the terminator — done. */
                return true;
            }
            pos++;
        }
    }

    /* Filled the buffer without finding a terminator — truncate. */
    buf[max - 1] = '\0';
    return true;
}

/* ---------------------------------------------------------------------------
 * rm_validate_ptr — Sanity-check a pointer value read from remote memory.
 *
 * Rejects:
 *   - NULL / very low addresses (guard pages, zero page)
 *   - Addresses outside the plausible ARM64 userspace range
 *
 * The valid range is (0x1000, 0x0001000000000000).  No alignment requirement
 * is enforced because some UE4 fields are only 4-byte-aligned.
 * ------------------------------------------------------------------------ */
bool
rm_validate_ptr(uint64_t ptr)
{
    if (ptr == 0) {
        return false;
    }
    if (ptr <= 0x1000) {
        return false;
    }
    if (ptr >= 0x0001000000000000ULL) {
        return false;
    }
    return true;
}
