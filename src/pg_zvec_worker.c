#include "pg_zvec_fdw.h"
#include "pg_zvec_shmem.h"
#include "zvec_bridge/zvec_bridge.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "miscadmin.h"
#include "postmaster/bgworker.h"
#include "storage/dsm.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/lwlock.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "storage/shm_mq.h"
#include "utils/memutils.h"
#include "utils/ps_status.h"
#include "utils/wait_event.h"

/* ----------------------------------------------------------------
 * Worker-local collection registry
 *
 * The background worker is the only process that holds writable
 * ZvecCollectionHandle pointers.  This array is not in shared
 * memory; it lives in the worker's private address space.
 * ---------------------------------------------------------------- */
typedef struct WorkerCollection
{
    bool                  in_use;
    char                  name[ZVEC_MAX_NAME_LEN];
    ZvecCollectionHandle *handle;
} WorkerCollection;

static WorkerCollection worker_colls[ZVEC_MAX_COLLECTIONS];

static WorkerCollection *
worker_find_coll(const char *name)
{
    int i;
    for (i = 0; i < ZVEC_MAX_COLLECTIONS; i++)
        if (worker_colls[i].in_use &&
            strcmp(worker_colls[i].name, name) == 0)
            return &worker_colls[i];
    return NULL;
}

static WorkerCollection *
worker_alloc_coll(const char *name)
{
    int i;
    for (i = 0; i < ZVEC_MAX_COLLECTIONS; i++)
    {
        if (!worker_colls[i].in_use)
        {
            memset(&worker_colls[i], 0, sizeof(WorkerCollection));
            worker_colls[i].in_use = true;
            strlcpy(worker_colls[i].name, name, ZVEC_MAX_NAME_LEN);
            return &worker_colls[i];
        }
    }
    return NULL;
}

static void
worker_free_coll(WorkerCollection *wc)
{
    memset(wc, 0, sizeof(WorkerCollection));
}

/* ----------------------------------------------------------------
 * Shared-state registry helpers (called with lock held)
 * ---------------------------------------------------------------- */

/* Find a collection entry by name+database in shared memory. */
static ZvecCollectionEntry *
shmem_find_entry(const char *name, Oid dbid)
{
    int i;
    for (i = 0; i < ZVEC_MAX_COLLECTIONS; i++)
        if (pg_zvec_state->collections[i].in_use &&
            pg_zvec_state->collections[i].database_id == dbid &&
            strcmp(pg_zvec_state->collections[i].name, name) == 0)
            return &pg_zvec_state->collections[i];
    return NULL;
}

/* Allocate a free slot in shared memory. */
static ZvecCollectionEntry *
shmem_alloc_entry(void)
{
    int i;
    for (i = 0; i < ZVEC_MAX_COLLECTIONS; i++)
        if (!pg_zvec_state->collections[i].in_use)
            return &pg_zvec_state->collections[i];
    return NULL;
}

/* ----------------------------------------------------------------
 * Signal handling
 * ---------------------------------------------------------------- */
static volatile sig_atomic_t worker_shutdown_requested = false;

static void
worker_sigterm_handler(SIGNAL_ARGS)
{
    int save_errno = errno;
    worker_shutdown_requested = true;
    SetLatch(MyLatch);
    errno = save_errno;
}

/* ----------------------------------------------------------------
 * Per-backend shm_mq session tracking (worker side)
 *
 * The worker attaches to each backend's DSM segment and keeps
 * shm_mq_handle pointers for req/resp queues.
 * ---------------------------------------------------------------- */
typedef struct WorkerSession
{
    bool            active;
    pid_t           backend_pid;
    dsm_segment    *seg;
    shm_mq_handle  *req_mqh;
    shm_mq_handle  *resp_mqh;
} WorkerSession;

static WorkerSession worker_sessions[ZVEC_MAX_SESSIONS];
static uint32        known_session_version = 0;

/*
 * worker_refresh_sessions — scan the session directory and attach/detach
 * DSM segments as needed.
 */
