/* ue4_json.c – Minimal JSON serializer for UE4 reflection metadata
 *
 * Produces a JSON file describing class layout, properties, and functions
 * harvested from a remote UE4 process.  Pure C11, no external JSON library.
 *
 * Output is chown'd to mobile (501:501) so Filza / Safari can read it.
 */

#include "ue4_json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

/* ------------------------------------------------------------------ */
/*  JSON string escaping                                               */
/* ------------------------------------------------------------------ */

static int json_write_escaped(FILE *fp, const char *s)
{
    if (!fp || !s) return -1;

    if (fputc('"', fp) == EOF) return -1;

    for (const char *p = s; *p; ++p) {
        switch (*p) {
        case '"':  if (fputs("\\\"", fp) == EOF) return -1; break;
        case '\\': if (fputs("\\\\", fp) == EOF) return -1; break;
        case '\b': if (fputs("\\b",  fp) == EOF) return -1; break;
        case '\f': if (fputs("\\f",  fp) == EOF) return -1; break;
        case '\n': if (fputs("\\n",  fp) == EOF) return -1; break;
        case '\r': if (fputs("\\r",  fp) == EOF) return -1; break;
        case '\t': if (fputs("\\t",  fp) == EOF) return -1; break;
        default:
            if ((unsigned char)*p < 0x20) {
                /* Control character – emit as \u00XX */
                if (fprintf(fp, "\\u%04x", (unsigned char)*p) < 0)
                    return -1;
            } else {
                if (fputc(*p, fp) == EOF) return -1;
            }
            break;
        }
    }

    if (fputc('"', fp) == EOF) return -1;
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Property / function / class writers                                */
/* ------------------------------------------------------------------ */

static int json_write_property(FILE *fp, const ue4_property_t *prop,
                               int last)
{
    if (!fp || !prop) return -1;

    fprintf(fp, "        { ");

    fprintf(fp, "\"name\": ");
    if (json_write_escaped(fp, prop->name) < 0) return -1;

    fprintf(fp, ", \"type\": ");
    if (json_write_escaped(fp, prop->type) < 0) return -1;

    fprintf(fp, ", \"offset\": %d",       prop->offset);
    fprintf(fp, ", \"element_size\": %d",  prop->element_size);
    fprintf(fp, ", \"array_dim\": %d",     prop->array_dim);

    fprintf(fp, " }%s\n", last ? "" : ",");
    return 0;
}

static int json_write_function(FILE *fp, const ue4_function_t *fn,
                               int last)
{
    if (!fp || !fn) return -1;

    fprintf(fp, "        { ");

    fprintf(fp, "\"name\": ");
    if (json_write_escaped(fp, fn->name) < 0) return -1;

    fprintf(fp, ", \"flags\": %u",          (unsigned)fn->flags);
    fprintf(fp, ", \"parameter_size\": %u",  (unsigned)fn->parms_size);

    fprintf(fp, " }%s\n", last ? "" : ",");
    return 0;
}

static int json_write_class(FILE *fp, const ue4_class_t *cls, int last)
{
    if (!fp || !cls) return -1;

    fprintf(fp, "    {\n");

    /* name / super / size */
    fprintf(fp, "      \"name\": ");
    if (json_write_escaped(fp, cls->name) < 0) return -1;
    fprintf(fp, ",\n");

    fprintf(fp, "      \"super\": ");
    if (json_write_escaped(fp, cls->super_name) < 0) return -1;
    fprintf(fp, ",\n");

    fprintf(fp, "      \"size\": %d,\n", cls->struct_size);

    /* properties */
    fprintf(fp, "      \"properties\": [\n");
    for (const ue4_property_t *p = cls->properties; p; p = p->next) {
        if (json_write_property(fp, p, p->next == NULL) < 0)
            return -1;
    }
    fprintf(fp, "      ],\n");

    /* functions */
    fprintf(fp, "      \"functions\": [\n");
    for (const ue4_function_t *f = cls->functions; f; f = f->next) {
        if (json_write_function(fp, f, f->next == NULL) < 0)
            return -1;
    }
    fprintf(fp, "      ]\n");

    fprintf(fp, "    }%s\n", last ? "" : ",");
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

int ue4j_write(const char *path, const ue4_class_t *classes)
{
    if (!path) return -1;

    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) return -1;

    FILE *fp = fdopen(fd, "w");
    if (!fp) {
        close(fd);
        return -1;
    }

    /* header */
    fprintf(fp, "{\n");
    fprintf(fp, "  \"format\": \"ue4-reflection-schema-v1\",\n");
    fprintf(fp, "  \"scope\": \"type metadata only; no object values\",\n");
    fprintf(fp, "  \"classes\": [\n");

    /* classes */
    for (const ue4_class_t *c = classes; c; c = c->next) {
        if (json_write_class(fp, c, c->next == NULL) < 0) {
            fclose(fp);        /* also closes fd */
            return -1;
        }
    }

    fprintf(fp, "  ]\n");
    fprintf(fp, "}\n");

    int flush_err = fflush(fp);
    fclose(fp);                /* closes fd as well */

    if (flush_err != 0) return -1;

    /* Make readable by the mobile user in case we're running as root. */
    chown(path, 501, 501);

    return 0;
}

/* ------------------------------------------------------------------ */
/*  Streaming Writer Implementation                                    */
/* ------------------------------------------------------------------ */

struct ue4j_writer {
    FILE *fp;
    char  path[1024];
    int   count;
    char  fbuf[65536];
};

ue4j_writer_t *ue4j_writer_open(const char *path)
{
    if (!path) return NULL;

    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) return NULL;

    FILE *fp = fdopen(fd, "w");
    if (!fp) {
        close(fd);
        return NULL;
    }

    ue4j_writer_t *w = calloc(1, sizeof(*w));
    if (!w) {
        fclose(fp);
        return NULL;
    }

    w->fp = fp;
    strncpy(w->path, path, sizeof(w->path) - 1);
    setvbuf(fp, w->fbuf, _IOFBF, sizeof(w->fbuf));

    /* header */
    fprintf(fp, "{\n");
    fprintf(fp, "  \"format\": \"ue4-reflection-schema-v1\",\n");
    fprintf(fp, "  \"scope\": \"type metadata only; no object values\",\n");
    fprintf(fp, "  \"classes\": [\n");

    return w;
}

int ue4j_writer_write_class(ue4j_writer_t *w, const ue4_class_t *cls)
{
    if (!w || !w->fp || !cls) return -1;

    if (w->count > 0) {
        fputs(",\n", w->fp);
    }

    int res = json_write_class(w->fp, cls, 1);
    if (res == 0) {
        w->count++;
    }
    return res;
}

int ue4j_writer_close(ue4j_writer_t *w)
{
    if (!w) return -1;

    int count = w->count;
    if (w->fp) {
        fprintf(w->fp, "\n  ]\n}\n");
        fflush(w->fp);
        fclose(w->fp);
        chown(w->path, 501, 501);
    }
    free(w);
    return count;
}
