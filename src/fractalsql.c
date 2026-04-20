/* src/fractalsql.c
 * mariadb-fractalsql v1.0 — Stochastic Fractal Search for MariaDB (UDF).
 *
 * Compatible with MariaDB 10.6, 10.11, 11.4 LTS, and 12.2 (rolling).
 * The UDF ABI has been stable across these majors — the struct
 * layouts of UDF_INIT / UDF_ARGS, the _init/main/_deinit signatures,
 * and MYSQL_ERRMSG_SIZE (512) have not changed. Ship ONE fractalsql.so
 * per arch that works on every supported major; release packages
 * depend on mariadb-server generically.
 *
 * MariaDB 11 compatibility notes
 *   * Plugin loader changes in MariaDB 11.x affect storage-engine and
 *     server-plugin loading. UDFs registered via CREATE FUNCTION ...
 *     SONAME go through a separate, unchanged code path — the
 *     mariadb_sys_var framework is not involved.
 *   * Result-memory lifetime is unchanged: the char* returned from
 *     the main function must remain valid until the next call on the
 *     same initid. This UDF owns the buffer in ctx->result_buf and
 *     frees it in *_deinit, same pattern as the MySQL port.
 *   * MariaDB 11.7+ introduced a native VECTOR type with its own
 *     VEC_FromBinary() / VEC_ToBinary() (distinct from MySQL 9.0's
 *     encoding). 11.4 LTS does not have VECTOR, so this source
 *     accepts only CSV / bracketed-JSON inputs. When 11.7+ joins the
 *     support matrix, a dedicated -DFRACTAL_HAVE_MDB_VECTOR_TYPE
 *     decode path can live alongside the existing MySQL one.
 *
 * SQL surface
 *   fractal_search(vector_csv, query_csv, k, params) -> JSON STRING
 *
 *     vector_csv  corpus of stored vectors, encoded as either
 *                   '[[v11,v12,...],[v21,...],...]'
 *                   'v11,v12,...;v21,...;...'
 *                   or the empty string ''
 *     query_csv   single query vector, same string formats accepted
 *     k           positive integer, number of top matches to return
 *     params      JSON object of SFS tuning knobs:
 *                   {"iterations":30,
 *                    "population_size":50,
 *                    "diffusion_factor":2,
 *                    "walk":0.5,
 *                    "debug":false}
 *
 * Output shape (pass to JSON_EXTRACT / ->> as usual)
 *   { "best_point": [d1, d2, ...],
 *     "best_fit":   <double>,
 *     "top_k":      [{"idx": <int>, "dist": <double>}, ...],
 *     "dim":        <int>,
 *     "n_corpus":   <int>,
 *     "trace":      { ... }          -- only when params.debug = true
 *   }
 *
 * Architecture
 *   * One Lua state per UDF invocation. MariaDB is multi-threaded with
 *     no stable thread affinity across calls, so a fresh state is
 *     built in *_init, the pointer travels through initid->ptr, and
 *     it is closed in *_deinit. No global Lua state, no cross-call
 *     locking.
 *   * The LuaJIT optimizer ships as pre-stripped bytecode embedded
 *     via include/sfs_core_bc.h. No Lua source exists at runtime.
 *
 * Porting note
 *   Derived from the PostgreSQL extension via mysql-fractalsql. Core
 *   fractal math and LuaJIT integration logic preserved verbatim;
 *   only the host bindings changed.
 */

#include <mysql.h>

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>
#include <luajit.h>

#include "sfs_core_bc.h"

/* Windows DLLs need exported UDF entry points in the module's export
 * table. Linux .so files land them in .dynsym automatically via
 * -fvisibility=default (the build flags don't hide them). One macro,
 * applied at the definition site only — never forward-declare UDF
 * entry points, or MSVC's C2375 "different linkage" fires. */
#if defined(_WIN32) || defined(__CYGWIN__)
#  define FRACTAL_EXPORT __declspec(dllexport)
#else
#  define FRACTAL_EXPORT
#endif