static void
worker_refresh_sessions(void)
{
    int i;

    LWLockAcquire(pg_zvec_state->lock, LW_SHARED);

    for (i = 0; i < ZVEC_MAX_SESSIONS; i++)
    {
        ZvecSessionSlot *slot = &pg_zvec_state->sessions[i];

        if (slot->in_use && !worker_sessions[i].active)
        {
            /* New session — attach to its DSM segment */
            dsm_segment *seg;
            char        *base;
            shm_mq      *req_mq;
            shm_mq      *resp_mq;

            LWLockRelease(pg_zvec_state->lock);

            seg = dsm_attach(slot->handle);
            if (!seg)
            {
                ereport(LOG, (errmsg("pg_zvec worker: could not attach DSM for session %d", i)));
                LWLockAcquire(pg_zvec_state->lock, LW_SHARED);
                continue;
            }

            base    = (char *) dsm_segment_address(seg);
            req_mq  = (shm_mq *) base;
            resp_mq = (shm_mq *) (base + ZVEC_REQ_QUEUE_SIZE);

            shm_mq_set_receiver(req_mq,  MyProc);
            shm_mq_set_sender(resp_mq, MyProc);

            worker_sessions[i].active      = true;
            worker_sessions[i].backend_pid = slot->backend_pid;
            worker_sessions[i].seg         = seg;
            worker_sessions[i].req_mqh     = shm_mq_attach(req_mq,  seg, NULL);
            worker_sessions[i].resp_mqh    = shm_mq_attach(resp_mq, seg, NULL);

            ereport(LOG, (errmsg("pg_zvec worker: attached session %d (pid %d)",
                                 i, (int) slot->backend_pid)));
            LWLockAcquire(pg_zvec_state->lock, LW_SHARED);
        }
        else if (!slot->in_use && worker_sessions[i].active)
        {
            /* Session removed — detach */
            ereport(LOG, (errmsg("pg_zvec worker: detaching session %d", i)));
            shm_mq_detach(worker_sessions[i].req_mqh);
            shm_mq_detach(worker_sessions[i].resp_mqh);
            dsm_detach(worker_sessions[i].seg);
            memset(&worker_sessions[i], 0, sizeof(WorkerSession));
        }
    }

    LWLockRelease(pg_zvec_state->lock);
    known_session_version = pg_atomic_read_u32(&pg_zvec_state->session_version);
}

/* ----------------------------------------------------------------
 * send_response — send a ZvecRespHeader + optional payload to a session
 * ---------------------------------------------------------------- */
static void
send_response(WorkerSession *sess, bool success,
              const char *payload, int payload_len,
              const char *errmsg_str)
{
    ZvecRespHeader hdr;
    shm_mq_iovec   iov[2];
    int            niov = 1;

    memset(&hdr, 0, sizeof(hdr));
    hdr.success = success;

    if (!success && errmsg_str && errmsg_str[0] != '\0')
    {
        hdr.data_len = (int) strlen(errmsg_str) + 1;
        iov[0].data = (const char *) &hdr;
        iov[0].len  = sizeof(ZvecRespHeader);
        iov[1].data = errmsg_str;
        iov[1].len  = hdr.data_len;
        niov = 2;
    }
    else if (success && payload && payload_len > 0)
    {
        hdr.data_len = payload_len;
        iov[0].data = (const char *) &hdr;
        iov[0].len  = sizeof(ZvecRespHeader);
        iov[1].data = payload;
        iov[1].len  = payload_len;
        niov = 2;
    }
    else
    {
        hdr.data_len = 0;
        iov[0].data = (const char *) &hdr;
        iov[0].len  = sizeof(ZvecRespHeader);
    }

    shm_mq_sendv(sess->resp_mqh, iov, niov, false, true);
}
/* ----------------------------------------------------------------
 * process_request — handle one request from a session.
 *
 * msg points to the raw shm_mq message: [ZvecMsgHeader][payload].
 * Response is sent via send_response() on the session's resp_mq.
 * ---------------------------------------------------------------- */
