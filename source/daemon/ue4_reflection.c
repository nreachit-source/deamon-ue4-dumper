/*
 * ue4_reflection.c — UE4 reflection system introspection (read-only)
 *
 * Walks the remote process's GUObjectArray and FNamePool to enumerate
 * UClass objects and extract their property/function metadata.
 * All memory access is read-only via Mach VM APIs (rm_read_*).
 *
 * Targets: ARM64 / iOS (jailbroken), UE 4.23–4.27
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <mach/mach.h>

#include "ue4_reflection.h"
#include "remote_memory.h"
#include "pattern_scan.h"
#include "ue4_offsets.h"

/* -----------------------------------------------------------------------
 * FNamePool layout constants (common for UE 4.25–4.27, may need tuning)
 * ----------------------------------------------------------------------- */
#define FNAMEPOOL_BLOCKS_OFFSET  0x40   /* Byte offset to the block pointer array inside FNamePool */
#define FNAME_ENTRY_STRIDE       2      /* Entries are 2-byte aligned; offset counts in stride-2 units */

/* Safety caps to prevent runaway iteration */
#define MAX_OBJECTS         500000
#define MAX_LIST_WALK       1000
#define MAX_PATH_DEPTH      10
#define MAX_INIT_SCAN_OBJS  8192    /* How many objects to scan when looking for the "Class" UClass */

/* -----------------------------------------------------------------------
 * Context
 * ----------------------------------------------------------------------- */
#define NAME_CACHE_SIZE 8192
#define NAME_CACHE_MASK (NAME_CACHE_SIZE - 1)

typedef struct {
    int32_t comp_index;
    char    name[60];
} name_cache_slot_t;

struct ue4r_ctx {
    mach_port_t task;
    uint64_t    image_base;
    uint64_t    slide;
    uint64_t    guobjectarray;     /* Runtime address of GUObjectArray               */
    uint64_t    gnamepool;         /* Runtime address of FNamePool (GNames)          */
    uint64_t    gname_chunk_table; /* Resolved chunk table base pointer              */
    uint64_t    cached_chunks[32]; /* Cached chunk base pointers                     */
    uint64_t    uclass_class;      /* Address of the UClass whose name is "Class"    */
    name_cache_slot_t name_cache[NAME_CACHE_SIZE]; /* 512 KB direct-mapped cache   */
};

/* -----------------------------------------------------------------------
 * Forward declarations (private helpers)
 * ----------------------------------------------------------------------- */
static uint64_t try_find_guobjectarray(mach_port_t task, uint64_t image_base, uint64_t slide);
static uint64_t try_find_gnamepool   (mach_port_t task, uint64_t image_base, uint64_t slide);

static bool resolve_object_name(ue4r_ctx_t *ctx, uint64_t obj_addr,
                                char *buf, size_t max);
static void build_path_name    (ue4r_ctx_t *ctx, uint64_t obj_addr,
                                char *buf, size_t max, int depth);

static ue4_property_t *read_properties(ue4r_ctx_t *ctx, uint64_t class_addr);
static ue4_function_t *read_functions (ue4r_ctx_t *ctx, uint64_t class_addr);

/* =======================================================================
 * Auto-detection helpers (pattern scan stubs)
 * ======================================================================= */

/*
 * try_find_guobjectarray
 *
 * Attempt to locate GUObjectArray by scanning the __DATA segment for
 * ADRP+LDR instruction sequences that reference the global.
 * Returns the runtime address, or 0 on failure.
 */
static uint64_t try_find_guobjectarray(mach_port_t task,
                                       uint64_t image_base,
                                       uint64_t slide)
{
    /*
     * A proper implementation would use pat_scan_segment() on "__DATA"
     * with a byte pattern derived from the ADRP/ADD or ADRP/LDR pair
     * that references FUObjectArray::ObjObjects.
     *
     * For now this is a placeholder — callers should supply the offset
     * explicitly via guobj_off.
     */
    (void)task;
    (void)image_base;
    (void)slide;

    fprintf(stderr, "[ue4r] auto-detect GUObjectArray: not implemented, "
                    "please supply offset explicitly\n");
    return 0;
}

/*
 * try_find_gnamepool
 *
 * Attempt to locate FNamePool (GNames) via pattern scanning.
 * Returns the runtime address, or 0 on failure.
 */
static uint64_t try_find_gnamepool(mach_port_t task,
                                   uint64_t image_base,
                                   uint64_t slide)
{
    (void)task;
    (void)image_base;
    (void)slide;

    fprintf(stderr, "[ue4r] auto-detect FNamePool: not implemented, "
                    "please supply offset explicitly\n");
    return 0;
}

/* =======================================================================
 * FName resolution — FNamePool block-based system (UE 4.23+)
 * ======================================================================= */