/* ------------------------------------------------------------------ */
/* Per-invocation context stored in initid->ptr                       */
/* ------------------------------------------------------------------ */

typedef struct sfs_ctx {
    lua_State *L;
    int        module_ref;
    char      *result_buf;
    size_t     result_cap;
} sfs_ctx;

#define SFS_INIT_ERROR(msg, ...) \
    (snprintf((msg), MYSQL_ERRMSG_SIZE, __VA_ARGS__))

/* ------------------------------------------------------------------ */
/* Lua lifecycle                                                      */
/* ------------------------------------------------------------------ */

static int
l_panic(lua_State *L)
{
    /* Every real Lua call in this TU goes through lua_pcall, so this
     * handler is never reached in practice. Silent return keeps UDF
     * errors out of mysqld's error log; LuaJIT's default behavior
     * (abort) will take over if we're ever wrong about pcall coverage. */
    (void) L;
    return 0;
}

static bool
load_module(sfs_ctx *c, char *errmsg)
{
    int rc;

    c->L = luaL_newstate();
    if (c->L == NULL) {
        SFS_INIT_ERROR(errmsg, "fractalsql: could not allocate LuaJIT state");
        return false;
    }
    lua_atpanic(c->L, l_panic);
    luaL_openlibs(c->L);

    rc = luaL_loadbuffer(c->L,
                         (const char *) luaJIT_BC_fractalsql_community,
                         luaJIT_BC_fractalsql_community_SIZE,
                         "=fractalsql_community");
    if (rc != 0) {
        const char *m = lua_tostring(c->L, -1);
        SFS_INIT_ERROR(errmsg,
                       "fractalsql: loading sfs_core bytecode: %s",
                       m ? m : "?");
        return false;
    }

    rc = lua_pcall(c->L, 0, 1, 0);
    if (rc != 0) {
        const char *m = lua_tostring(c->L, -1);
        SFS_INIT_ERROR(errmsg,
                       "fractalsql: initializing sfs_core: %s",
                       m ? m : "?");
        return false;
    }

    c->module_ref = luaL_ref(c->L, LUA_REGISTRYINDEX);
    return true;
}

/* ------------------------------------------------------------------ */
/* Argument parsing                                                   */
/* ------------------------------------------------------------------ */

#ifdef FRACTAL_HAVE_VECTOR_TYPE
/* MySQL 9.0 VECTOR values arrive as binary strings containing packed
 * little-endian float32 values. Detect by elimination: CSV text always
 * starts with a sign, digit, dot, bracket, or whitespace. Anything
 * else with a length that's a multiple of 4 is treated as binary. */
static bool
looks_like_vector_binary(const char *s, size_t n)
{
    if (n == 0 || n % 4 != 0) return false;
    unsigned char c = (unsigned char) s[0];
    if (c == '[' || c == '-' || c == '+' || c == '.' || c == ' ' ||
        c == '\t' || c == '\n' || c == '\r' ||
        (c >= '0' && c <= '9'))
        return false;
    return true;
}

static bool
parse_vector_binary(const char *s, size_t n,
                    double **out, size_t *n_out, char *errmsg)
{
    size_t  count = n / 4;
    double *v;
    size_t  i;

    v = malloc(count * sizeof(double));
    if (v == NULL) {
        SFS_INIT_ERROR(errmsg, "fractalsql: oom decoding VECTOR");
        return false;
    }
    /* Unaligned 32-bit reads via memcpy; hot inlined on every arch. */
    for (i = 0; i < count; i++) {
        float f;
        memcpy(&f, s + i * 4, 4);
        v[i] = (double) f;
    }
    *out = v;
    *n_out = count;
    return true;
}
#endif  /* FRACTAL_HAVE_VECTOR_TYPE */

/* Parse a CSV/JSON-ish flat vector into doubles. Accepts
 *   '1.0,2.0,3.0'    '[1.0,2.0,3.0]'    with or without whitespace
 */