static void
process_request(WorkerSession *sess, const char *msg, Size msg_len)
{
    ZvecMsgHeader   hdr;
    const char     *data;
    int             data_len;
    char            err[256];

    if (msg_len < sizeof(ZvecMsgHeader))
    {
        send_response(sess, false, NULL, 0, "truncated request header");
        return;
    }

    memcpy(&hdr, msg, sizeof(ZvecMsgHeader));
    data     = msg + sizeof(ZvecMsgHeader);
    data_len = hdr.data_len;

    err[0] = '\0';

    switch (hdr.type)
    {
        /* --------------------------------------------------------
         * PING
         * -------------------------------------------------------- */
        case ZVEC_REQ_PING:
            send_response(sess, true, NULL, 0, NULL);
            break;

        /* --------------------------------------------------------
         * CREATE_COLLECTION
         * -------------------------------------------------------- */
        case ZVEC_REQ_CREATE_COLLECTION:
        {
            char col_name[ZVEC_MAX_NAME_LEN];
            char data_dir_buf[ZVEC_MAX_PATH_LEN];
            char index_type[32];
            char metric[32];
            int  dimension;
            char vec_type[32];
            char params_json[ZVEC_MAX_PARAMS_LEN];
            int  pos = 0;
            int  n_scalar_fields = 0;
            char scalar_names_buf[ZVEC_MAX_SCALAR_FIELDS][64];
            const char *scalar_names_ptrs[ZVEC_MAX_SCALAR_FIELDS];
            int  si;
            ZvecCollectionHandle *h;
            WorkerCollection     *wc;
            ZvecCollectionEntry  *entry;

            pos = zvec_unpack_str(data, pos, data_len, col_name, sizeof(col_name));
            if (pos < 0) goto create_err;
            pos = zvec_unpack_str(data, pos, data_len, data_dir_buf, sizeof(data_dir_buf));
            if (pos < 0) goto create_err;
            pos = zvec_unpack_str(data, pos, data_len, index_type, sizeof(index_type));
            if (pos < 0) goto create_err;
            pos = zvec_unpack_str(data, pos, data_len, metric, sizeof(metric));
            if (pos < 0) goto create_err;
            pos = zvec_unpack_int(data, pos, data_len, &dimension);
            if (pos < 0) goto create_err;
            pos = zvec_unpack_str(data, pos, data_len, vec_type, sizeof(vec_type));
            if (pos < 0) goto create_err;
            pos = zvec_unpack_str(data, pos, data_len, params_json, sizeof(params_json));
            if (pos < 0) goto create_err;

            if (pos < data_len)
            {
                pos = zvec_unpack_int(data, pos, data_len, &n_scalar_fields);
                if (pos < 0) n_scalar_fields = 0;
                if (n_scalar_fields > ZVEC_MAX_SCALAR_FIELDS)
                    n_scalar_fields = ZVEC_MAX_SCALAR_FIELDS;
                for (si = 0; si < n_scalar_fields; si++)
                {
                    pos = zvec_unpack_str(data, pos, data_len,
                                          scalar_names_buf[si], sizeof(scalar_names_buf[si]));
                    if (pos < 0) { n_scalar_fields = si; break; }
                    scalar_names_ptrs[si] = scalar_names_buf[si];
                }
            }

            LWLockAcquire(pg_zvec_state->lock, LW_SHARED);
            entry = shmem_find_entry(col_name, hdr.database_id);
            LWLockRelease(pg_zvec_state->lock);
            if (entry)
            {
                snprintf(err, sizeof(err), "collection \"%s\" already exists", col_name);
                send_response(sess, false, NULL, 0, err);
                break;
            }

            h = zvec_collection_create(data_dir_buf, index_type, metric,
                                       dimension, vec_type, params_json,
                                       n_scalar_fields, scalar_names_ptrs,
                                       err, sizeof(err));
            if (!h)
            {
                send_response(sess, false, NULL, 0, err);
                break;
            }

            wc = worker_alloc_coll(col_name);
            if (!wc)
            {
                zvec_collection_close(h);
                send_response(sess, false, NULL, 0, "too many open collections");
                break;
            }
            wc->handle = h;

            LWLockAcquire(pg_zvec_state->lock, LW_EXCLUSIVE);
            entry = shmem_alloc_entry();
            if (!entry)
            {
                LWLockRelease(pg_zvec_state->lock);
                zvec_collection_close(h);
                worker_free_coll(wc);
                send_response(sess, false, NULL, 0,
                              "shared-memory collection registry is full");
                break;
            }
            entry->in_use      = true;
            entry->database_id = hdr.database_id;
            strlcpy(entry->name,       col_name,     sizeof(entry->name));
            strlcpy(entry->data_dir,   data_dir_buf, sizeof(entry->data_dir));
            strlcpy(entry->index_type, index_type,   sizeof(entry->index_type));
            strlcpy(entry->metric,     metric,       sizeof(entry->metric));
            strlcpy(entry->vec_type,   vec_type,     sizeof(entry->vec_type));
            entry->dimension = dimension;
            pg_zvec_state->num_collections++;
            LWLockRelease(pg_zvec_state->lock);

            ereport(LOG, (errmsg("pg_zvec: created collection \"%s\" at \"%s\"",
                                  col_name, data_dir_buf)));
            send_response(sess, true, NULL, 0, NULL);
            break;

        create_err:
            send_response(sess, false, NULL, 0, "malformed CREATE_COLLECTION payload");
            break;
        }

        /* --------------------------------------------------------
         * DROP_COLLECTION
         * -------------------------------------------------------- */
        case ZVEC_REQ_DROP_COLLECTION:
        {
            char col_name[ZVEC_MAX_NAME_LEN];
            int  pos = 0;
            WorkerCollection    *wc;
            ZvecCollectionEntry *entry;

            pos = zvec_unpack_str(data, pos, data_len, col_name, sizeof(col_name));
            if (pos < 0)
            {
                send_response(sess, false, NULL, 0, "malformed DROP_COLLECTION payload");
                break;
            }

            wc = worker_find_coll(col_name);
            if (!wc)
            {
                snprintf(err, sizeof(err), "collection \"%s\" not found", col_name);
                send_response(sess, false, NULL, 0, err);
                break;
            }

            zvec_collection_flush(wc->handle, err, sizeof(err));
            if (!zvec_collection_destroy(wc->handle, err, sizeof(err)))
            {
                send_response(sess, false, NULL, 0, err);
                zvec_collection_close(wc->handle);
                worker_free_coll(wc);
                goto drop_deregister;
            }
            zvec_collection_close(wc->handle);
            worker_free_coll(wc);

        drop_deregister:
            LWLockAcquire(pg_zvec_state->lock, LW_EXCLUSIVE);
            entry = shmem_find_entry(col_name, hdr.database_id);
            if (entry)
            {
                memset(entry, 0, sizeof(ZvecCollectionEntry));
                pg_zvec_state->num_collections--;
            }
            LWLockRelease(pg_zvec_state->lock);

            ereport(LOG, (errmsg("pg_zvec: dropped collection \"%s\"", col_name)));
            send_response(sess, true, NULL, 0, NULL);
            break;
        }

        /* --------------------------------------------------------
         * INSERT (upsert)
         * -------------------------------------------------------- */
        case ZVEC_REQ_INSERT:
        {
            char  col_name[ZVEC_MAX_NAME_LEN];
            char  pk[256];
            int   vec_len;
            float *vec = NULL;
            int   pos = 0;
            int   n_scalars = 0;
            char  scalar_names_buf[ZVEC_MAX_SCALAR_FIELDS][64];
            char  scalar_values_buf[ZVEC_MAX_SCALAR_FIELDS][4096];
            const char *scalar_names_ptrs[ZVEC_MAX_SCALAR_FIELDS];
            const char *scalar_values_ptrs[ZVEC_MAX_SCALAR_FIELDS];
            int   si;
            WorkerCollection *wc;

            pos = zvec_unpack_str(data, pos, data_len, col_name, sizeof(col_name));
            if (pos < 0) goto insert_err;
            pos = zvec_unpack_str(data, pos, data_len, pk, sizeof(pk));
            if (pos < 0) goto insert_err;
            pos = zvec_unpack_int(data, pos, data_len, &vec_len);
            if (pos < 0) goto insert_err;
            if (vec_len <= 0 || vec_len > 65536) goto insert_err;

            vec = (float *) palloc(vec_len * sizeof(float));
            pos = zvec_unpack_floats(data, pos, data_len, vec, vec_len);
            if (pos < 0) goto insert_err;

            if (pos < data_len)
            {
                pos = zvec_unpack_int(data, pos, data_len, &n_scalars);
                if (pos < 0) n_scalars = 0;
                if (n_scalars > ZVEC_MAX_SCALAR_FIELDS)
                    n_scalars = ZVEC_MAX_SCALAR_FIELDS;
                for (si = 0; si < n_scalars; si++)
                {
                    int is_null_int = 0;
                    pos = zvec_unpack_str(data, pos, data_len,
                                          scalar_names_buf[si], sizeof(scalar_names_buf[si]));
                    if (pos < 0) { n_scalars = si; break; }
                    scalar_names_ptrs[si] = scalar_names_buf[si];

                    pos = zvec_unpack_int(data, pos, data_len, &is_null_int);
                    if (pos < 0) { n_scalars = si; break; }

                    if (is_null_int)
                    {
                        scalar_values_ptrs[si] = NULL;
                    }
                    else
                    {
                        pos = zvec_unpack_str(data, pos, data_len,
                                              scalar_values_buf[si], sizeof(scalar_values_buf[si]));
                        if (pos < 0) { n_scalars = si; break; }
                        scalar_values_ptrs[si] = scalar_values_buf[si];
                    }
                }
            }

            wc = worker_find_coll(col_name);
            if (!wc)
            {
                snprintf(err, sizeof(err), "collection \"%s\" not found", col_name);
                send_response(sess, false, NULL, 0, err);
                break;
            }

            if (!zvec_collection_upsert(wc->handle, pk, vec, vec_len,
                                         n_scalars, scalar_names_ptrs,
                                         scalar_values_ptrs,
                                         err, sizeof(err)))
            {
                send_response(sess, false, NULL, 0, err);
            }
            else
            {
                send_response(sess, true, NULL, 0, NULL);
            }
            break;

        insert_err:
            send_response(sess, false, NULL, 0, "malformed INSERT payload");
            break;
        }

        /* --------------------------------------------------------
         * DELETE
         * -------------------------------------------------------- */
        case ZVEC_REQ_DELETE:
        {
            char col_name[ZVEC_MAX_NAME_LEN];
            char pk[256];
            int  pos = 0;
            WorkerCollection *wc;

            pos = zvec_unpack_str(data, pos, data_len, col_name, sizeof(col_name));
            if (pos < 0) goto delete_err;
            pos = zvec_unpack_str(data, pos, data_len, pk, sizeof(pk));
            if (pos < 0) goto delete_err;

            wc = worker_find_coll(col_name);
            if (!wc)
            {
                snprintf(err, sizeof(err), "collection \"%s\" not found", col_name);
                send_response(sess, false, NULL, 0, err);
                break;
            }

            if (!zvec_collection_delete(wc->handle, pk, err, sizeof(err)))
                send_response(sess, false, NULL, 0, err);
            else
                send_response(sess, true, NULL, 0, NULL);
            break;

        delete_err:
            send_response(sess, false, NULL, 0, "malformed DELETE payload");
            break;
        }

        /* --------------------------------------------------------
         * OPTIMIZE
         * -------------------------------------------------------- */
        case ZVEC_REQ_OPTIMIZE:
        {
            char col_name[ZVEC_MAX_NAME_LEN];
            int  pos = 0;
            WorkerCollection *wc;

            pos = zvec_unpack_str(data, pos, data_len, col_name, sizeof(col_name));
            if (pos < 0)
            {
                send_response(sess, false, NULL, 0, "malformed OPTIMIZE payload");
                break;
            }

            wc = worker_find_coll(col_name);
            if (!wc)
            {
                snprintf(err, sizeof(err), "collection \"%s\" not found", col_name);
                send_response(sess, false, NULL, 0, err);
                break;
            }

            set_ps_display("optimizing collection");
            if (!zvec_collection_optimize(wc->handle, err, sizeof(err)))
                send_response(sess, false, NULL, 0, err);
            else
                send_response(sess, true, NULL, 0, NULL);
            break;
        }

        /* --------------------------------------------------------
         * SCAN — results returned inline via resp_mq.
         *
         * Request:  [name\0][max_rows:int32][dimension:int32]
         *           [n_scalar_fields:int32][field_name\0]...
         *
         * Response payload:
         *   [nrows:int32][n_scalar:int32]
         *   [pks: nrows*256][vecs: nrows*dim*float]
         *   [scalar_data: (has_val:uint8 [string\0])... ]
         * -------------------------------------------------------- */
        case ZVEC_REQ_SCAN:
        {
            char   col_name[ZVEC_MAX_NAME_LEN];
            int    max_rows;
            int    dimension;
            int    pos = 0;
            int    n_scalar_fields = 0;
            char   scalar_names_buf[ZVEC_MAX_SCALAR_FIELDS][64];
            const char *scalar_names_ptrs[ZVEC_MAX_SCALAR_FIELDS];
            int    si;
            WorkerCollection *wc;
            char  (*pks)[256];
            float *vecs;
            int    nrows;

            pos = zvec_unpack_str(data, pos, data_len, col_name, sizeof(col_name));
            if (pos < 0) goto scan_err;
            pos = zvec_unpack_int(data, pos, data_len, &max_rows);
            if (pos < 0) goto scan_err;
            pos = zvec_unpack_int(data, pos, data_len, &dimension);
            if (pos < 0) goto scan_err;

            if (pos < data_len)
            {
                pos = zvec_unpack_int(data, pos, data_len, &n_scalar_fields);
                if (pos < 0) n_scalar_fields = 0;
                if (n_scalar_fields > ZVEC_MAX_SCALAR_FIELDS)
                    n_scalar_fields = ZVEC_MAX_SCALAR_FIELDS;
                for (si = 0; si < n_scalar_fields; si++)
                {
                    pos = zvec_unpack_str(data, pos, data_len,
                                          scalar_names_buf[si], sizeof(scalar_names_buf[si]));
                    if (pos < 0) { n_scalar_fields = si; break; }
                    scalar_names_ptrs[si] = scalar_names_buf[si];
                }
            }

            wc = worker_find_coll(col_name);
            if (!wc)
            {
                snprintf(err, sizeof(err), "collection \"%s\" not found", col_name);
                send_response(sess, false, NULL, 0, err);
                break;
            }

            if (max_rows > ZVEC_SCAN_MAX_ROWS)
                max_rows = ZVEC_SCAN_MAX_ROWS;
            if (max_rows <= 0 || dimension <= 0)
            {
                /* Empty result: [nrows=0][n_scalar] */
                char empty_buf[8];
                int  epos = 0;
                epos = zvec_pack_int(empty_buf, epos, sizeof(empty_buf), 0);
                epos = zvec_pack_int(empty_buf, epos, sizeof(empty_buf), n_scalar_fields);
                send_response(sess, true, empty_buf, epos, NULL);
                break;
            }

            pks  = (char (*)[256]) palloc(max_rows * 256);
            vecs = (float *) palloc((size_t) max_rows * dimension * sizeof(float));

            nrows = zvec_collection_scan_all(wc->handle, max_rows, dimension,
                                              pks, vecs, err, sizeof(err));
            if (nrows < 0)
            {
                send_response(sess, false, NULL, 0, err);
                pfree(pks);
                pfree(vecs);
                break;
            }

            /* Build response payload in memory */
            {
                Size pk_size   = (Size) nrows * 256;
                Size vec_size  = (Size) nrows * dimension * sizeof(float);
                char *resp_buf;
                int   rpos = 0;
                int   resp_bufsz;
                char *scalar_membuf = NULL;
                size_t scalar_memsize = 0;

                /* Compute scalar data size via open_memstream */
                if (n_scalar_fields > 0 && nrows > 0)
                {
                    FILE *memfp = open_memstream(&scalar_membuf, &scalar_memsize);
                    if (memfp)
                    {
                        zvec_collection_write_scalars(wc->handle, memfp,
                                                       nrows, pks,
                                                       n_scalar_fields, scalar_names_ptrs,
                                                       err, sizeof(err));
                        fclose(memfp);
                    }
                }

                resp_bufsz = (int)(sizeof(int) * 2 + pk_size + vec_size + scalar_memsize);
                resp_buf = (char *) palloc(resp_bufsz);

                rpos = zvec_pack_int(resp_buf, rpos, resp_bufsz, nrows);
                rpos = zvec_pack_int(resp_buf, rpos, resp_bufsz, n_scalar_fields);
                memcpy(resp_buf + rpos, pks, pk_size);
                rpos += (int) pk_size;
                memcpy(resp_buf + rpos, vecs, vec_size);
                rpos += (int) vec_size;

                if (scalar_membuf && scalar_memsize > 0)
                {
                    memcpy(resp_buf + rpos, scalar_membuf, scalar_memsize);
                    rpos += (int) scalar_memsize;
                }
                if (scalar_membuf)
                    free(scalar_membuf);

                send_response(sess, true, resp_buf, rpos, NULL);
                pfree(resp_buf);
            }

            pfree(pks);
            pfree(vecs);
            break;

        scan_err:
            send_response(sess, false, NULL, 0, "malformed SCAN payload");
            break;
        }

        default:
            snprintf(err, sizeof(err), "unknown request type %d", (int) hdr.type);
            send_response(sess, false, NULL, 0, err);
            break;
    }
}