bool ue4r_resolve_name(ue4r_ctx_t *ctx, uint64_t fname_addr,
                       char *buf, size_t max)
{
    if (!ctx || !buf || max == 0) return false;
    buf[0] = '\0';

    if (!rm_validate_ptr(fname_addr)) return false;

    /* 1. Read the 8-byte FName struct (ComparisonIndex + Number) */
    bool ok = false;
    int32_t comp_index = rm_read_i32(ctx->task,
                                     fname_addr + OFF_FNAME_INDEX, &ok);
    if (!ok || comp_index < 0) return false;

    int32_t number = rm_read_i32(ctx->task,
                                 fname_addr + OFF_FNAME_NUMBER, &ok);
    if (!ok) number = 0;

    /* Fast name cache hit (direct-mapped 512KB) */
    uint32_t slot = (uint32_t)comp_index & NAME_CACHE_MASK;
    if (ctx->name_cache[slot].comp_index == comp_index && ctx->name_cache[slot].name[0] != '\0') {
        if (number > 0) {
            snprintf(buf, max, "%s_%d", ctx->name_cache[slot].name, number - 1);
        } else {
            snprintf(buf, max, "%s", ctx->name_cache[slot].name);
        }
        return true;
    }

    char name_buf[1024] = {0};
    bool resolved = false;

    /* --- Strategy A: UE 4.18 TNameEntryArray (chunked array of FNameEntry*) --- */
    uint32_t chunk_idx = (uint32_t)comp_index / 16384;
    uint32_t offset_in_chunk = (uint32_t)comp_index % 16384;

    if (rm_validate_ptr(ctx->gname_chunk_table) && chunk_idx < 32) {
        if (!ctx->cached_chunks[chunk_idx]) {
            ctx->cached_chunks[chunk_idx] = rm_read_ptr(ctx->task, ctx->gname_chunk_table + (uint64_t)chunk_idx * 8);
        }
        uint64_t chunk_ptr = ctx->cached_chunks[chunk_idx];
        if (rm_validate_ptr(chunk_ptr)) {
            uint64_t entry_ptr = rm_read_ptr(ctx->task, chunk_ptr + (uint64_t)offset_in_chunk * 8);
            if (rm_validate_ptr(entry_ptr)) {
                uint16_t flags = rm_read_u16(ctx->task, entry_ptr + 8, &ok);
                bool is_wide = ok && ((flags & 1) != 0);
                if (!is_wide) {
                    if (rm_read_string(ctx->task, entry_ptr + 0x0c, name_buf, sizeof(name_buf))) {
                        if (name_buf[0] != '\0') resolved = true;
                    }
                } else {
                    uint8_t wide_buf[512] = {0};
                    if (rm_read(ctx->task, entry_ptr + 0x0c, wide_buf, sizeof(wide_buf))) {
                        for (size_t i = 0; i < sizeof(name_buf) - 1 && i < 256; i++) {
                            char c = (char)wide_buf[i * 2];
                            if (c == '\0') break;
                            name_buf[i] = c;
                        }
                        if (name_buf[0] != '\0') resolved = true;
                    }
                }
            }
        }
    }

    if (!resolved) {
        uint64_t chunk_tables[3];
        chunk_tables[0] = ctx->gnamepool;
        chunk_tables[1] = rm_read_ptr(ctx->task, ctx->gnamepool);
        chunk_tables[2] = rm_read_ptr(ctx->task, ctx->gnamepool + 8);

        for (int t = 0; t < 3 && !resolved; t++) {
            uint64_t table = chunk_tables[t];
            if (!rm_validate_ptr(table)) continue;

            uint64_t chunk_ptr = rm_read_ptr(ctx->task, table + (uint64_t)chunk_idx * 8);
            if (!rm_validate_ptr(chunk_ptr)) continue;

            uint64_t entry_ptr = rm_read_ptr(ctx->task, chunk_ptr + (uint64_t)offset_in_chunk * 8);
            if (!rm_validate_ptr(entry_ptr)) continue;

            uint16_t flags = rm_read_u16(ctx->task, entry_ptr + 8, &ok);
            bool is_wide = ok && ((flags & 1) != 0);
            if (!is_wide) {
                if (rm_read_string(ctx->task, entry_ptr + 0x0c, name_buf, sizeof(name_buf))) {
                    if (name_buf[0] != '\0') resolved = true;
                }
            } else {
                uint8_t wide_buf[512] = {0};
                if (rm_read(ctx->task, entry_ptr + 0x0c, wide_buf, sizeof(wide_buf))) {
                    for (size_t i = 0; i < sizeof(name_buf) - 1 && i < 256; i++) {
                        char c = (char)wide_buf[i * 2];
                        if (c == '\0') break;
                        name_buf[i] = c;
                    }
                    if (name_buf[0] != '\0') resolved = true;
                }
            }
        }
    }

    /* --- Strategy B: UE 4.23+ FNamePool fallback --- */
    if (!resolved) {
        uint32_t block_index = (uint32_t)comp_index >> FNAME_BLOCK_OFFSET_BITS;
        uint32_t offset_in_block = (uint32_t)comp_index & ((1u << FNAME_BLOCK_OFFSET_BITS) - 1u);
        uint64_t block_array_addr = ctx->gnamepool + FNAMEPOOL_BLOCKS_OFFSET;
        uint64_t block_ptr = rm_read_ptr(ctx->task, block_array_addr + (uint64_t)block_index * 8);
        if (rm_validate_ptr(block_ptr)) {
            uint64_t entry_addr = block_ptr + (uint64_t)offset_in_block * FNAME_ENTRY_STRIDE;
            uint16_t header = rm_read_u16(ctx->task, entry_addr, &ok);
            if (ok) {
                bool is_wide = (header & FNAMEENTRY_HEADER_WIDE_MASK) != 0;
                uint16_t len = header >> FNAMEENTRY_HEADER_LEN_SHIFT;
                if (len > 0 && len <= 1024) {
                    uint64_t str_addr = entry_addr + FNAMEENTRY_HEADER_SIZE;
                    size_t name_cap = (len < max - 1) ? len : (max - 1);
                    if (name_cap > sizeof(name_buf) - 1) name_cap = sizeof(name_buf) - 1;
                    if (!is_wide) {
                        if (rm_read(ctx->task, str_addr, name_buf, name_cap)) {
                            name_buf[name_cap] = '\0';
                            resolved = true;
                        }
                    }
                }
            }
        }
    }

    if (!resolved) return false;

    /* Populate name cache (direct-mapped 512KB) */
    {
        uint32_t slot = (uint32_t)comp_index & NAME_CACHE_MASK;
        ctx->name_cache[slot].comp_index = comp_index;
        strncpy(ctx->name_cache[slot].name, name_buf, sizeof(ctx->name_cache[slot].name) - 1);
        ctx->name_cache[slot].name[sizeof(ctx->name_cache[slot].name) - 1] = '\0';
    }

    if (number > 0) {
        char suffix[32];
        snprintf(suffix, sizeof(suffix), "_%d", number - 1);
        size_t nlen = strlen(name_buf);
        size_t slen = strlen(suffix);
        if (nlen + slen < sizeof(name_buf)) {
            memcpy(name_buf + nlen, suffix, slen + 1);
        }
    }

    snprintf(buf, max, "%s", name_buf);
    return true;
}