static bool
parse_vector_csv(const char *src, size_t srclen,
                 double **out, size_t *n_out, char *errmsg)
{
    char   *buf, *p, *end;
    size_t  cap = 16, n = 0;
    double *v;

#ifdef FRACTAL_HAVE_VECTOR_TYPE
    if (looks_like_vector_binary(src, srclen))
        return parse_vector_binary(src, srclen, out, n_out, errmsg);
#endif

    buf = malloc(srclen + 1);
    if (buf == NULL) {
        SFS_INIT_ERROR(errmsg, "fractalsql: oom parsing vector");
        return false;
    }
    memcpy(buf, src, srclen);
    buf[srclen] = '\0';

    v = malloc(cap * sizeof(double));
    if (v == NULL) {
        free(buf);
        SFS_INIT_ERROR(errmsg, "fractalsql: oom parsing vector");
        return false;
    }

    p = buf;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == ',' ||
               *p == '[' || *p == ']' || *p == '\n' || *p == '\r')
            p++;
        if (*p == '\0') break;

        errno = 0;
        double d = strtod(p, &end);
        if (end == p) {
            SFS_INIT_ERROR(errmsg,
                           "fractalsql: invalid number near '%.20s'", p);
            free(buf); free(v);
            return false;
        }
        if (errno == ERANGE) {
            SFS_INIT_ERROR(errmsg, "fractalsql: value out of range");
            free(buf); free(v);
            return false;
        }

        if (n == cap) {
            size_t ncap = cap * 2;
            double *nv = realloc(v, ncap * sizeof(double));
            if (nv == NULL) {
                SFS_INIT_ERROR(errmsg, "fractalsql: oom growing vector");
                free(buf); free(v);
                return false;
            }
            v = nv; cap = ncap;
        }
        v[n++] = d;
        p = end;
    }

    free(buf);

    if (n == 0) {
        SFS_INIT_ERROR(errmsg, "fractalsql: vector must have at least one element");
        free(v);
        return false;
    }

    *out = v;
    *n_out = n;
    return true;
}

/* Parse a corpus: rows separated by ';' or by '],['. Each row is a
 * flat CSV, all rows must share the same dimension. Empty string ->
 * zero-row corpus (valid; disables top-k and returns best_point only). */
