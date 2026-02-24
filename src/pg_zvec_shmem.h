#ifndef PG_ZVEC_SHMEM_H
#define PG_ZVEC_SHMEM_H

#include "postgres.h"
#include "storage/lwlock.h"
#include <string.h>

/* ----------------------------------------------------------------
 * Compile-time limits
 * ---------------------------------------------------------------- */
#define ZVEC_MAX_COLLECTIONS    64
#define ZVEC_MAX_NAME_LEN       128
#define ZVEC_MAX_PATH_LEN       512
#define ZVEC_MAX_PARAMS_LEN     1024
/*
 * 20 KB request payload: supports vectors up to ~5000 float32 dimensions
 * plus collection name, pk, and metadata.
 */
#define ZVEC_MAX_REQUEST_DATA   20480
#define ZVEC_MAX_RESPONSE_DATA  256     /* error message buffer           */

/* ----------------------------------------------------------------
 * Request types (backend → worker)
 * ---------------------------------------------------------------- */
typedef enum ZvecRequestType
{
    ZVEC_REQ_NONE              = 0,
    ZVEC_REQ_PING              = 1,
    ZVEC_REQ_CREATE_COLLECTION = 2,
    ZVEC_REQ_DROP_COLLECTION   = 3,
    ZVEC_REQ_INSERT            = 4,
    ZVEC_REQ_DELETE            = 5,
    ZVEC_REQ_OPTIMIZE          = 6,
    /*
     * ZVEC_REQ_SCAN — full-table scan via the worker (which holds the
     * write lock).  Results are written to a tmpfile whose path is
     * encoded in the request payload; backend reads and deletes it.
     *
     * Request payload: [col_name\0][max_rows: int32][dimension: int32]
     *                  [tmpfile_path\0]
     * Tmpfile layout:  [nrows: int32]
     *                  [pks: nrows × char[256]]
     *                  [vecs: nrows × dimension × float32]
     */
    ZVEC_REQ_SCAN              = 7,
} ZvecRequestType;

/* ----------------------------------------------------------------
 * Collection registry entry (lives in shared memory)
 * ---------------------------------------------------------------- */
typedef struct ZvecCollectionEntry
{
    bool    in_use;
    Oid     database_id;
    char    name[ZVEC_MAX_NAME_LEN];
    char    data_dir[ZVEC_MAX_PATH_LEN];
    char    index_type[32];     /* "hnsw" | "ivf" | "flat" */
    char    metric[16];         /* "l2"   | "ip"  | "cosine" */
    int32   dimension;
} ZvecCollectionEntry;

/* ----------------------------------------------------------------
 * Request struct: one backend posts this; worker reads it
 * ---------------------------------------------------------------- */
typedef struct ZvecRequest
{
    ZvecRequestType type;
    Oid             database_id;
    pid_t           sender_pid;                     /* backend to notify via SetLatch */
    int             data_len;
    char            data[ZVEC_MAX_REQUEST_DATA];    /* serialised payload */
} ZvecRequest;

/* ----------------------------------------------------------------
 * Response struct: worker writes this; backend reads it
 * ---------------------------------------------------------------- */
typedef struct ZvecResponse
{
    bool    success;
    char    error_msg[ZVEC_MAX_RESPONSE_DATA];
} ZvecResponse;

/* ----------------------------------------------------------------
 * Global shared state
 *
 * MVP: single-slot request/response (serialised, one at a time).
 * Phase 2 will replace with a proper per-backend shm_mq pair.
 * ---------------------------------------------------------------- */
typedef struct PgZvecSharedState
{
    LWLock             *lock;           /* pointer into named LWLock tranche  */

    /* Worker lifecycle */
    pid_t               worker_pid;
    bool                worker_ready;

    /* Collection registry */
    int                 num_collections;
    ZvecCollectionEntry collections[ZVEC_MAX_COLLECTIONS];

    /* Request / response slot */
    bool                request_pending;
    bool                response_ready;
    ZvecRequest         request;
    ZvecResponse        response;
} PgZvecSharedState;

/* Pointer to the shared state; set once during shmem startup */
extern PgZvecSharedState *pg_zvec_state;

/* Shared memory helpers */
Size pg_zvec_shmem_size(void);
void pg_zvec_shmem_startup(void);

/* ----------------------------------------------------------------
 * Flat-buffer pack / unpack helpers (inlined into each translation
 * unit that includes this header).
 *
 * All functions take and return a byte-offset cursor (pos).
 * They return -1 on overflow / format error.
 * ---------------------------------------------------------------- */

/* Pack a null-terminated string (including the '\0'). */
static inline int
zvec_pack_str(char *buf, int pos, int bufsz, const char *s)
{
    int n = (int) strlen(s) + 1;
    if (pos + n > bufsz)
        return -1;
    memcpy(buf + pos, s, n);
    return pos + n;
}

/* Pack a 32-bit integer in native byte order. */
static inline int
zvec_pack_int(char *buf, int pos, int bufsz, int val)
{
    if (pos + (int) sizeof(int) > bufsz)
        return -1;
    memcpy(buf + pos, &val, sizeof(int));
    return pos + (int) sizeof(int);
}

/* Pack an array of float32 values. */
static inline int
zvec_pack_floats(char *buf, int pos, int bufsz, const float *arr, int count)
{
    int n = count * (int) sizeof(float);
    if (pos + n > bufsz)
        return -1;
    memcpy(buf + pos, arr, n);
    return pos + n;
}

/* Unpack a null-terminated string. outsize must include room for '\0'. */
static inline int
zvec_unpack_str(const char *buf, int pos, int bufsz, char *out, int outsz)
{
    int rem = bufsz - pos;
    int n = 0;
    const char *src = buf + pos;

    while (n < rem && src[n] != '\0')
        n++;
    if (n == rem)
        return -1;  /* not null-terminated within bounds */
    n++;            /* include '\0' */
    if (n > outsz)
        return -1;  /* won't fit in destination */
    memcpy(out, src, n);
    return pos + n;
}

/* Unpack a 32-bit integer. */
static inline int
zvec_unpack_int(const char *buf, int pos, int bufsz, int *out)
{
    if (pos + (int) sizeof(int) > bufsz)
        return -1;
    memcpy(out, buf + pos, sizeof(int));
    return pos + (int) sizeof(int);
}

/* Unpack an array of float32 values into caller-allocated buffer. */
static inline int
zvec_unpack_floats(const char *buf, int pos, int bufsz, float *out, int count)
{
    int n = count * (int) sizeof(float);
    if (pos + n > bufsz)
        return -1;
    memcpy(out, buf + pos, n);
    return pos + n;
}

#endif /* PG_ZVEC_SHMEM_H */