/* =======================================================================
 * Object name helpers
 * ======================================================================= */

/*
 * resolve_object_name — read a UObject's FName and resolve it.
 */
static bool resolve_object_name(ue4r_ctx_t *ctx, uint64_t obj_addr,
                                char *buf, size_t max)
{
    if (!rm_validate_ptr(obj_addr)) {
        if (max > 0) buf[0] = '\0';
        return false;
    }
    return ue4r_resolve_name(ctx, obj_addr + OFF_UOBJECT_NAME, buf, max);
}

/*
 * build_path_name — recursively build a UE4-style path name by following
 * the OuterPrivate chain.
 *
 * Result format mirrors UObject::GetPathName():
 *   /PackageName.ObjectName        (for classes)
 *   /PackageName/SubPkg.Object     (nested)
 *
 * The separator between a package (outermost object with no outer) and its
 * children is "."; deeper nesting also uses ".".
 */
static void build_path_name(ue4r_ctx_t *ctx, uint64_t obj_addr,
                            char *buf, size_t max, int depth)
{
    if (!buf || max == 0) return;
    buf[0] = '\0';

    if (depth > MAX_PATH_DEPTH) {
        snprintf(buf, max, "...");
        return;
    }
    if (!rm_validate_ptr(obj_addr)) {
        snprintf(buf, max, "<null>");
        return;
    }

    /* Resolve this object's own name */
    char name[256];
    if (!resolve_object_name(ctx, obj_addr, name, sizeof(name)))
        snprintf(name, sizeof(name), "<unknown>");

    /* Read OuterPrivate */
    uint64_t outer = rm_read_ptr(ctx->task, obj_addr + OFF_UOBJECT_OUTER);

    if (rm_validate_ptr(outer)) {
        /* Build the outer's path first */
        char outer_path[512];
        build_path_name(ctx, outer, outer_path, sizeof(outer_path), depth + 1);

        /*
         * Determine separator:
         *   If outer has no outer itself (i.e. it is a top-level package),
         *   use "." between the package path and this object's name.
         *   Otherwise use "." as well (matches GetPathName behaviour for
         *   sub-objects).
         */
        snprintf(buf, max, "%s.%s", outer_path, name);
    } else {
        /* No outer — this is a top-level package */
        if (name[0] == '/') {
            snprintf(buf, max, "%s", name);
        } else {
            snprintf(buf, max, "/%s", name);
        }
    }
}

/* =======================================================================
 * Property & Function list readers
 * ======================================================================= */