static bool
parse_corpus(const char *src, size_t srclen, size_t expected_dim,
             double **out, size_t *n_rows_out, size_t *dim_out, char *errmsg)
{
    double *store = NULL;
    size_t  cap_rows = 0, n_rows = 0, dim = expected_dim;
    size_t  i, start;
    bool    in_brackets = false;

    /* strip outer whitespace + optional enclosing '[ ... ]' of the
     * whole-corpus JSON-ish form */
    i = 0;
    while (i < srclen && (src[i] == ' ' || src[i] == '\t' ||
                          src[i] == '\n' || src[i] == '\r'))
        i++;
    if (i < srclen && src[i] == '[') {
        /* Could be either "[[...],[...]]" or "[v1,v2,...]" (single row).
         * Disambiguate by looking for a nested '['. */
        size_t j = i + 1;
        while (j < srclen && (src[j] == ' ' || src[j] == '\t' ||
                              src[j] == '\n' || src[j] == '\r'))
            j++;
        if (j < srclen && src[j] == '[') {
            in_brackets = true;
            i++;  /* consume the outer '[' — nested '['s stay as row markers */
            /* strip trailing ']' */
            while (srclen > i && (src[srclen - 1] == ' ' ||
                                  src[srclen - 1] == '\t' ||
                                  src[srclen - 1] == '\n' ||
                                  src[srclen - 1] == '\r'))
                srclen--;
            if (srclen > i && src[srclen - 1] == ']')
                srclen--;
        }
    }

    /* Empty corpus case — legal. */
    {
        size_t j = i;
        while (j < srclen && (src[j] == ' ' || src[j] == '\t' ||
                              src[j] == '\n' || src[j] == '\r' ||
                              src[j] == '[' || src[j] == ']'))
            j++;
        if (j >= srclen) {
            *out = NULL;
            *n_rows_out = 0;
            *dim_out = dim;
            return true;
        }
    }

    start = i;
    for (; i <= srclen; i++) {
        bool at_sep;
        if (i == srclen) {
            at_sep = true;
        } else if (src[i] == ';') {
            at_sep = true;
        } else if (in_brackets && src[i] == '[' && i > start) {
            /* boundary between "],[": back up past the comma/']' */
            at_sep = true;
        } else {
            at_sep = false;
        }
        if (!at_sep) continue;

        /* Trim ']' / ',' / ws from the end of this row span. */
        size_t end = i;
        while (end > start && (src[end - 1] == ' ' || src[end - 1] == '\t' ||
                               src[end - 1] == ']' || src[end - 1] == ',' ||
                               src[end - 1] == '\n' || src[end - 1] == '\r'))
            end--;
        /* Skip a leading '[' from this row span if present. */
        size_t s = start;
        while (s < end && (src[s] == ' ' || src[s] == '\t' ||
                           src[s] == '[' || src[s] == '\n' || src[s] == '\r'))
            s++;

        if (s < end) {
            double *row;
            size_t  row_n;
            if (!parse_vector_csv(src + s, end - s, &row, &row_n, errmsg)) {
                free(store);
                return false;
            }
            if (dim == 0) dim = row_n;
            if (row_n != dim) {
                SFS_INIT_ERROR(errmsg,
                               "fractalsql: corpus row %zu has dim %zu, expected %zu",
                               n_rows, row_n, dim);
                free(row); free(store);
                return false;
            }
            if (n_rows == cap_rows) {
                size_t ncap = cap_rows ? cap_rows * 2 : 16;
                double *nv = realloc(store, ncap * dim * sizeof(double));
                if (nv == NULL) {
                    SFS_INIT_ERROR(errmsg, "fractalsql: oom growing corpus");
                    free(row); free(store);
                    return false;
                }
                store = nv; cap_rows = ncap;
            }
            memcpy(store + n_rows * dim, row, dim * sizeof(double));
            free(row);
            n_rows++;
        }

        start = (i < srclen && src[i] == '[') ? i : i + 1;
    }

    *out = store;
    *n_rows_out = n_rows;
    *dim_out = dim;
    return true;
}

/* ------------------------------------------------------------------ */
/* Minimal JSON key lookup for the params object                      */
/* ------------------------------------------------------------------ */

static bool
json_find_key(const char *s, size_t slen, const char *key, size_t *out_pos)
{
    size_t klen = strlen(key);
    for (size_t i = 0; i + klen + 2 < slen; i++) {
        if (s[i] != '"') continue;
        if (strncmp(s + i + 1, key, klen) != 0) continue;
        if (s[i + 1 + klen] != '"') continue;
        size_t j = i + 2 + klen;
        while (j < slen && (s[j] == ' ' || s[j] == '\t' ||
                            s[j] == '\n' || s[j] == '\r'))
            j++;
        if (j < slen && s[j] == ':') {
            j++;
            while (j < slen && (s[j] == ' ' || s[j] == '\t' ||
                                s[j] == '\n' || s[j] == '\r'))
                j++;
            *out_pos = j;
            return true;
        }
    }
    return false;
}

static int
json_get_int(const char *s, size_t slen, const char *key, int fallback)
{
    size_t pos;
    char   buf[32];
    size_t n = 0;

    if (!json_find_key(s, slen, key, &pos)) return fallback;
    while (pos < slen && n < sizeof(buf) - 1 &&
           (isdigit((unsigned char) s[pos]) || s[pos] == '-' || s[pos] == '+'))
        buf[n++] = s[pos++];
    buf[n] = '\0';
    if (n == 0) return fallback;
    return atoi(buf);
}

static double
json_get_double(const char *s, size_t slen, const char *key, double fallback)
{
    size_t pos;
    char   buf[64];
    size_t n = 0;

    if (!json_find_key(s, slen, key, &pos)) return fallback;
    while (pos < slen && n < sizeof(buf) - 1 &&
           (isdigit((unsigned char) s[pos]) || s[pos] == '-' || s[pos] == '+' ||
            s[pos] == '.' || s[pos] == 'e' || s[pos] == 'E'))
        buf[n++] = s[pos++];
    buf[n] = '\0';
    if (n == 0) return fallback;
    return strtod(buf, NULL);
}

