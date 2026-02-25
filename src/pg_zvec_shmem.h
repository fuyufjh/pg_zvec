#ifndef PG_ZVEC_SHMEM_H
#define PG_ZVEC_SHMEM_H

#include "postgres.h"
#include "port/atomics.h"
#include "storage/dsm_impl.h"
#include "storage/lwlock.h"
#include <string.h>

/* ----------------------------------------------------------------
 * Compile-time limits
 * ---------------------------------------------------------------- */
#define ZVEC_MAX_COLLECTIONS    64
#define ZVEC_MAX_NAME_LEN       128
#define ZVEC_MAX_PATH_LEN       512
#define ZVEC_MAX_PARAMS_LEN     1024

/* ----------------------------------------------------------------
 * Per-backend shm_mq session limits and DSM sizes
 *
 * Each backend that talks to the worker creates a DSM segment
 * containing two shm_mq queues (request + response).  The session
 * directory in PgZvecSharedState tracks active DSM handles.
 * ---------------------------------------------------------------- */
#define ZVEC_MAX_SESSIONS       64
#define ZVEC_REQ_QUEUE_SIZE     (64 * 1024)         /* 64 KB  */
#define ZVEC_RESP_QUEUE_SIZE    (4 * 1024 * 1024)   /* 4 MB   */

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
     * ZVEC_REQ_SCAN — full-table scan via the worker.
     *
     * Request payload: [col_name\0][max_rows: int32][dimension: int32]
     *                  [n_scalar_fields: int32][field_name\0]...
     *
     * Response payload (via resp_mq):
     *   [nrows: int32][n_scalar: int32]
     *   [pks: nrows × char[256]]
     *   [vecs: nrows × dimension × float32]
     *   [scalar_data: (has_val:uint8 [string\0])... ]
     */
    ZVEC_REQ_SCAN              = 7,
} ZvecRequestType;

/* ----------------------------------------------------------------
 * Wire-format message headers (sent through shm_mq)
 * ---------------------------------------------------------------- */

/* Request header: backend → worker via req_mq.
 * Followed by data_len bytes of payload (same pack/unpack format). */
typedef struct ZvecMsgHeader
{
    ZvecRequestType  type;
    Oid              database_id;
    int32            data_len;       /* bytes following this header */
} ZvecMsgHeader;

/* Response header: worker → backend via resp_mq.
 * Followed by data_len bytes:
 *   - On error:        null-terminated error string
 *   - On SCAN success: binary scan payload
 *   - On other success: empty (data_len == 0)
 */
typedef struct ZvecRespHeader
{
    bool    success;
    int32   data_len;
} ZvecRespHeader;

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
    char    vec_type[16];       /* "vector_fp32" | "vector_fp16" */
    int32   dimension;
} ZvecCollectionEntry;

/* ----------------------------------------------------------------
 * Session directory entry (one per connected backend)
 *
 * Each backend that needs IPC creates a DSM segment with a pair
 * of shm_mq queues and registers here so the worker can discover it.
 * ---------------------------------------------------------------- */
typedef struct ZvecSessionSlot
{
    bool        in_use;
    pid_t       backend_pid;
    dsm_handle  handle;         /* DSM segment: req_mq + resp_mq */
} ZvecSessionSlot;

/* ----------------------------------------------------------------
 * Global shared state
 * ---------------------------------------------------------------- */
typedef struct PgZvecSharedState
{
    LWLock             *lock;           /* protects collections[] and sessions[] */

    /* Worker lifecycle */
    pid_t               worker_pid;
    bool                worker_ready;

    /* Collection registry */
    int                 num_collections;
    ZvecCollectionEntry collections[ZVEC_MAX_COLLECTIONS];

    /* Per-backend session directory */
    pg_atomic_uint32    session_version;   /* bumped on session add/remove */
    ZvecSessionSlot     sessions[ZVEC_MAX_SESSIONS];
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