/*
 * read_properties — walk the FProperty linked list starting at
 * UStruct::ChildProperties.
 *
 * FProperty uses the FField layout (OFF_FFIELD_*) rather than UObject.
 */
static ue4_property_t *read_properties(ue4r_ctx_t *ctx, uint64_t class_addr)
{
    /* UE 4.18 has properties in Children (+0x38). UE 4.25 has ChildProperties (+0x50) or Children (+0x48). */
    uint64_t prop_ptr = rm_read_ptr(ctx->task, class_addr + OFF_USTRUCT_CHILDREN);
    if (!rm_validate_ptr(prop_ptr)) {
        prop_ptr = rm_read_ptr(ctx->task, class_addr + OFF_USTRUCT_CHILD_PROPS);
    }
    if (!rm_validate_ptr(prop_ptr)) {
        prop_ptr = rm_read_ptr(ctx->task, class_addr + OFF_USTRUCT_CHILDREN_425);
    }

    ue4_property_t *head = NULL;
    ue4_property_t *tail = NULL;
    int count = 0;

    while (rm_validate_ptr(prop_ptr) && count < MAX_LIST_WALK) {
        /* Check if UProperty (UE 4.18) or FProperty (UE 4.25+) */
        uint64_t uclass = rm_read_ptr(ctx->task, prop_ptr + OFF_UOBJECT_CLASS);
        char type_buf[256] = {0};
        bool is_uproperty = false;
        bool is_ufield = false;
        if (rm_validate_ptr(uclass)) {
            is_ufield = true;
            if (ue4r_resolve_name(ctx, uclass + OFF_UOBJECT_NAME, type_buf, sizeof(type_buf))) {
                if (strstr(type_buf, "Property")) {
                    is_uproperty = true;
                }
            }
        }

        if (is_uproperty) {
            ue4_property_t *p = calloc(1, sizeof(*p));
            if (!p) break;

            /* --- UProperty path (UE 4.18) --- */
            snprintf(p->type, sizeof(p->type), "%s", type_buf);
            if (!ue4r_resolve_name(ctx, prop_ptr + OFF_UOBJECT_NAME, p->name, sizeof(p->name))) {
                snprintf(p->name, sizeof(p->name), "<unknown>");
            }
            bool ok;
            p->array_dim = rm_read_i32(ctx->task, prop_ptr + 0x30, &ok);
            p->element_size = rm_read_i32(ctx->task, prop_ptr + 0x34, &ok);
            p->offset = rm_read_i32(ctx->task, prop_ptr + 0x44, &ok);
            if (p->offset == 0) p->offset = rm_read_i32(ctx->task, prop_ptr + 0x4c, &ok);

            prop_ptr = rm_read_ptr(ctx->task, prop_ptr + OFF_UFIELD_NEXT);

            p->next = NULL;
            if (tail) { tail->next = p; tail = p; }
            else      { head = tail = p; }
            count++;
        } else if (is_ufield) {
            /* Skip non-property UField (e.g. UFunction in Children linked list) */
            prop_ptr = rm_read_ptr(ctx->task, prop_ptr + OFF_UFIELD_NEXT);
        } else {
            /* --- FProperty path (UE 4.25+) --- */
            ue4_property_t *p = calloc(1, sizeof(*p));
            if (!p) break;

            if (!ue4r_resolve_name(ctx, prop_ptr + OFF_FFIELD_NAME,
                                   p->name, sizeof(p->name))) {
                snprintf(p->name, sizeof(p->name), "<unknown>");
            }

            uint64_t fclass = rm_read_ptr(ctx->task,
                                          prop_ptr + OFF_FFIELD_CLASS);
            if (rm_validate_ptr(fclass)) {
                if (!ue4r_resolve_name(ctx, fclass + OFF_FFIELDCLASS_NAME,
                                       p->type, sizeof(p->type))) {
                    snprintf(p->type, sizeof(p->type), "Unknown");
                }
            } else {
                snprintf(p->type, sizeof(p->type), "Unknown");
            }

            bool ok;
            p->array_dim    = rm_read_i32(ctx->task,
                                          prop_ptr + OFF_FPROP_ARRAY_DIM, &ok);
            if (!ok) p->array_dim = 0;

            p->element_size = rm_read_i32(ctx->task,
                                          prop_ptr + OFF_FPROP_ELEMENT_SIZE, &ok);
            if (!ok) p->element_size = 0;

            p->offset       = rm_read_i32(ctx->task,
                                          prop_ptr + OFF_FPROP_OFFSET, &ok);
            if (!ok) p->offset = 0;

            prop_ptr = rm_read_ptr(ctx->task, prop_ptr + OFF_FFIELD_NEXT);

            p->next = NULL;
            if (tail) { tail->next = p; tail = p; }
            else      { head = tail = p; }
            count++;
        }
    }

    return head;
}