static bool
json_get_bool(const char *s, size_t slen, const char *key, bool fallback)
{
    size_t pos;
    if (!json_find_key(s, slen, key, &pos)) return fallback;
    if (pos + 4 <= slen && strncmp(s + pos, "true", 4) == 0) return true;
    if (pos + 5 <= slen && strncmp(s + pos, "false", 5) == 0) return false;
    return fallback;
}

/* ------------------------------------------------------------------ */
/* Lua bridge: build cfg and run sfs_core.run / run_debug             */
/* ------------------------------------------------------------------ */

static int
prepare_call(lua_State *L, int module_ref, const char *entry_name,
             const double *qv, int dim,
             int iterations, int pop_size, int diff_factor, double walk)
{
    int rc, i;

    lua_rawgeti(L, LUA_REGISTRYINDEX, module_ref);     /* [M] */
    lua_getfield(L, -1, entry_name);                   /* [M, entry] */
    lua_getfield(L, -2, "cosine_fitness");             /* [M, entry, cf] */
    lua_remove(L, -3);                                 /* [entry, cf] */

    lua_createtable(L, dim, 0);
    for (i = 0; i < dim; i++) {
        lua_pushnumber(L, qv[i]);
        lua_rawseti(L, -2, i + 1);
    }
    rc = lua_pcall(L, 1, 1, 0);                        /* [entry, fn] */
    if (rc != 0) return rc;

    lua_createtable(L, 0, 8);                          /* [entry, fn, cfg] */

    lua_createtable(L, dim, 0);
    for (i = 1; i <= dim; i++) { lua_pushnumber(L, -1.0); lua_rawseti(L, -2, i); }
    lua_setfield(L, -2, "lower");

    lua_createtable(L, dim, 0);
    for (i = 1; i <= dim; i++) { lua_pushnumber(L, 1.0); lua_rawseti(L, -2, i); }
    lua_setfield(L, -2, "upper");

    lua_pushinteger(L, iterations); lua_setfield(L, -2, "max_generation");
    lua_pushinteger(L, pop_size);   lua_setfield(L, -2, "population_size");
    lua_pushinteger(L, diff_factor); lua_setfield(L, -2, "maximum_diffusion");
    lua_pushnumber(L, walk);        lua_setfield(L, -2, "walk");
    lua_pushboolean(L, 1);          lua_setfield(L, -2, "bound_clipping");

    lua_pushvalue(L, -2);
    lua_setfield(L, -2, "fitness");
    lua_remove(L, -2);                                 /* [entry, cfg] */
    return 0;
}

/* ------------------------------------------------------------------ */
/* Top-k ranking: cosine distance from corpus rows to best_point      */
/* ------------------------------------------------------------------ */

static double
cosine_distance(const double *a, const double *b, int dim)
{
    double dot = 0, na = 0, nb = 0;
    for (int i = 0; i < dim; i++) {
        dot += a[i] * b[i];
        na  += a[i] * a[i];
        nb  += b[i] * b[i];
    }
    if (na == 0 || nb == 0) return 1.0;
    return 1.0 - dot / (sqrt(na) * sqrt(nb));
}

/* Partial selection of the k smallest distances. Naive O(N*k);
 * fine for the scales this UDF targets. */
