/* FACE::Configuration service (FACE 3.2 IDL) with memory: and file: backends.
 *
 * No nng dependency. One mutex guards the session table; sessions are
 * reference-counted only by their handle (close removes them).
 */
#include "face_tss/config_service.h"

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* ------------------------------------------------------------------ */
/* internals                                                          */
/* ------------------------------------------------------------------ */

typedef struct config_set {
    char *name;
    uint8_t *data;
    size_t len;
    struct config_set *next;
} config_set;

typedef enum {
    BACKEND_MEMORY,
    BACKEND_FILE
} backend_kind;

typedef struct config_session {
    int64_t handle;
    backend_kind kind;
    config_set *sets; /* memory backend */
    char *dir;        /* file backend: directory path */
    int64_t pos;      /* session position indicator */
    char *last_set;   /* set named by the most recent Read/Get_Size */
    struct config_session *next;
} config_session;

struct FACE_TSS_CONFIG_SERVICE {
    pthread_mutex_t lock;
    config_session *sessions;
    int64_t next_handle;
};

static void set_free(config_set *s)
{
    while (s) {
        config_set *n = s->next;
        free(s->name);
        free(s->data);
        free(s);
        s = n;
    }
}

static void session_free(config_session *s)
{
    set_free(s->sets);
    free(s->dir);
    free(s->last_set);
    free(s);
}

/* Must hold svc->lock. */
static config_session *find_session(FACE_TSS_CONFIG_SERVICE *svc, int64_t handle)
{
    config_session *s;
    for (s = svc->sessions; s; s = s->next) {
        if (s->handle == handle)
            return s;
    }
    return NULL;
}

static config_set *find_set(config_set *sets, const char *name)
{
    for (; sets; sets = sets->next) {
        if (strcmp(sets->name, name) == 0)
            return sets;
    }
    return NULL;
}

/* Validate a set name for the file backend: no separators, no dot-dots,
 * non-empty. Returns 1 if valid. */
static int valid_file_set_name(const char *name)
{
    if (!name || !*name)
        return 0;
    if (strchr(name, '/') != NULL)
        return 0;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
        return 0;
    return 1;
}

/* Build "<dir>/<name>" into out (outsz bytes). Returns 1 on success. */
static int join_path(const char *dir, const char *name, char *out, size_t outsz)
{
    size_t dn = strlen(dir);
    size_t nn = strlen(name);
    /* need dn + 1 + nn + 1 bytes */
    if (dn + nn + 2 > outsz)
        return 0;
    memcpy(out, dir, dn);
    out[dn] = '/';
    memcpy(out + dn + 1, name, nn + 1);
    return 1;
}

/* Size of a set in bytes, or -1 when the set does not exist.
 * Must hold svc->lock. */
static int64_t set_size(config_session *s, const char *name)
{
    if (s->kind == BACKEND_MEMORY) {
        config_set *cs = find_set(s->sets, name);
        if (!cs)
            return -1;
        if (cs->len > (size_t)INT64_MAX)
            return -1;
        return (int64_t)cs->len;
    }
    /* file backend */
    {
        char path[4096];
        struct stat st;
        if (!join_path(s->dir, name, path, sizeof(path)))
            return -1;
        if (stat(path, &st) != 0 || !S_ISREG(st.st_mode))
            return -1;
        return (int64_t)st.st_size;
    }
}

static void remember_set(config_session *s, const char *name)
{
    char *copy = NULL;
    if (name) {
        copy = strdup(name);
        if (!copy)
            return; /* keep the old value on OOM */
    }
    free(s->last_set);
    s->last_set = copy;
}

/* ------------------------------------------------------------------ */
/* public API                                                         */
/* ------------------------------------------------------------------ */

FACE_TSS_CONFIG_SERVICE *face_tss_config_service_create(void)
{
    FACE_TSS_CONFIG_SERVICE *svc = calloc(1, sizeof(*svc));
    if (!svc)
        return NULL;
    if (pthread_mutex_init(&svc->lock, NULL) != 0) {
        free(svc);
        return NULL;
    }
    svc->next_handle = 1;
    return svc;
}