/*
 * read_functions — walk the UField linked list starting at
 * UStruct::Children, filtering for UFunction objects.
 *
 * Children are UField subclasses linked via UField::Next (OFF_UFIELD_NEXT).
 * We identify UFunctions by checking whether the child's ClassPrivate has
 * the name "Function".
 */
static ue4_function_t *read_functions(ue4r_ctx_t *ctx, uint64_t class_addr)
{
    uint64_t child = rm_read_ptr(ctx->task,
                                 class_addr + OFF_USTRUCT_CHILDREN);
    if (!rm_validate_ptr(child)) {
        child = rm_read_ptr(ctx->task, class_addr + OFF_USTRUCT_CHILDREN_425);
    }

    ue4_function_t *head = NULL;
    ue4_function_t *tail = NULL;
    int count = 0;

    while (rm_validate_ptr(child) && count < MAX_LIST_WALK) {
        /* Read the child's ClassPrivate to determine its type */
        uint64_t child_class = rm_read_ptr(ctx->task,
                                           child + OFF_UOBJECT_CLASS);
        char class_name[256] = {0};

        if (rm_validate_ptr(child_class)) {
            resolve_object_name(ctx, child_class, class_name,
                                sizeof(class_name));
        }

        if (strcmp(class_name, "Function") == 0) {
            ue4_function_t *f = calloc(1, sizeof(*f));
            if (!f) break;

            /* Name */
            if (!resolve_object_name(ctx, child, f->name, sizeof(f->name)))
                snprintf(f->name, sizeof(f->name), "<unknown>");

            /* FunctionFlags (uint32) */
            bool ok;
            f->flags = (uint32_t)rm_read_i32(ctx->task,
                                             child + OFF_UFUNC_FLAGS_418, &ok);
            if (!ok || f->flags == 0) {
                f->flags = (uint32_t)rm_read_i32(ctx->task,
                                                 child + OFF_UFUNC_FLAGS_425, &ok);
            }
            if (!ok) f->flags = 0;

            /* ParmsSize (uint16) */
            f->parms_size = rm_read_u16(ctx->task,
                                        child + OFF_UFUNC_PARMS_SIZE_418, &ok);
            if (!ok || f->parms_size == 0) {
                f->parms_size = rm_read_u16(ctx->task,
                                            child + OFF_UFUNC_PARMS_SIZE_425, &ok);
            }
            if (!ok) f->parms_size = 0;

            /* Append */
            f->next = NULL;
            if (tail) { tail->next = f; tail = f; }
            else      { head = tail = f; }
        }

        /* Next child in the UField linked list */
        child = rm_read_ptr(ctx->task, child + OFF_UFIELD_NEXT);
        count++;
    }

    return head;
}

/* =======================================================================
 * GUObjectArray iteration helpers
 * ======================================================================= */

/*
 * read_object_from_array — read the UObject* at a given linear index in the
 * chunked FUObjectArray.
 *
 * The chunked array stores an array of chunk pointers at
 * guobjectarray + OFF_GUOBJ_CHUNKED + OFF_CHUNKED_OBJECTS.
 * Each chunk holds ELEMENTS_PER_CHUNK items of size FUOBJECTITEM_SIZE.
 */
static uint64_t read_object_from_array(ue4r_ctx_t *ctx,
                                       uint64_t   objects_ptr,
                                       int32_t    index)
{
    /* 1. Try flat FUObjectItem array (Item = objects_ptr + index * 24) */
    uint64_t flat_item = objects_ptr + (uint64_t)index * FUOBJECTITEM_SIZE;
    uint64_t flat_obj = rm_read_ptr(ctx->task, flat_item + FUOBJECTITEM_OBJECT);
    if (rm_validate_ptr(flat_obj)) {
        return flat_obj;
    }

    /* 2. Fallback: chunked array */
    int32_t chunk_index  = index / ELEMENTS_PER_CHUNK;
    int32_t within_chunk = index % ELEMENTS_PER_CHUNK;

    uint64_t chunk_ptr = rm_read_ptr(ctx->task,
                                     objects_ptr + (uint64_t)chunk_index * 8);
    if (rm_validate_ptr(chunk_ptr)) {
        uint64_t item_addr = chunk_ptr +
                             (uint64_t)within_chunk * FUOBJECTITEM_SIZE +
                             FUOBJECTITEM_OBJECT;
        return rm_read_ptr(ctx->task, item_addr);
    }
    return 0;
}