static void
topk_by_distance(const double *store, size_t n_rows, int dim,
                 const double *best_point, int k,
                 int *out_idx, double *out_dist)
{
    for (int i = 0; i < k; i++) {
        out_idx[i] = -1;
        out_dist[i] = INFINITY;
    }
    for (size_t r = 0; r < n_rows; r++) {
        double d = cosine_distance(store + r * dim, best_point, dim);
        /* worst slot = largest dist in out_dist[] */
        int worst = 0;
        for (int i = 1; i < k; i++)
            if (out_dist[i] > out_dist[worst]) worst = i;
        if (d < out_dist[worst]) {
            out_dist[worst] = d;
            out_idx[worst] = (int) r;
        }
    }
    /* Simple insertion sort ascending by dist — k is small. */
    for (int i = 1; i < k; i++) {
        double d = out_dist[i]; int idx = out_idx[i];
        int j = i - 1;
        while (j >= 0 && out_dist[j] > d) {
            out_dist[j+1] = out_dist[j]; out_idx[j+1] = out_idx[j]; j--;
        }
        out_dist[j+1] = d; out_idx[j+1] = idx;
    }
}

/* ------------------------------------------------------------------ */
/* Result buffer emission                                             */
/* ------------------------------------------------------------------ */

static bool
ensure_result_cap(sfs_ctx *c, size_t need)
{
    if (c->result_cap >= need) return true;
    size_t ncap = c->result_cap ? c->result_cap : 512;
    while (ncap < need) ncap *= 2;
    char *nb = realloc(c->result_buf, ncap);
    if (nb == NULL) return false;
    c->result_buf = nb;
    c->result_cap = ncap;
    return true;
}

static bool
append_fmt(sfs_ctx *c, size_t *off, const char *fmt, ...)
{
    va_list ap;
    for (;;) {
        va_start(ap, fmt);
        int w = vsnprintf(c->result_buf + *off, c->result_cap - *off, fmt, ap);
        va_end(ap);
        if (w < 0) return false;
        if ((size_t) w < c->result_cap - *off) { *off += (size_t) w; return true; }
        if (!ensure_result_cap(c, c->result_cap + (size_t) w + 64)) return false;
    }
}

/* ------------------------------------------------------------------ */
/* UDF triad: fractal_search                                          */
/* ------------------------------------------------------------------ */

FRACTAL_EXPORT bool
fractal_search_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
    sfs_ctx *c;

    if (args->arg_count != 4) {
        SFS_INIT_ERROR(message,
            "fractal_search(vector_csv, query_csv, k, params): "
            "expected 4 arguments, got %u", args->arg_count);
        return true;
    }

    args->arg_type[0] = STRING_RESULT;
    args->arg_type[1] = STRING_RESULT;
    args->arg_type[2] = INT_RESULT;
    args->arg_type[3] = STRING_RESULT;

    if (args->args[2] != NULL) {
        long long kv = *(long long *) args->args[2];
        if (kv < 1 || kv > 1000000) {
            SFS_INIT_ERROR(message, "fractal_search: k must be 1..1000000");
            return true;
        }
    }

    c = calloc(1, sizeof(*c));
    if (c == NULL) {
        SFS_INIT_ERROR(message, "fractalsql: out of memory");
        return true;
    }
    c->module_ref = LUA_NOREF;

    if (!load_module(c, message)) {
        if (c->L) lua_close(c->L);
        free(c);
        return true;
    }

    initid->ptr        = (char *) c;
    initid->maybe_null = 1;
    initid->max_length = 64 * 1024 * 1024;
    return false;
}

FRACTAL_EXPORT void
fractal_search_deinit(UDF_INIT *initid)
{
    sfs_ctx *c = (sfs_ctx *) initid->ptr;
    if (c == NULL) return;
    if (c->L) lua_close(c->L);
    free(c->result_buf);
    free(c);
    initid->ptr = NULL;
}