/* ----------------------------------------------------------------
 * Worker entry point
 * ---------------------------------------------------------------- */
void
pg_zvec_worker_main(Datum main_arg)
{
    int i;

    /* Set up signal handlers before unblocking signals */
    pqsignal(SIGTERM, worker_sigterm_handler);
    BackgroundWorkerUnblockSignals();

    ereport(LOG, (errmsg("pg_zvec worker started")));

    memset(worker_colls, 0, sizeof(worker_colls));
    memset(worker_sessions, 0, sizeof(worker_sessions));

    /* Announce ourselves in shared memory */
    LWLockAcquire(pg_zvec_state->lock, LW_EXCLUSIVE);
    pg_zvec_state->worker_pid   = MyProcPid;
    pg_zvec_state->worker_ready = true;
    LWLockRelease(pg_zvec_state->lock);

    set_ps_display("idle");

    /* ----------------------------------------------------------------
     * Main loop: poll all active sessions for requests.
     *
     * Backends wake us via SetLatch (done automatically by shm_mq when
     * they send a message).  We also wake periodically to discover
     * new/removed sessions and do housekeeping.
     * ---------------------------------------------------------------- */
    while (!worker_shutdown_requested)
    {
        int  rc;
        bool did_work = false;

        rc = WaitLatch(MyLatch,
                       WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
                       1000L,      /* 1 s housekeeping timeout */
                       PG_WAIT_EXTENSION);
        ResetLatch(MyLatch);

        if (rc & WL_EXIT_ON_PM_DEATH)
            break;

        if (worker_shutdown_requested)
            break;

        /* Refresh session list if anything changed */
        if (pg_atomic_read_u32(&pg_zvec_state->session_version) != known_session_version)
            worker_refresh_sessions();

        /* Poll all active sessions (nowait) */
        for (i = 0; i < ZVEC_MAX_SESSIONS; i++)
        {
            shm_mq_result  res;
            Size           nbytes;
            void          *data;

            if (!worker_sessions[i].active)
                continue;

            res = shm_mq_receive(worker_sessions[i].req_mqh, &nbytes, &data, true);

            if (res == SHM_MQ_SUCCESS)
            {
                set_ps_display("processing request");
                process_request(&worker_sessions[i], (const char *) data, nbytes);
                set_ps_display("idle");
                did_work = true;
            }
            else if (res == SHM_MQ_DETACHED)
            {
                ereport(LOG, (errmsg("pg_zvec worker: session %d detached", i)));
                dsm_detach(worker_sessions[i].seg);
                memset(&worker_sessions[i], 0, sizeof(WorkerSession));
            }
            /* SHM_MQ_WOULD_BLOCK — no message, skip */
        }

        /* If we did work, loop immediately without sleeping */
        if (did_work)
        {
            SetLatch(MyLatch);
            continue;
        }
    }

    /* ----------------------------------------------------------------
     * Shutdown: detach sessions, flush and close all open collections
     * ---------------------------------------------------------------- */
    ereport(LOG, (errmsg("pg_zvec worker shutting down")));

    for (i = 0; i < ZVEC_MAX_SESSIONS; i++)
    {
        if (worker_sessions[i].active)
        {
            shm_mq_detach(worker_sessions[i].req_mqh);
            shm_mq_detach(worker_sessions[i].resp_mqh);
            dsm_detach(worker_sessions[i].seg);
            memset(&worker_sessions[i], 0, sizeof(WorkerSession));
        }
    }

    for (i = 0; i < ZVEC_MAX_COLLECTIONS; i++)
    {
        char errbuf[256];
        if (!worker_colls[i].in_use)
            continue;
        zvec_collection_flush(worker_colls[i].handle,
                              errbuf, sizeof(errbuf));
        zvec_collection_close(worker_colls[i].handle);
        worker_colls[i].in_use = false;
    }

    LWLockAcquire(pg_zvec_state->lock, LW_EXCLUSIVE);
    pg_zvec_state->worker_ready = false;
    pg_zvec_state->worker_pid   = 0;
    LWLockRelease(pg_zvec_state->lock);
}