static uint64_t get_objects_info(ue4r_ctx_t *ctx, int32_t *out_count)
{
    bool ok = false;
    /* Try +0x10 (ObjObjects) */
    uint64_t chunked = ctx->guobjectarray + OFF_GUOBJ_CHUNKED;
    uint64_t ptr = rm_read_ptr(ctx->task, chunked + OFF_CHUNKED_OBJECTS);
    int32_t count = rm_read_i32(ctx->task, chunked + OFF_CHUNKED_NUM_ELEMS, &ok);
    if (rm_validate_ptr(ptr) && ok && count > 0) {
        if (out_count) *out_count = count;
        return ptr;
    }

    /* Try count at chunked + 0x14 */
    count = rm_read_i32(ctx->task, chunked + 0x14, &ok);
    if (rm_validate_ptr(ptr) && ok && count > 0) {
        if (out_count) *out_count = count;
        return ptr;
    }

    /* Try guobjectarray + 0x10 and + 0x1c */
    ptr = rm_read_ptr(ctx->task, ctx->guobjectarray + 0x10);
    count = rm_read_i32(ctx->task, ctx->guobjectarray + 0x1c, &ok);
    if (rm_validate_ptr(ptr) && ok && count > 0) {
        if (out_count) *out_count = count;
        return ptr;
    }

    /* Try guobjectarray + 0x00 and + 0x14 */
    ptr = rm_read_ptr(ctx->task, ctx->guobjectarray);
    count = rm_read_i32(ctx->task, ctx->guobjectarray + 0x14, &ok);
    if (rm_validate_ptr(ptr) && ok && count > 0) {
        if (out_count) *out_count = count;
        return ptr;
    }

    if (rm_validate_ptr(ptr)) {
        if (out_count) *out_count = 100000;
        return ptr;
    }
    return 0;
}

/* =======================================================================
 * Initialization
 * ======================================================================= */

/*
 * find_uclass_class — scan the first few hundred GUObjectArray entries to
 * locate the UClass object whose name is "Class" and whose ClassPrivate
 * points to itself (i.e. the meta-class).
 */
static uint64_t find_uclass_class(ue4r_ctx_t *ctx)
{
    int32_t num_elems = 0;
    uint64_t objects_ptr = get_objects_info(ctx, &num_elems);
    if (!objects_ptr || num_elems <= 0) {
        fprintf(stderr, "[ue4r] failed to read GUObjectArray objects/count\n");
        return 0;
    }

    int32_t scan_limit = (num_elems < MAX_INIT_SCAN_OBJS)
                         ? num_elems : MAX_INIT_SCAN_OBJS;

    for (int32_t i = 0; i < scan_limit; i++) {
        uint64_t obj = read_object_from_array(ctx, objects_ptr, i);
        if (!rm_validate_ptr(obj)) continue;

        uint64_t cls = rm_read_ptr(ctx->task, obj + OFF_UOBJECT_CLASS);

        char name[256] = {0};
        if (resolve_object_name(ctx, obj, name, sizeof(name))) {
            if (strcmp(name, "Class") == 0) {
                if (cls == obj) return obj;
                char cls_name[256] = {0};
                if (resolve_object_name(ctx, cls, cls_name, sizeof(cls_name)) &&
                    strcmp(cls_name, "Class") == 0) {
                    return cls;
                }
            }
        }
    }

    return 0;
}

ue4r_ctx_t *ue4r_init(mach_port_t task,
                      uint64_t    image_base,
                      uint64_t    slide,
                      uint64_t    guobj_off,
                      uint64_t    gnames_off)
{
    ue4r_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) {
        fprintf(stderr, "[ue4r] allocation failed\n");
        return NULL;
    }

    ctx->task       = task;
    ctx->image_base = image_base;
    ctx->slide      = slide;

    for (int i = 0; i < NAME_CACHE_SIZE; i++) {
        ctx->name_cache[i].comp_index = -1;
    }

    /* ----- Resolve GUObjectArray address ----- */
    if (guobj_off != 0) {
        if (guobj_off >= 0x100000000ULL) {
            ctx->guobjectarray = guobj_off + slide;
        } else {
            ctx->guobjectarray = image_base + guobj_off;
        }
    } else {
        ctx->guobjectarray = try_find_guobjectarray(task, image_base, slide);
        if (ctx->guobjectarray == 0) {
            /* Fallback to known ShadowTrackerExtra pre-ASLR offset */
            ctx->guobjectarray = image_base + 0x0aad3898ULL;
        }
    }

    /* ----- Resolve FNamePool / GNames address ----- */
    if (gnames_off != 0) {
        if (gnames_off >= 0x100000000ULL) {
            ctx->gnamepool = gnames_off + slide;
        } else {
            ctx->gnamepool = image_base + gnames_off;
        }
    } else {
        ctx->gnamepool = try_find_gnamepool(task, image_base, slide);
        if (ctx->gnamepool == 0) {
            /* Fallback to known ShadowTrackerExtra pre-ASLR offset */
            ctx->gnamepool = image_base + 0x0a898170ULL;
        }
    }

    /* ----- Resolve GNames chunk table (dereference chain) ----- */
    uint32_t val0 = 0;
    rm_read(task, ctx->gnamepool, &val0, sizeof(val0));
    uint32_t deref_count = (val0 >= 100) ? (val0 - 100) / 3 : 0;
    uint64_t ptr = rm_read_ptr(task, ctx->gnamepool + 8);
    if (!ptr) ptr = rm_read_ptr(task, ctx->gnamepool);
    for (uint32_t i = 0; i < deref_count; i++) {
        if (!rm_validate_ptr(ptr)) break;
        ptr = rm_read_ptr(task, ptr);
    }
    ctx->gname_chunk_table = ptr;
    fprintf(stderr, "[ue4r] GNames chunk table: 0x%llx (val0=0x%x, derefs=%u)\n",
            (unsigned long long)ctx->gname_chunk_table, val0, deref_count);

    /* ----- Locate the UClass meta-class ("Class" whose class is itself) --- */
    ctx->uclass_class = find_uclass_class(ctx);
    if (ctx->uclass_class == 0) {
        fprintf(stderr, "[ue4r] warning: UClass(\"Class\") not found in init scan; will use dynamic lookup\n");
    }

    fprintf(stderr, "[ue4r] init OK  guobj=0x%llx  gnames=0x%llx  "
                    "uclass=0x%llx\n",
            (unsigned long long)ctx->guobjectarray,
            (unsigned long long)ctx->gnamepool,
            (unsigned long long)ctx->uclass_class);

    return ctx;
}

