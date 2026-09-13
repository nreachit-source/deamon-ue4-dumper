/*
 * remote_memory.h — Safe remote process memory reading via Mach VM.
 *
 * All reads are performed with mach_vm_read_overwrite().  Every
 * pointer is validated before use.  This module never writes to
 * the target process.
 */

#ifndef REMOTE_MEMORY_H
#define REMOTE_MEMORY_H

#include <mach/mach.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/* Acquire a Mach task port for the given PID.  Returns MACH_PORT_NULL
 * on failure.  The caller must deallocate the port when done. */
mach_port_t rm_task_acquire(pid_t pid);

/* Release a previously acquired task port. */
void rm_task_release(mach_port_t task);

/* Read `len` bytes from `addr` in the remote task into `buf`.
 * Returns true on success, false on any failure. */
bool rm_read(mach_port_t task, uint64_t addr, void *buf, size_t len);

/* Read a single 64-bit pointer from the remote task.
 * Returns 0 on failure (and 0 is also a valid "null pointer" value). */
uint64_t rm_read_ptr(mach_port_t task, uint64_t addr);

/* Read a 32-bit integer from the remote task.
 * Returns 0 on failure. `ok` is set to true/false if non-NULL. */
int32_t rm_read_i32(mach_port_t task, uint64_t addr, bool *ok);

/* Read a 16-bit integer from the remote task. */
uint16_t rm_read_u16(mach_port_t task, uint64_t addr, bool *ok);

/* Read a NUL-terminated string from the remote task.
 * At most `max - 1` bytes are read; the result is always NUL-terminated.
 * Returns true if at least one byte was read. */
bool rm_read_string(mach_port_t task, uint64_t addr, char *buf, size_t max);

/* Validate that a pointer value looks plausible for an ARM64 userspace
 * address.  Checks alignment, non-null, and address range. */
bool rm_validate_ptr(uint64_t ptr);

#endif /* REMOTE_MEMORY_H */