FRACTAL_EXPORT char *
fractal_search(UDF_INIT *initid, UDF_ARGS *args, char *result,
               unsigned long *length, char *is_null, char *error)
{
    sfs_ctx   *c = (sfs_ctx *) initid->ptr;
    lua_State *L = c->L;
    char       errbuf[MYSQL_ERRMSG_SIZE];

    double   *query = NULL;
    double   *corpus = NULL;
    size_t    dim = 0, corpus_dim = 0, n_corpus = 0;
    int       k, iterations, pop_size, diff_factor, saved_top, rc, i;
    double    walk;
    bool      debug_mode;
    const char *params_s;
    size_t     params_len;

    (void) result;

    if (args->args[1] == NULL || args->args[2] == NULL) {
        *is_null = 1;
        return NULL;
    }

    /* --- query (required) ------------------------------------------ */
    if (!parse_vector_csv(args->args[1], args->lengths[1],
                          &query, &dim, errbuf)) {
        *error = 1;
        return NULL;
    }

    /* --- corpus (may be empty) ------------------------------------- */
    if (args->args[0] != NULL && args->lengths[0] > 0) {
        if (!parse_corpus(args->args[0], args->lengths[0], dim,
                          &corpus, &n_corpus, &corpus_dim, errbuf)) {
            free(query);
            *error = 1;
            return NULL;
        }
        if (n_corpus > 0 && corpus_dim != dim) {
            free(query); free(corpus);
            *error = 1;
            return NULL;
        }
    }

    /* --- k ---------------------------------------------------------- */
    k = (int) *(long long *) args->args[2];
    if (n_corpus > 0 && (size_t) k > n_corpus) k = (int) n_corpus;

    /* --- params ----------------------------------------------------- */
    params_s   = (args->args[3] != NULL) ? args->args[3] : "{}";
    params_len = (args->args[3] != NULL) ? args->lengths[3] : 2;

    iterations  = json_get_int   (params_s, params_len, "iterations",       30);
    pop_size    = json_get_int   (params_s, params_len, "population_size",  50);
    diff_factor = json_get_int   (params_s, params_len, "diffusion_factor", 2);
    walk        = json_get_double(params_s, params_len, "walk",             0.5);
    debug_mode  = json_get_bool  (params_s, params_len, "debug",            false);

    if (iterations < 1 || iterations > 100000 ||
        pop_size   < 2 || pop_size   > 10000  ||
        diff_factor < 1 || diff_factor > 100) {
        free(query); free(corpus);
        *error = 1;
        return NULL;
    }

    /* --- SFS call --------------------------------------------------- */
    saved_top = lua_gettop(L);

    rc = prepare_call(L, c->module_ref,
                      debug_mode ? "run_debug" : "run",
                      query, (int) dim,
                      iterations, pop_size, diff_factor, walk);
    if (rc != 0) {
        lua_settop(L, saved_top);
        free(query); free(corpus);
        *error = 1;
        return NULL;
    }

    int nresults = debug_mode ? 3 : 4;
    rc = lua_pcall(L, 1, nresults, 0);
    if (rc != 0) {
        lua_settop(L, saved_top);
        free(query); free(corpus);
        *error = 1;
        return NULL;
    }

    /* Stack: [best_point, best_fit, ...] */
    int bp_idx = saved_top + 1;
    int bf_idx = saved_top + 2;
    int trace_idx = saved_top + 3;   /* only valid in debug_mode */

    /* --- extract best_point to C ------------------------------------ */
    double *best_point = malloc(dim * sizeof(double));
    if (best_point == NULL) {
        lua_settop(L, saved_top);
        free(query); free(corpus);
        *error = 1;
        return NULL;
    }
    for (i = 0; i < (int) dim; i++) {
        lua_rawgeti(L, bp_idx, i + 1);
        best_point[i] = lua_tonumber(L, -1);
        lua_pop(L, 1);
    }
    double best_fit = lua_tonumber(L, bf_idx);

    /* --- top-k against best_point ----------------------------------- */
    int    *top_idx  = NULL;
    double *top_dist = NULL;
    int     k_out    = 0;
    if (n_corpus > 0 && k > 0) {
        k_out = k;
        top_idx  = malloc(k_out * sizeof(int));
        top_dist = malloc(k_out * sizeof(double));
        if (!top_idx || !top_dist) {
            free(top_idx); free(top_dist);
            lua_settop(L, saved_top);
            free(query); free(corpus); free(best_point);
            *error = 1;
            return NULL;
        }
        topk_by_distance(corpus, n_corpus, (int) dim, best_point,
                         k_out, top_idx, top_dist);
    }

    /* --- emit JSON --------------------------------------------------- */
    size_t off = 0;
    if (!ensure_result_cap(c, 1024)) goto oom;
    if (!append_fmt(c, &off, "{\"dim\":%d,\"n_corpus\":%zu,\"best_fit\":%.17g,\"best_point\":[",
                    (int) dim, n_corpus, best_fit)) goto oom;
    for (i = 0; i < (int) dim; i++)
        if (!append_fmt(c, &off, "%s%.17g",
                        (i == 0 ? "" : ","), best_point[i])) goto oom;
    if (!append_fmt(c, &off, "]")) goto oom;

    if (k_out > 0) {
        if (!append_fmt(c, &off, ",\"top_k\":[")) goto oom;
        for (i = 0; i < k_out; i++) {
            if (top_idx[i] < 0) continue;
            if (!append_fmt(c, &off, "%s{\"idx\":%d,\"dist\":%.17g}",
                            (i == 0 ? "" : ","),
                            top_idx[i], top_dist[i])) goto oom;
        }
        if (!append_fmt(c, &off, "]")) goto oom;
    }

    if (debug_mode) {
        size_t jlen = 0;
        const char *js = lua_tolstring(L, trace_idx, &jlen);
        if (js != NULL && jlen > 0) {
            if (!append_fmt(c, &off, ",\"trace\":")) goto oom;
            if (!ensure_result_cap(c, off + jlen + 2)) goto oom;
            memcpy(c->result_buf + off, js, jlen);
            off += jlen;
        }
    }

    if (!append_fmt(c, &off, "}")) goto oom;

    *length = (unsigned long) off;
    *is_null = 0;

    lua_settop(L, saved_top);
    free(query); free(corpus); free(best_point);
    free(top_idx); free(top_dist);
    return c->result_buf;

oom:
    lua_settop(L, saved_top);
    free(query); free(corpus); free(best_point);
    free(top_idx); free(top_dist);
    *error = 1;
    return NULL;
}

