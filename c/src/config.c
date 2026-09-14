/* FACE TSS config: normalize/validate/parse. See config.h for contract. */

#include "face_tss/config.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

FACE_TSS_RETURN_CODE face_tss_normalize_name(
    const char *name, char out[FACE_TSS_MAX_CONNECTION_NAME])
{
    size_t i;
    if (!name || !name[0])
        return FACE_TSS_RC_INVALID_PARAM;
    for (i = 0; name[i] != '\0'; i++) {
        if (i + 1 >= FACE_TSS_MAX_CONNECTION_NAME)
            return FACE_TSS_RC_INVALID_PARAM;
        out[i] = (char)toupper((unsigned char)name[i]);
    }
    out[i] = '\0';
    return FACE_TSS_RC_NO_ERROR;
}

void face_tss_config_init(FACE_TSS_CONFIG *cfg, const char *instance_name)
{
    if (!cfg)
        return;
    memset(cfg, 0, sizeof(*cfg));
    if (instance_name) {
        strncpy(cfg->instance_name, instance_name, sizeof(cfg->instance_name) - 1);
        cfg->instance_name[sizeof(cfg->instance_name) - 1] = '\0';
    } else {
        strcpy(cfg->instance_name, "face-tss");
    }
}

void face_tss_config_fini(FACE_TSS_CONFIG *cfg)
{
    if (!cfg)
        return;
    free(cfg->connections);
    cfg->connections = NULL;
    cfg->count = 0;
    cfg->capacity = 0;
}

FACE_TSS_RETURN_CODE face_tss_config_reserve(FACE_TSS_CONFIG *cfg, size_t n)
{
    FACE_TSS_CONNECTION_CONFIG *p;
    if (!cfg)
        return FACE_TSS_RC_INVALID_PARAM;
    if (n <= cfg->capacity)
        return FACE_TSS_RC_NO_ERROR;
    p = (FACE_TSS_CONNECTION_CONFIG *)realloc(
        cfg->connections, n * sizeof(*cfg->connections));
    if (!p)
        return FACE_TSS_RC_NOT_AVAILABLE;
    cfg->connections = p;
    cfg->capacity = n;
    return FACE_TSS_RC_NO_ERROR;
}

static FACE_TSS_RETURN_CODE validate_conn(FACE_TSS_CONNECTION_CONFIG *c)
{
    char norm[FACE_TSS_MAX_CONNECTION_NAME];
    FACE_TSS_RETURN_CODE rc = face_tss_normalize_name(c->name, norm);
    if (rc != FACE_TSS_RC_NO_ERROR)
        return rc;
    strcpy(c->name, norm);
    if (c->transport != FACE_TSS_TRANSPORT_PUBSUB &&
        c->transport != FACE_TSS_TRANSPORT_BUS)
        return FACE_TSS_RC_INVALID_CONFIG;
    if (c->transport == FACE_TSS_TRANSPORT_BUS) {
        c->role = FACE_TSS_ROLE_BUS; /* bus ignores role */
    } else if (c->role != FACE_TSS_ROLE_PUBLISHER &&
               c->role != FACE_TSS_ROLE_SUBSCRIBER) {
        return FACE_TSS_RC_INVALID_CONFIG;
    }
    if (c->max_message_size <= 0)
        return FACE_TSS_RC_INVALID_CONFIG;
    if (c->queue_depth <= 0)
        return FACE_TSS_RC_INVALID_CONFIG;
    if (!c->address[0])
        return FACE_TSS_RC_INVALID_CONFIG;
    return FACE_TSS_RC_NO_ERROR;
}

FACE_TSS_RETURN_CODE face_tss_config_add(
    FACE_TSS_CONFIG *cfg, const FACE_TSS_CONNECTION_CONFIG *conn)
{
    FACE_TSS_RETURN_CODE rc;
    FACE_TSS_CONNECTION_CONFIG copy;
    size_t i;
    if (!cfg || !conn)
        return FACE_TSS_RC_INVALID_PARAM;
    copy = *conn;
    copy.name[FACE_TSS_MAX_CONNECTION_NAME - 1] = '\0';
    copy.address[FACE_TSS_MAX_ADDRESS - 1] = '\0';
    rc = validate_conn(&copy);
    if (rc != FACE_TSS_RC_NO_ERROR)
        return rc;
    for (i = 0; i < cfg->count; i++) {
        if (strcmp(cfg->connections[i].name, copy.name) == 0)
            return FACE_TSS_RC_INVALID_CONFIG; /* duplicate */
    }
    if (cfg->count == cfg->capacity) {
        size_t want = cfg->capacity ? cfg->capacity * 2 : 4;
        rc = face_tss_config_reserve(cfg, want);
        if (rc != FACE_TSS_RC_NO_ERROR)
            return rc;
    }
    cfg->connections[cfg->count++] = copy;
    return FACE_TSS_RC_NO_ERROR;
}

