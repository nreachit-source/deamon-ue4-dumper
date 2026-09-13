/*
 * ue4_reflection.h — Walk UE4 reflection structures in a remote process.
 *
 * Reads GUObjectArray and GNames/FNamePool to enumerate UClass objects
 * and their properties and functions.  All reads go through
 * remote_memory.h; nothing is ever written to the game process.
 */

#ifndef UE4_REFLECTION_H
#define UE4_REFLECTION_H

#include <mach/mach.h>
#include <stdbool.h>
#include <stdint.h>

/* Forward declarations for the JSON builder. */
typedef struct ue4_class    ue4_class_t;
typedef struct ue4_property ue4_property_t;
typedef struct ue4_function ue4_function_t;

struct ue4_property {
    char     name[256];
    char     type[256];        /* FFieldClass name, e.g. "BoolProperty" */
    int32_t  offset;
    int32_t  element_size;
    int32_t  array_dim;
    ue4_property_t *next;
};

struct ue4_function {
    char     name[256];
    uint32_t flags;
    uint16_t parms_size;
    ue4_function_t *next;
};

struct ue4_class {
    char     name[512];        /* Full path, e.g. "/Script/Engine.Actor" */
    char     super_name[512];
    int32_t  struct_size;
    ue4_property_t *properties;
    ue4_function_t *functions;
    ue4_class_t    *next;
};

/* Opaque context for the reflection walker. */
typedef struct ue4r_ctx ue4r_ctx_t;

/* Initialise the reflection walker.
 *
 *   task       — Mach task port for the game process.
 *   image_base — Runtime base address of the main executable.
 *   slide      — ASLR slide value.
 *   guobj_off  — File offset of GUObjectArray (0 = auto-detect).
 *   gnames_off — File offset of GNames/FNamePool (0 = auto-detect).
 *
 * Returns a heap-allocated context, or NULL on failure. */
ue4r_ctx_t *ue4r_init(mach_port_t task,
                       uint64_t    image_base,
                       uint64_t    slide,
                       uint64_t    guobj_off,
                       uint64_t    gnames_off);

/* Resolve an FName to a string.  `buf` must be at least `max` bytes.
 * Returns true on success. */
bool ue4r_resolve_name(ue4r_ctx_t *ctx,
                       uint64_t    fname_addr,
                       char       *buf,
                       size_t      max);

/* Walk all UClass objects and return a linked list.
 * The caller must free the result with ue4r_free_classes(). */
ue4_class_t *ue4r_walk_classes(ue4r_ctx_t *ctx);

/* Streaming class iterator: invokes `cb` for each UClass found and frees
 * class resources immediately. Peak memory footprint is <500 KB.
 * Returns the total number of classes processed. */
typedef void (*ue4r_class_callback_t)(void *userdata, const ue4_class_t *cls);
int ue4r_iterate_classes(ue4r_ctx_t *ctx, ue4r_class_callback_t cb, void *userdata);

/* Free a class list returned by ue4r_walk_classes(). */
void ue4r_free_classes(ue4_class_t *list);

/* Destroy the context. */
void ue4r_destroy(ue4r_ctx_t *ctx);

#endif /* UE4_REFLECTION_H */