/* =======================================================================
 * Class enumeration
 * ======================================================================= */

ue4_class_t *ue4r_walk_classes(ue4r_ctx_t *ctx)
{
    if (!ctx) return NULL;

    int32_t num_elems = 0;
    uint64_t objects_ptr = get_objects_info(ctx, &num_elems);
    if (!objects_ptr || num_elems <= 0) {
        fprintf(stderr, "[ue4r] walk_classes: bad Objects pointer or count\n");
        return NULL;
    }
    if (num_elems > MAX_OBJECTS) num_elems = MAX_OBJECTS;

    ue4_class_t *head = NULL;
    ue4_class_t *tail = NULL;

    typedef struct {
        uint64_t uobject;
        int32_t  flags;
        int32_t  cluster_root_index;
        int32_t  serial_number;
        int32_t  pad;
    } fu_object_item_t;

    fu_object_item_t batch[512];
    const int batch_size = 512;

    for (int32_t b = 0; b < num_elems; b += batch_size) {
        int32_t to_read = (num_elems - b < batch_size) ? (num_elems - b) : batch_size;
        bool batch_ok = rm_read(ctx->task, objects_ptr + (uint64_t)b * sizeof(fu_object_item_t),
                                batch, (size_t)to_read * sizeof(fu_object_item_t));

        for (int32_t k = 0; k < to_read; k++) {
            uint64_t obj = 0;
            if (batch_ok) {
                obj = batch[k].uobject;
            } else {
                obj = read_object_from_array(ctx, objects_ptr, b + k);
            }
            if (!rm_validate_ptr(obj)) continue;

            /* Is this object a UClass? */
            uint64_t cls = rm_read_ptr(ctx->task, obj + OFF_UOBJECT_CLASS);
            bool is_uclass = false;
            if (ctx->uclass_class != 0 && cls == ctx->uclass_class) {
                is_uclass = true;
            } else if (rm_validate_ptr(cls)) {
                char cls_name[256] = {0};
                if (resolve_object_name(ctx, cls, cls_name, sizeof(cls_name))) {
                    if (strcmp(cls_name, "Class") == 0) {
                        is_uclass = true;
                        if (ctx->uclass_class == 0) ctx->uclass_class = cls;
                    }
                }
            }
            if (!is_uclass) continue;

            /* ---- Build a ue4_class_t for this UClass ---- */
            ue4_class_t *c = calloc(1, sizeof(*c));
            if (!c) break;

            /* Full path name (e.g. "/Script/Engine.Actor") */
            build_path_name(ctx, obj, c->name, sizeof(c->name), 0);

            /* SuperStruct path name */
            uint64_t super_ptr = rm_read_ptr(ctx->task, obj + OFF_USTRUCT_SUPER);
            if (!rm_validate_ptr(super_ptr)) {
                super_ptr = rm_read_ptr(ctx->task, obj + OFF_USTRUCT_SUPER_425);
            }
            if (rm_validate_ptr(super_ptr)) {
                build_path_name(ctx, super_ptr, c->super_name,
                                sizeof(c->super_name), 0);
            } else {
                c->super_name[0] = '\0';
            }

            /* PropertiesSize (int32) */
            bool ok = false;
            c->struct_size = rm_read_i32(ctx->task, obj + OFF_USTRUCT_PROPS_SIZE, &ok);
            if (!ok || c->struct_size == 0) {
                c->struct_size = rm_read_i32(ctx->task, obj + OFF_USTRUCT_PROPS_SIZE_425, &ok);
            }
            if (!ok) c->struct_size = 0;

            /* Property list (FProperty chain via ChildProperties) */
            c->properties = read_properties(ctx, obj);

            /* Function list (UField chain via Children, filtered) */
            c->functions = read_functions(ctx, obj);

            /* Append to result list */
            c->next = NULL;
            if (tail) { tail->next = c; tail = c; }
            else      { head = tail = c; }
        }
    }

    return head;
}

