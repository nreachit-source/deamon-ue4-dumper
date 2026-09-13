/* ue4_sdk.c – Top-level orchestrator for UE4 reflection dump
 *
 * Acquires a Mach task port, resolves ASLR, walks UE4 reflection
 * metadata, and serialises the result to JSON.  All operations on the
 * target process are strictly read-only.
 *
 * An optional config file (UE4_SDK_CONFIG_PATH) can supply manual
 * offsets for GUObjectArray / GNamePool when auto-detection isn't
 * available.
 */

#include "ue4_sdk.h"

#include "remote_memory.h"
#include "aslr_slide.h"
#include "ue4_reflection.h"
#include "ue4_json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <time.h>
#include <errno.h>
#include <mach/mach.h>

/* ------------------------------------------------------------------ */
/*  Logging helper                                                     */
/* ------------------------------------------------------------------ */

static FILE *g_log = NULL;

static void sdk_log(const char *fmt, ...)
{
    if (!g_log) return;

    /* Timestamp prefix */
    time_t now = time(NULL);
    struct tm tm_buf;
    struct tm *tm = localtime_r(&now, &tm_buf);
    if (tm) {
        char ts[32];
        strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", tm);
        fprintf(g_log, "[%s] ", ts);
    }

    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);

    fprintf(g_log, "\n");
    fflush(g_log);
}

/* ------------------------------------------------------------------ */
/*  Config file parser                                                 */
/* ------------------------------------------------------------------ */

static void parse_config(uint64_t *guobj_out, uint64_t *gnames_out)
{
    *guobj_out  = 0;
    *gnames_out = 0;

    FILE *fp = fopen(UE4_SDK_CONFIG_PATH, "r");
    if (!fp) {
        sdk_log("config: %s not found, using auto-detect",
                UE4_SDK_CONFIG_PATH);
        return;
    }

    char line[512];
    while (fgets(line, sizeof(line), fp)) {
        /* Strip leading whitespace */
        char *p = line;
        while (*p == ' ' || *p == '\t') ++p;

        /* Skip blank lines and comments */
        if (*p == '\0' || *p == '\n' || *p == '#') continue;

        char key[128] = {0};
        uint64_t val  = 0;

        if (sscanf(p, "%127[^=]=%llx", key, &val) == 2 ||
            sscanf(p, "%127[^=]=0x%llx", key, &val) == 2) {

            /* Trim trailing whitespace from key */
            size_t klen = strlen(key);
            while (klen > 0 && (key[klen - 1] == ' ' ||
                                key[klen - 1] == '\t')) {
                key[--klen] = '\0';
            }

            if (strcmp(key, "guobjectarray") == 0) {
                *guobj_out = val;
                sdk_log("config: guobjectarray = 0x%llx", val);
            } else if (strcmp(key, "gnamepool") == 0) {
                *gnames_out = val;
                sdk_log("config: gnamepool = 0x%llx", val);
            } else {
                sdk_log("config: unknown key '%s'", key);
            }
        }
    }

    fclose(fp);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

int ue4_sdk_generate(pid_t target_pid)
{
    int result = -1;

    /* ---- open log ------------------------------------------------ */
    g_log = fopen(UE4_SDK_LOG_PATH, "a");
    if (!g_log) {
        /* Best-effort: try stderr so we don't lose the message. */
        fprintf(stderr, "ue4_sdk: cannot open log %s: %s\n",
                UE4_SDK_LOG_PATH, strerror(errno));
        return -1;
    }

    sdk_log("========== SDK generate start (pid %d) ==========",
            (int)target_pid);

    /* ---- read config --------------------------------------------- */
    uint64_t guobj_off  = 0;
    uint64_t gnames_off = 0;
    parse_config(&guobj_off, &gnames_off);

    /* ---- acquire task port --------------------------------------- */
    mach_port_t task = rm_task_acquire(target_pid);
    if (task == MACH_PORT_NULL) {
        sdk_log("ERROR: failed to acquire task port for pid %d",
                (int)target_pid);
        goto out_log;
    }
    sdk_log("task port acquired: 0x%x", task);

    /* ---- resolve ASLR -------------------------------------------- */
    aslr_result_t aslr = aslr_get_slide(task);
    if (!aslr.found) {
        sdk_log("ERROR: ASLR slide resolution failed");
        goto out_task;
    }
    sdk_log("Base: 0x%llx, Slide: 0x%llx",
            (unsigned long long)aslr.base,
            (unsigned long long)aslr.slide);

    /* ---- init reflection walker ---------------------------------- */
    ue4r_ctx_t *ctx = ue4r_init(task, aslr.base, aslr.slide,
                                guobj_off, gnames_off);
    if (!ctx) {
        sdk_log("ERROR: ue4r_init failed");
        goto out_task;
    }
    sdk_log("reflection context initialised");

    /* ---- stream classes to JSON (< 500 KB peak memory) ---------- */
    ue4j_writer_t *writer = ue4j_writer_open(UE4_SDK_OUTPUT_PATH);
    if (!writer) {
        sdk_log("ERROR: failed to open JSON writer for %s", UE4_SDK_OUTPUT_PATH);
        goto out_ctx;
    }

    int count = ue4r_iterate_classes(ctx, (ue4r_class_callback_t)ue4j_writer_write_class, writer);
    int written = ue4j_writer_close(writer);

    sdk_log("classes found: %d", count);
    if (count > 0 && written == count) {
        sdk_log("JSON written to %s", UE4_SDK_OUTPUT_PATH);
        result = 0;
    } else {
        sdk_log("ERROR: class iteration or JSON write failed (found=%d, written=%d)", count, written);
        result = -1;
    }

out_ctx:
    ue4r_destroy(ctx);

out_task:
    rm_task_release(task);

out_log:
    sdk_log("========== SDK generate end (result %d) ==========", result);
    fclose(g_log);
    g_log = NULL;

    return result;
}