void face_tss_config_service_destroy(FACE_TSS_CONFIG_SERVICE *svc)
{
    config_session *s;
    if (!svc)
        return;
    pthread_mutex_lock(&svc->lock);
    s = svc->sessions;
    svc->sessions = NULL;
    pthread_mutex_unlock(&svc->lock);
    while (s) {
        config_session *n = s->next;
        session_free(s);
        s = n;
    }
    pthread_mutex_destroy(&svc->lock);
    free(svc);
}

FACE_TSS_RETURN_CODE face_tss_config_service_initialize(
    FACE_TSS_CONFIG_SERVICE *svc, const char *initialization_information)
{
    if (!svc || !initialization_information)
        return FACE_TSS_RC_INVALID_PARAM;
    /* Nothing implementation-specific to do; the string is accepted. */
    return FACE_TSS_RC_NO_ERROR;
}

FACE_TSS_RETURN_CODE face_tss_config_service_open(
    FACE_TSS_CONFIG_SERVICE *svc, const char *container_name,
    FACE_TSS_CONFIG_HANDLE *handle)
{
    config_session *s;
    if (!svc || !container_name || !handle)
        return FACE_TSS_RC_INVALID_PARAM;

    s = calloc(1, sizeof(*s));
    if (!s)
        return FACE_TSS_RC_NOT_AVAILABLE;

    if (strncmp(container_name, "memory:", 7) == 0) {
        s->kind = BACKEND_MEMORY;
    } else if (strncmp(container_name, "file:", 5) == 0) {
        struct stat st;
        const char *dir = container_name + 5;
        if (!*dir || stat(dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
            free(s);
            return FACE_TSS_RC_INVALID_CONFIG;
        }
        s->kind = BACKEND_FILE;
        s->dir = strdup(dir);
        if (!s->dir) {
            free(s);
            return FACE_TSS_RC_NOT_AVAILABLE;
        }
    } else {
        free(s);
        return FACE_TSS_RC_INVALID_CONFIG;
    }

    pthread_mutex_lock(&svc->lock);
    s->handle = svc->next_handle++;
    s->next = svc->sessions;
    svc->sessions = s;
    *handle = s->handle;
    pthread_mutex_unlock(&svc->lock);
    return FACE_TSS_RC_NO_ERROR;
}

FACE_TSS_RETURN_CODE face_tss_config_service_get_size(
    FACE_TSS_CONFIG_SERVICE *svc, FACE_TSS_CONFIG_HANDLE handle,
    const char *set_name, long *size)
{
    config_session *s;
    int64_t sz;
    if (!svc || !set_name || !*set_name || !size)
        return FACE_TSS_RC_INVALID_PARAM;
    pthread_mutex_lock(&svc->lock);
    s = find_session(svc, handle);
    if (!s) {
        pthread_mutex_unlock(&svc->lock);
        return FACE_TSS_RC_INVALID_CONFIG;
    }
    if (s->kind == BACKEND_FILE && !valid_file_set_name(set_name)) {
        pthread_mutex_unlock(&svc->lock);
        return FACE_TSS_RC_INVALID_PARAM;
    }
    sz = set_size(s, set_name);
    if (sz < 0) {
        pthread_mutex_unlock(&svc->lock);
        return FACE_TSS_RC_INVALID_CONFIG;
    }
    remember_set(s, set_name);
    if (sz > (int64_t)LONG_MAX)
        sz = (int64_t)LONG_MAX;
    *size = (long)sz;
    pthread_mutex_unlock(&svc->lock);
    return FACE_TSS_RC_NO_ERROR;
}

FACE_TSS_RETURN_CODE face_tss_config_service_read(
    FACE_TSS_CONFIG_SERVICE *svc, FACE_TSS_CONFIG_HANDLE handle,
    const char *set_name, void *buffer, long buffer_size, long *bytes_read)
{
    config_session *s;
    int64_t sz, avail;
    long want, got = 0;
    FACE_TSS_RETURN_CODE rc = FACE_TSS_RC_NO_ERROR;
    if (!svc || !set_name || !*set_name || !buffer || !bytes_read)
        return FACE_TSS_RC_INVALID_PARAM;
    if (buffer_size < 0)
        return FACE_TSS_RC_INVALID_PARAM;

    pthread_mutex_lock(&svc->lock);
    s = find_session(svc, handle);
    if (!s) {
        pthread_mutex_unlock(&svc->lock);
        return FACE_TSS_RC_INVALID_CONFIG;
    }
    if (s->kind == BACKEND_FILE && !valid_file_set_name(set_name)) {
        pthread_mutex_unlock(&svc->lock);
        return FACE_TSS_RC_INVALID_PARAM;
    }
    sz = set_size(s, set_name);
    if (sz < 0) {
        pthread_mutex_unlock(&svc->lock);
        return FACE_TSS_RC_INVALID_CONFIG;
    }
    remember_set(s, set_name);
    if (s->pos >= sz) {
        /* Entire stream already read. */
        *bytes_read = 0;
        pthread_mutex_unlock(&svc->lock);
        return FACE_TSS_RC_NOT_AVAILABLE;
    }
    avail = sz - s->pos;
    want = buffer_size;
    if ((int64_t)want > avail)
        want = (long)avail;

    if (s->kind == BACKEND_MEMORY) {
        config_set *cs = find_set(s->sets, set_name);
        /* set_size succeeded, so cs exists. */
        memcpy(buffer, cs->data + s->pos, (size_t)want);
        got = want;
    } else {
        char path[4096];
        FILE *f;
        if (!join_path(s->dir, set_name, path, sizeof(path))) {
            pthread_mutex_unlock(&svc->lock);
            return FACE_TSS_RC_INVALID_CONFIG;
        }
        f = fopen(path, "rb");
        if (!f) {
            pthread_mutex_unlock(&svc->lock);
            return FACE_TSS_RC_INVALID_CONFIG;
        }
        if (fseek(f, (long)s->pos, SEEK_SET) != 0) {
            fclose(f);
            pthread_mutex_unlock(&svc->lock);
            return FACE_TSS_RC_NOT_AVAILABLE;
        }
        got = (long)fread(buffer, 1, (size_t)want, f);
        if (got < 0)
            got = 0;
        if (ferror(f)) {
            fclose(f);
            pthread_mutex_unlock(&svc->lock);
            return FACE_TSS_RC_NOT_AVAILABLE;
        }
        fclose(f);
    }
    s->pos += got;
    *bytes_read = got;
    pthread_mutex_unlock(&svc->lock);
    return rc;
}

FACE_TSS_RETURN_CODE face_tss_config_service_seek(
    FACE_TSS_CONFIG_SERVICE *svc, FACE_TSS_CONFIG_HANDLE handle,
    FACE_TSS_CONFIG_WHENCE whence, long offset)
{
    config_session *s;
    int64_t base, sz;
    int64_t newpos;
    if (!svc)
        return FACE_TSS_RC_INVALID_PARAM;
    if (whence != FACE_TSS_CONFIG_SEEK_FROM_START &&
        whence != FACE_TSS_CONFIG_SEEK_FROM_CURRENT &&
        whence != FACE_TSS_CONFIG_SEEK_FROM_END)
        return FACE_TSS_RC_INVALID_PARAM;

    pthread_mutex_lock(&svc->lock);
    s = find_session(svc, handle);
    if (!s) {
        pthread_mutex_unlock(&svc->lock);
        return FACE_TSS_RC_INVALID_CONFIG;
    }

    switch (whence) {
    case FACE_TSS_CONFIG_SEEK_FROM_START:
        if (offset < 0) {
            pthread_mutex_unlock(&svc->lock);
            return FACE_TSS_RC_INVALID_PARAM;
        }
        newpos = (int64_t)offset;
        break;
    case FACE_TSS_CONFIG_SEEK_FROM_CURRENT:
        base = s->pos;
        if ((offset < 0 && (int64_t)offset < -base) ||
            (offset > 0 && (int64_t)offset > INT64_MAX - base)) {
            pthread_mutex_unlock(&svc->lock);
            return FACE_TSS_RC_INVALID_PARAM;
        }
        newpos = base + (int64_t)offset;
        break;
    case FACE_TSS_CONFIG_SEEK_FROM_END:
        if (offset > 0) {
            pthread_mutex_unlock(&svc->lock);
            return FACE_TSS_RC_INVALID_PARAM;
        }
        if (!s->last_set) {
            pthread_mutex_unlock(&svc->lock);
            return FACE_TSS_RC_INVALID_CONFIG;
        }
        sz = set_size(s, s->last_set);
        if (sz < 0) {
            pthread_mutex_unlock(&svc->lock);
            return FACE_TSS_RC_INVALID_CONFIG;
        }
        if ((int64_t)offset < -sz) {
            pthread_mutex_unlock(&svc->lock);
            return FACE_TSS_RC_INVALID_PARAM;
        }
        newpos = sz + (int64_t)offset;
        break;
    default:
        pthread_mutex_unlock(&svc->lock);
        return FACE_TSS_RC_INVALID_PARAM;
    }

    s->pos = newpos;
    pthread_mutex_unlock(&svc->lock);
    return FACE_TSS_RC_NO_ERROR;
}

FACE_TSS_RETURN_CODE face_tss_config_service_close(
    FACE_TSS_CONFIG_SERVICE *svc, FACE_TSS_CONFIG_HANDLE handle)
{
    config_session **pp, *s;
    if (!svc)
        return FACE_TSS_RC_INVALID_PARAM;
    pthread_mutex_lock(&svc->lock);
    for (pp = &svc->sessions; *pp; pp = &(*pp)->next) {
        if ((*pp)->handle == handle) {
            s = *pp;
            *pp = s->next;
            pthread_mutex_unlock(&svc->lock);
            session_free(s);
            return FACE_TSS_RC_NO_ERROR;
        }
    }
    pthread_mutex_unlock(&svc->lock);
    return FACE_TSS_RC_INVALID_CONFIG;
}

FACE_TSS_RETURN_CODE face_tss_config_service_write(
    FACE_TSS_CONFIG_SERVICE *svc, FACE_TSS_CONFIG_HANDLE handle,
    const char *set_name, const void *data, long len)
{
    config_session *s;
    config_set *cs;
    uint8_t *copy = NULL;
    if (!svc || !set_name || !*set_name)
        return FACE_TSS_RC_INVALID_PARAM;
    if (len < 0 || (len > 0 && !data))
        return FACE_TSS_RC_INVALID_PARAM;

    pthread_mutex_lock(&svc->lock);
    s = find_session(svc, handle);
    if (!s) {
        pthread_mutex_unlock(&svc->lock);
        return FACE_TSS_RC_INVALID_CONFIG;
    }
    if (s->kind != BACKEND_MEMORY) {
        pthread_mutex_unlock(&svc->lock);
        return FACE_TSS_RC_INVALID_CONFIG;
    }
    if (len > 0) {
        copy = malloc((size_t)len);
        if (!copy) {
            pthread_mutex_unlock(&svc->lock);
            return FACE_TSS_RC_NOT_AVAILABLE;
        }
        memcpy(copy, data, (size_t)len);
    }
    cs = find_set(s->sets, set_name);
    if (cs) {
        free(cs->data);
        cs->data = copy;
        cs->len = (size_t)len;
    } else {
        cs = calloc(1, sizeof(*cs));
        if (!cs) {
            free(copy);
            pthread_mutex_unlock(&svc->lock);
            return FACE_TSS_RC_NOT_AVAILABLE;
        }
        cs->name = strdup(set_name);
        if (!cs->name) {
            free(copy);
            free(cs);
            pthread_mutex_unlock(&svc->lock);
            return FACE_TSS_RC_NOT_AVAILABLE;
        }
        cs->data = copy;
        cs->len = (size_t)len;
        cs->next = s->sets;
        s->sets = cs;
    }
    pthread_mutex_unlock(&svc->lock);
    return FACE_TSS_RC_NO_ERROR;
}