/* ------------------------------------------------------------------ */
/* UDF triad: fractalsql_edition                                      */
/* ------------------------------------------------------------------ */

FRACTAL_EXPORT bool
fractalsql_edition_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
    if (args->arg_count != 0) {
        SFS_INIT_ERROR(message, "fractalsql_edition(): expected 0 arguments");
        return true;
    }
    initid->maybe_null = 0;
    initid->max_length = 32;
    return false;
}

FRACTAL_EXPORT void
fractalsql_edition_deinit(UDF_INIT *initid)
{
    (void) initid;
}

FRACTAL_EXPORT char *
fractalsql_edition(UDF_INIT *initid, UDF_ARGS *args, char *result,
                   unsigned long *length, char *is_null, char *error)
{
    static const char kEdition[] = "Community";
    (void) initid; (void) args; (void) error;
    memcpy(result, kEdition, sizeof(kEdition) - 1);
    *length  = (unsigned long)(sizeof(kEdition) - 1);
    *is_null = 0;
    return result;
}

/* ------------------------------------------------------------------ */
/* UDF triad: fractalsql_version                                      */
/* ------------------------------------------------------------------ */

FRACTAL_EXPORT bool
fractalsql_version_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
    if (args->arg_count != 0) {
        SFS_INIT_ERROR(message, "fractalsql_version(): expected 0 arguments");
        return true;
    }
    initid->maybe_null = 0;
    initid->max_length = 32;
    return false;
}

FRACTAL_EXPORT void
fractalsql_version_deinit(UDF_INIT *initid)
{
    (void) initid;
}

FRACTAL_EXPORT char *
fractalsql_version(UDF_INIT *initid, UDF_ARGS *args, char *result,
                   unsigned long *length, char *is_null, char *error)
{
    static const char kVersion[] = "1.0.0";
    (void) initid; (void) args; (void) error;
    memcpy(result, kVersion, sizeof(kVersion) - 1);
    *length  = (unsigned long)(sizeof(kVersion) - 1);
    *is_null = 0;
    return result;
}