int ue4r_iterate_classes(ue4r_ctx_t *ctx, ue4r_class_callback_t cb, void *userdata)
{
    if (!ctx || !cb) return -1;

    int32_t num_elems = 0;
    uint64_t objects_ptr = get_objects_info(ctx, &num_elems);
    if (!objects_ptr || num_elems <= 0) {
        fprintf(stderr, "[ue4r] iterate_classes: bad Objects pointer or count\n");
        return -1;
    }
    if (num_elems > MAX_OBJECTS) num_elems = MAX_OBJECTS;

    typedef struct {
        uint64_t uobject;
        int32_t  flags;
        int32_t  cluster_root_index;
        int32_t  serial_number;
        int32_t  pad;
    } fu_object_item_t;

    fu_object_item_t batch[512];
    const int batch_size = 512;
    int class_count = 0;

    for (int32_t b = 0; b < num_elems; b += batch_size) {
        int32_t to_read = (num_elems - b < batch_size) ? (num_elems - b) : batch_size;
        bool batch_ok = rm_read(ctx->task, objects_ptr + (uint64_t)b * sizeof(fu_object_item_t),
                                batch, (size_t)to_read * sizeof(fu_object_item_t));

        for (int32_t k = 0; k < to_read; k++) {
            uint64_t obj = 0;
            if (batch_ok) {
                obj = batch[k].uobject;
            } else {
                obj = read_object_from_array(ctx, objects_ptr, b + k);
            }
            if (!rm_validate_ptr(obj)) continue;

            /* Is this object a UClass? */
            uint64_t cls = rm_read_ptr(ctx->task, obj + OFF_UOBJECT_CLASS);
            bool is_uclass = false;
            if (ctx->uclass_class != 0 && cls == ctx->uclass_class) {
                is_uclass = true;
            } else if (rm_validate_ptr(cls)) {
                char cls_name[256] = {0};
                if (resolve_object_name(ctx, cls, cls_name, sizeof(cls_name))) {
                    if (strcmp(cls_name, "Class") == 0) {
                        is_uclass = true;
                        if (ctx->uclass_class == 0) ctx->uclass_class = cls;
                    }
                }
            }
            if (!is_uclass) continue;

            /* ---- Build a stack-allocated ue4_class_t for this UClass ---- */
            ue4_class_t c;
            memset(&c, 0, sizeof(c));

            build_path_name(ctx, obj, c.name, sizeof(c.name), 0);

            uint64_t super_ptr = rm_read_ptr(ctx->task, obj + OFF_USTRUCT_SUPER);
            if (!rm_validate_ptr(super_ptr)) {
                super_ptr = rm_read_ptr(ctx->task, obj + OFF_USTRUCT_SUPER_425);
            }
            if (rm_validate_ptr(super_ptr)) {
                build_path_name(ctx, super_ptr, c.super_name, sizeof(c.super_name), 0);
            }

            bool ok = false;
            c.struct_size = rm_read_i32(ctx->task, obj + OFF_USTRUCT_PROPS_SIZE, &ok);
            if (!ok || c.struct_size == 0) {
                c.struct_size = rm_read_i32(ctx->task, obj + OFF_USTRUCT_PROPS_SIZE_425, &ok);
            }
            if (!ok) c.struct_size = 0;

            c.properties = read_properties(ctx, obj);
            c.functions = read_functions(ctx, obj);

            /* Invoke callback to stream to JSON */
            cb(userdata, &c);
            class_count++;

            /* Free properties and functions immediately (constant heap usage < 500 KB) */
            ue4_property_t *p = c.properties;
            while (p) {
                ue4_property_t *np = p->next;
                free(p);
                p = np;
            }
            ue4_function_t *f = c.functions;
            while (f) {
                ue4_function_t *nf = f->next;
                free(f);
                f = nf;
            }
        }
    }

    return class_count;
}

/* =======================================================================
 * Cleanup
 * ======================================================================= */

void ue4r_free_classes(ue4_class_t *list)
{
    while (list) {
        ue4_class_t *next_class = list->next;

        /* Free property chain */
        ue4_property_t *p = list->properties;
        while (p) {
            ue4_property_t *np = p->next;
            free(p);
            p = np;
        }

        /* Free function chain */
        ue4_function_t *f = list->functions;
        while (f) {
            ue4_function_t *nf = f->next;
            free(f);
            f = nf;
        }

        free(list);
        list = next_class;
    }
}

void ue4r_destroy(ue4r_ctx_t *ctx)
{
    if (ctx) free(ctx);
}