const FACE_TSS_CONNECTION_CONFIG *face_tss_config_lookup(
    const FACE_TSS_CONFIG *cfg, const char *name)
{
    char norm[FACE_TSS_MAX_CONNECTION_NAME];
    size_t i;
    if (!cfg || !name)
        return NULL;
    if (face_tss_normalize_name(name, norm) != FACE_TSS_RC_NO_ERROR)
        return NULL;
    for (i = 0; i < cfg->count; i++) {
        if (strcmp(cfg->connections[i].name, norm) == 0)
            return &cfg->connections[i];
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Minimal JSON reader (enough for {"instance_name", "connections":[]} */
/* ------------------------------------------------------------------ */

typedef struct { const char *p; const char *end; } cur_t;

static void skip_ws(cur_t *c)
{
    while (c->p < c->end &&
           (*c->p == ' ' || *c->p == '\t' || *c->p == '\n' || *c->p == '\r'))
        c->p++;
}

static int expect(cur_t *c, char ch)
{
    skip_ws(c);
    if (c->p < c->end && *c->p == ch) {
        c->p++;
        return 1;
    }
    return 0;
}

/* Parse "string" with escapes into out (truncated). Returns 1 on success. */
static int parse_string(cur_t *c, char *out, size_t cap)
{
    size_t n = 0;
    skip_ws(c);
    if (c->p >= c->end || *c->p != '"')
        return 0;
    c->p++;
    while (c->p < c->end) {
        char ch = *c->p++;
        if (ch == '"') {
            if (n < cap)
                out[n] = '\0';
            else if (cap)
                out[cap - 1] = '\0';
            return 1;
        }
        if (ch == '\\' && c->p < c->end) {
            char e = *c->p++;
            switch (e) {
            case 'n': ch = '\n'; break;
            case 't': ch = '\t'; break;
            case 'r': ch = '\r'; break;
            case 'u': {
                /* \uXXXX: keep ASCII, replace the rest with '?'. */
                unsigned v = 0;
                int k;
                for (k = 0; k < 4 && c->p < c->end; k++) {
                    char h = *c->p++;
                    v <<= 4;
                    if (h >= '0' && h <= '9')
                        v |= (unsigned)(h - '0');
                    else if (h >= 'a' && h <= 'f')
                        v |= (unsigned)(h - 'a' + 10);
                    else if (h >= 'A' && h <= 'F')
                        v |= (unsigned)(h - 'A' + 10);
                }
                ch = (v < 128) ? (char)v : '?';
                break;
            }
            default: ch = e; break;
            }
        }
        if (n + 1 < cap)
            out[n++] = ch;
    }
    return 0;
}

static int parse_long(cur_t *c, long long *out)
{
    int neg = 0;
    long long v = 0;
    int digits = 0;
    skip_ws(c);
    if (c->p < c->end && (*c->p == '-' || *c->p == '+')) {
        neg = (*c->p == '-');
        c->p++;
    }
    while (c->p < c->end && *c->p >= '0' && *c->p <= '9') {
        v = v * 10 + (*c->p - '0');
        c->p++;
        digits++;
    }
    if (!digits)
        return 0;
    *out = neg ? -v : v;
    return 1;
}

/* Skip one JSON value (object/array/string/number/literal). */
static int skip_value(cur_t *c)
{
    char tmp[32];
    long long num;
    skip_ws(c);
    if (c->p >= c->end)
        return 0;
    if (*c->p == '{') {
        int first = 1;
        c->p++;
        skip_ws(c);
        if (c->p < c->end && *c->p == '}') {
            c->p++;
            return 1;
        }
        while (c->p < c->end) {
            if (!first && !expect(c, ','))
                return 0;
            first = 0;
            if (!parse_string(c, tmp, sizeof(tmp)))
                return 0;
            if (!expect(c, ':'))
                return 0;
            if (!skip_value(c))
                return 0;
            skip_ws(c);
            if (c->p < c->end && *c->p == '}') {
                c->p++;
                return 1;
            }
        }
        return 0;
    }
    if (*c->p == '[') {
        int first = 1;
        c->p++;
        skip_ws(c);
        if (c->p < c->end && *c->p == ']') {
            c->p++;
            return 1;
        }
        while (c->p < c->end) {
            if (!first && !expect(c, ','))
                return 0;
            first = 0;
            if (!skip_value(c))
                return 0;
            skip_ws(c);
            if (c->p < c->end && *c->p == ']') {
                c->p++;
                return 1;
            }
        }
        return 0;
    }
    if (*c->p == '"')
        return parse_string(c, tmp, sizeof(tmp));
    if ((*c->p >= '0' && *c->p <= '9') || *c->p == '-' || *c->p == '+')
        return parse_long(c, &num);
    if ((size_t)(c->end - c->p) >= 4 && !memcmp(c->p, "true", 4)) {
        c->p += 4;
        return 1;
    }
    if ((size_t)(c->end - c->p) >= 5 && !memcmp(c->p, "false", 5)) {
        c->p += 5;
        return 1;
    }
    if ((size_t)(c->end - c->p) >= 4 && !memcmp(c->p, "null", 4)) {
        c->p += 4;
        return 1;
    }
    return 0;
}

static void conn_defaults(FACE_TSS_CONNECTION_CONFIG *c)
{
    memset(c, 0, sizeof(*c));
    c->direction = FACE_TSS_BI_DIRECTIONAL;
    c->transport = FACE_TSS_TRANSPORT_PUBSUB;
    c->role = FACE_TSS_ROLE_SUBSCRIBER;
    c->max_message_size = 65536;
    c->queue_depth = 64;
}

static int streq_ci(const char *a, const char *b)
{
    while (*a && *b) {
        if (toupper((unsigned char)*a) != toupper((unsigned char)*b))
            return 0;
        a++;
        b++;
    }
    return *a == *b;
}

/* Parse one connection object; `c` already holds defaults. Unknown keys
 * are skipped. Returns 1 ok / 0 malformed. */
static int parse_conn(cur_t *c, FACE_TSS_CONNECTION_CONFIG *conn)
{
    char key[64], val[FACE_TSS_MAX_ADDRESS];
    long long num;
    int first = 1;
    if (!expect(c, '{'))
        return 0;
    skip_ws(c);
    if (c->p < c->end && *c->p == '}') {
        c->p++;
        return 1;
    }
    while (c->p < c->end) {
        if (!first && !expect(c, ','))
            return 0;
        first = 0;
        if (!parse_string(c, key, sizeof(key)))
            return 0;
        if (!expect(c, ':'))
            return 0;
        if (streq_ci(key, "name")) {
            if (!parse_string(c, conn->name, sizeof(conn->name)))
                return 0;
        } else if (streq_ci(key, "address")) {
            if (!parse_string(c, conn->address, sizeof(conn->address)))
                return 0;
        } else if (streq_ci(key, "direction")) {
            skip_ws(c);
            if (c->p < c->end && *c->p == '"') {
                if (!parse_string(c, val, sizeof(val)))
                    return 0;
                if (streq_ci(val, "SOURCE"))
                    conn->direction = FACE_TSS_SOURCE;
                else if (streq_ci(val, "DESTINATION"))
                    conn->direction = FACE_TSS_DESTINATION;
                else if (streq_ci(val, "BI_DIRECTIONAL") ||
                         streq_ci(val, "BIDIRECTIONAL"))
                    conn->direction = FACE_TSS_BI_DIRECTIONAL;
                else
                    return 0;
            } else {
                if (!parse_long(c, &num))
                    return 0;
                if (num < 0 || num > 2)
                    return 0;
                conn->direction = (FACE_TSS_DIRECTION)num;
            }
        } else if (streq_ci(key, "transport")) {
            if (!parse_string(c, val, sizeof(val)))
                return 0;
            if (streq_ci(val, "pubsub") || streq_ci(val, "pub_sub") ||
                streq_ci(val, "pub-sub"))
                conn->transport = FACE_TSS_TRANSPORT_PUBSUB;
            else if (streq_ci(val, "bus"))
                conn->transport = FACE_TSS_TRANSPORT_BUS;
            else
                return 0;
        } else if (streq_ci(key, "role")) {
            if (!parse_string(c, val, sizeof(val)))
                return 0;
            if (streq_ci(val, "publisher") || streq_ci(val, "pub"))
                conn->role = FACE_TSS_ROLE_PUBLISHER;
            else if (streq_ci(val, "subscriber") || streq_ci(val, "sub"))
                conn->role = FACE_TSS_ROLE_SUBSCRIBER;
            else if (streq_ci(val, "bus"))
                conn->role = FACE_TSS_ROLE_BUS;
            else
                return 0;
        } else if (streq_ci(key, "max_message_size")) {
            if (!parse_long(c, &num) || num <= 0 || num > INT32_MAX)
                return 0;
            conn->max_message_size = (FACE_TSS_MESSAGE_SIZE_TYPE)num;
        } else if (streq_ci(key, "queue_depth")) {
            if (!parse_long(c, &num) || num <= 0 || num > 1000000)
                return 0;
            conn->queue_depth = (int)num;
        } else {
            if (!skip_value(c))
                return 0;
        }
        skip_ws(c);
        if (c->p < c->end && *c->p == '}') {
            c->p++;
            return 1;
        }
    }
    return 0;
}

FACE_TSS_RETURN_CODE face_tss_config_from_json(
    const char *json, size_t len, FACE_TSS_CONFIG *out)
{
    cur_t c;
    char key[64], val[FACE_TSS_MAX_STRING];
    FACE_TSS_CONFIG tmp;
    FACE_TSS_RETURN_CODE rc;
    int first;
    if (!json || !out)
        return FACE_TSS_RC_INVALID_PARAM;
    face_tss_config_init(&tmp, "face-tss");
    c.p = json;
    c.end = json + len;
    if (!expect(&c, '{')) {
        rc = FACE_TSS_RC_INVALID_CONFIG;
        goto fail;
    }
    first = 1;
    skip_ws(&c);
    if (c.p < c.end && *c.p == '}') {
        c.p++;
        goto done; /* empty object: valid, no connections */
    }
    while (c.p < c.end) {
        if (!first && !expect(&c, ',')) {
            rc = FACE_TSS_RC_INVALID_CONFIG;
            goto fail;
        }
        first = 0;
        if (!parse_string(&c, key, sizeof(key))) {
            rc = FACE_TSS_RC_INVALID_CONFIG;
            goto fail;
        }
        if (!expect(&c, ':')) {
            rc = FACE_TSS_RC_INVALID_CONFIG;
            goto fail;
        }
        if (streq_ci(key, "instance_name")) {
            if (!parse_string(&c, val, sizeof(val))) {
                rc = FACE_TSS_RC_INVALID_CONFIG;
                goto fail;
            }
            strncpy(tmp.instance_name, val, sizeof(tmp.instance_name) - 1);
            tmp.instance_name[sizeof(tmp.instance_name) - 1] = '\0';
        } else if (streq_ci(key, "connections")) {
            int afirst;
            if (!expect(&c, '[')) {
                rc = FACE_TSS_RC_INVALID_CONFIG;
                goto fail;
            }
            afirst = 1;
            skip_ws(&c);
            if (c.p < c.end && *c.p == ']') {
                c.p++;
            } else {
                while (c.p < c.end) {
                    FACE_TSS_CONNECTION_CONFIG conn;
                    if (!afirst && !expect(&c, ',')) {
                        rc = FACE_TSS_RC_INVALID_CONFIG;
                        goto fail;
                    }
                    afirst = 0;
                    conn_defaults(&conn);
                    if (!parse_conn(&c, &conn)) {
                        rc = FACE_TSS_RC_INVALID_CONFIG;
                        goto fail;
                    }
                    rc = face_tss_config_add(&tmp, &conn);
                    if (rc != FACE_TSS_RC_NO_ERROR)
                        goto fail;
                    skip_ws(&c);
                    if (c.p < c.end && *c.p == ']') {
                        c.p++;
                        break;
                    }
                }
            }
        } else {
            if (!skip_value(&c)) {
                rc = FACE_TSS_RC_INVALID_CONFIG;
                goto fail;
            }
        }
        skip_ws(&c);
        if (c.p < c.end && *c.p == '}') {
            c.p++;
            goto done;
        }
    }
    rc = FACE_TSS_RC_INVALID_CONFIG;
    goto fail;

done:
    skip_ws(&c);
    if (c.p != c.end) { /* trailing garbage */
        rc = FACE_TSS_RC_INVALID_CONFIG;
        goto fail;
    }
    face_tss_config_fini(out);
    *out = tmp;
    return FACE_TSS_RC_NO_ERROR;

fail:
    face_tss_config_fini(&tmp);
    return rc;
}

FACE_TSS_RETURN_CODE face_tss_config_from_file(
    const char *path, FACE_TSS_CONFIG *out)
{
    FILE *f;
    long n;
    char *buf;
    size_t got;
    FACE_TSS_RETURN_CODE rc;
    if (!path || !out)
        return FACE_TSS_RC_INVALID_PARAM;
    f = fopen(path, "rb");
    if (!f)
        return FACE_TSS_RC_INVALID_CONFIG;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return FACE_TSS_RC_INVALID_CONFIG;
    }
    n = ftell(f);
    if (n < 0 || n > (long)(16 << 20)) {
        fclose(f);
        return FACE_TSS_RC_INVALID_CONFIG;
    }
    rewind(f);
    buf = (char *)malloc((size_t)n + 1);
    if (!buf) {
        fclose(f);
        return FACE_TSS_RC_NOT_AVAILABLE;
    }
    got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    if (got != (size_t)n) {
        free(buf);
        return FACE_TSS_RC_INVALID_CONFIG;
    }
    rc = face_tss_config_from_json(buf, got, out);
    free(buf);
    return rc;
}
