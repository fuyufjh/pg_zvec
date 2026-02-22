#include "pg_zvec.h"
#include "pg_zvec_shmem.h"
#include "zvec_bridge/zvec_bridge.h"

#include "miscadmin.h"
#include "postmaster/bgworker.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/lwlock.h"
#include "storage/proc.h"
#include "storage/procarray.h"
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
 * process_request
 *
 * Called with pg_zvec_state->lock held EXCLUSIVELY.
 * Copies the request, releases the lock, does the work, then
 * re-acquires the lock to write the response.
 * ---------------------------------------------------------------- */
static void
process_request(void)
{
    ZvecRequest  req;
    ZvecResponse resp;
    PGPROC      *sender_proc;

    /* Copy request and release lock so backends can talk to us again */
    memcpy(&req, &pg_zvec_state->request, sizeof(ZvecRequest));
    pg_zvec_state->request_pending = false;
    LWLockRelease(pg_zvec_state->lock);

    memset(&resp, 0, sizeof(ZvecResponse));
    resp.success = true;

    switch (req.type)
    {
        /* --------------------------------------------------------
         * PING — smoke-test roundtrip
         * -------------------------------------------------------- */
        case ZVEC_REQ_PING:
            break;

        /* --------------------------------------------------------
         * CREATE_COLLECTION
         * Payload: [name\0][data_dir\0][index_type\0][metric\0]
         *          [dimension: int32][params_json\0]
         * -------------------------------------------------------- */
        case ZVEC_REQ_CREATE_COLLECTION:
        {
            char col_name[ZVEC_MAX_NAME_LEN];
            char data_dir[ZVEC_MAX_PATH_LEN];
            char index_type[32];
            char metric[32];
            int  dimension;
            char params_json[ZVEC_MAX_PARAMS_LEN];
            char errbuf[256];
            int  pos = 0;
            ZvecCollectionHandle *h;
            WorkerCollection     *wc;
            ZvecCollectionEntry  *entry;

            pos = zvec_unpack_str(req.data, pos, req.data_len,
                                  col_name, sizeof(col_name));
            if (pos < 0) goto unpack_error;
            pos = zvec_unpack_str(req.data, pos, req.data_len,
                                  data_dir, sizeof(data_dir));
            if (pos < 0) goto unpack_error;
            pos = zvec_unpack_str(req.data, pos, req.data_len,
                                  index_type, sizeof(index_type));
            if (pos < 0) goto unpack_error;
            pos = zvec_unpack_str(req.data, pos, req.data_len,
                                  metric, sizeof(metric));
            if (pos < 0) goto unpack_error;
            pos = zvec_unpack_int(req.data, pos, req.data_len, &dimension);
            if (pos < 0) goto unpack_error;
            pos = zvec_unpack_str(req.data, pos, req.data_len,
                                  params_json, sizeof(params_json));
            if (pos < 0) goto unpack_error;

            /* Check not already registered */
            LWLockAcquire(pg_zvec_state->lock, LW_SHARED);
            entry = shmem_find_entry(col_name, req.database_id);
            LWLockRelease(pg_zvec_state->lock);
            if (entry)
            {
                snprintf(resp.error_msg, sizeof(resp.error_msg),
                         "collection \"%s\" already exists", col_name);
                resp.success = false;
                break;
            }

            /* Create collection on disk */
            h = zvec_collection_create(data_dir, index_type, metric,
                                       dimension, params_json,
                                       errbuf, sizeof(errbuf));
            if (!h)
            {
                snprintf(resp.error_msg, sizeof(resp.error_msg), "%s", errbuf);
                resp.success = false;
                break;
            }

            /* Register in worker-local map */
            wc = worker_alloc_coll(col_name);
            if (!wc)
            {
                zvec_collection_close(h);
                snprintf(resp.error_msg, sizeof(resp.error_msg),
                         "too many open collections");
                resp.success = false;
                break;
            }
            wc->handle = h;

            /* Register in shared-memory registry */
            LWLockAcquire(pg_zvec_state->lock, LW_EXCLUSIVE);
            entry = shmem_alloc_entry();
            if (!entry)
            {
                LWLockRelease(pg_zvec_state->lock);
                zvec_collection_close(h);
                worker_free_coll(wc);
                snprintf(resp.error_msg, sizeof(resp.error_msg),
                         "shared-memory collection registry is full");
                resp.success = false;
                break;
            }
            entry->in_use      = true;
            entry->database_id = req.database_id;
            strlcpy(entry->name,       col_name,   sizeof(entry->name));
            strlcpy(entry->data_dir,   data_dir,   sizeof(entry->data_dir));
            strlcpy(entry->index_type, index_type, sizeof(entry->index_type));
            strlcpy(entry->metric,     metric,     sizeof(entry->metric));
            entry->dimension   = dimension;
            pg_zvec_state->num_collections++;
            LWLockRelease(pg_zvec_state->lock);

            ereport(LOG,
                    (errmsg("pg_zvec: created collection \"%s\" at \"%s\"",
                            col_name, data_dir)));
            break;

        unpack_error:
            snprintf(resp.error_msg, sizeof(resp.error_msg),
                     "malformed CREATE_COLLECTION payload");
            resp.success = false;
            break;
        }

        /* --------------------------------------------------------
         * DROP_COLLECTION
         * Payload: [name\0]
         * -------------------------------------------------------- */
        case ZVEC_REQ_DROP_COLLECTION:
        {
            char col_name[ZVEC_MAX_NAME_LEN];
            char errbuf[256];
            int  pos = 0;
            WorkerCollection    *wc;
            ZvecCollectionEntry *entry;

            pos = zvec_unpack_str(req.data, pos, req.data_len,
                                  col_name, sizeof(col_name));
            if (pos < 0)
            {
                snprintf(resp.error_msg, sizeof(resp.error_msg),
                         "malformed DROP_COLLECTION payload");
                resp.success = false;
                break;
            }

            wc = worker_find_coll(col_name);
            if (!wc)
            {
                snprintf(resp.error_msg, sizeof(resp.error_msg),
                         "collection \"%s\" not found", col_name);
                resp.success = false;
                break;
            }

            /* Flush, destroy on disk, close handle */
            zvec_collection_flush(wc->handle, errbuf, sizeof(errbuf));
            if (!zvec_collection_destroy(wc->handle, errbuf, sizeof(errbuf)))
            {
                snprintf(resp.error_msg, sizeof(resp.error_msg), "%s", errbuf);
                resp.success = false;
                /* Close handle even on destroy failure */
                zvec_collection_close(wc->handle);
                worker_free_coll(wc);
                /* Remove from shared-memory registry */
                goto drop_deregister;
            }
            zvec_collection_close(wc->handle);
            worker_free_coll(wc);

        drop_deregister:
            LWLockAcquire(pg_zvec_state->lock, LW_EXCLUSIVE);
            entry = shmem_find_entry(col_name, req.database_id);
            if (entry)
            {
                memset(entry, 0, sizeof(ZvecCollectionEntry));
                pg_zvec_state->num_collections--;
            }
            LWLockRelease(pg_zvec_state->lock);

            ereport(LOG,
                    (errmsg("pg_zvec: dropped collection \"%s\"", col_name)));
            break;
        }

        /* --------------------------------------------------------
         * INSERT (upsert)
         * Payload: [name\0][pk\0][vec_len: int32][float32 * vec_len]
         * -------------------------------------------------------- */
        case ZVEC_REQ_INSERT:
        {
            char  col_name[ZVEC_MAX_NAME_LEN];
            char  pk[256];
            int   vec_len;
            float *vec = NULL;
            char  errbuf[256];
            int   pos = 0;
            WorkerCollection *wc;

            pos = zvec_unpack_str(req.data, pos, req.data_len,
                                  col_name, sizeof(col_name));
            if (pos < 0) goto insert_unpack_error;
            pos = zvec_unpack_str(req.data, pos, req.data_len,
                                  pk, sizeof(pk));
            if (pos < 0) goto insert_unpack_error;
            pos = zvec_unpack_int(req.data, pos, req.data_len, &vec_len);
            if (pos < 0) goto insert_unpack_error;
            if (vec_len <= 0 || vec_len > 65536) goto insert_unpack_error;

            vec = (float *) palloc(vec_len * sizeof(float));
            pos = zvec_unpack_floats(req.data, pos, req.data_len,
                                     vec, vec_len);
            if (pos < 0) goto insert_unpack_error;

            wc = worker_find_coll(col_name);
            if (!wc)
            {
                snprintf(resp.error_msg, sizeof(resp.error_msg),
                         "collection \"%s\" not found", col_name);
                resp.success = false;
                break;
            }

            if (!zvec_collection_upsert(wc->handle, pk, vec, vec_len,
                                         errbuf, sizeof(errbuf)))
            {
                snprintf(resp.error_msg, sizeof(resp.error_msg), "%s", errbuf);
                resp.success = false;
            }
            break;

        insert_unpack_error:
            snprintf(resp.error_msg, sizeof(resp.error_msg),
                     "malformed INSERT payload");
            resp.success = false;
            break;
        }

        /* --------------------------------------------------------
         * DELETE
         * Payload: [name\0][pk\0]
         * -------------------------------------------------------- */
        case ZVEC_REQ_DELETE:
        {
            char col_name[ZVEC_MAX_NAME_LEN];
            char pk[256];
            char errbuf[256];
            int  pos = 0;
            WorkerCollection *wc;

            pos = zvec_unpack_str(req.data, pos, req.data_len,
                                  col_name, sizeof(col_name));
            if (pos < 0) goto delete_unpack_error;
            pos = zvec_unpack_str(req.data, pos, req.data_len,
                                  pk, sizeof(pk));
            if (pos < 0) goto delete_unpack_error;

            wc = worker_find_coll(col_name);
            if (!wc)
            {
                snprintf(resp.error_msg, sizeof(resp.error_msg),
                         "collection \"%s\" not found", col_name);
                resp.success = false;
                break;
            }

            if (!zvec_collection_delete(wc->handle, pk, errbuf, sizeof(errbuf)))
            {
                snprintf(resp.error_msg, sizeof(resp.error_msg), "%s", errbuf);
                resp.success = false;
            }
            break;

        delete_unpack_error:
            snprintf(resp.error_msg, sizeof(resp.error_msg),
                     "malformed DELETE payload");
            resp.success = false;
            break;
        }

        /* --------------------------------------------------------
         * OPTIMIZE
         * Payload: [name\0]
         * -------------------------------------------------------- */
        case ZVEC_REQ_OPTIMIZE:
        {
            char col_name[ZVEC_MAX_NAME_LEN];
            char errbuf[256];
            int  pos = 0;
            WorkerCollection *wc;

            pos = zvec_unpack_str(req.data, pos, req.data_len,
                                  col_name, sizeof(col_name));
            if (pos < 0)
            {
                snprintf(resp.error_msg, sizeof(resp.error_msg),
                         "malformed OPTIMIZE payload");
                resp.success = false;
                break;
            }

            wc = worker_find_coll(col_name);
            if (!wc)
            {
                snprintf(resp.error_msg, sizeof(resp.error_msg),
                         "collection \"%s\" not found", col_name);
                resp.success = false;
                break;
            }

            set_ps_display("optimizing collection");
            if (!zvec_collection_optimize(wc->handle, errbuf, sizeof(errbuf)))
            {
                snprintf(resp.error_msg, sizeof(resp.error_msg), "%s", errbuf);
                resp.success = false;
            }
            break;
        }

        default:
            snprintf(resp.error_msg, sizeof(resp.error_msg),
                     "unknown request type %d", (int) req.type);
            resp.success = false;
            break;
    }

    /* Write response and wake the requesting backend */
    LWLockAcquire(pg_zvec_state->lock, LW_EXCLUSIVE);
    memcpy(&pg_zvec_state->response, &resp, sizeof(ZvecResponse));
    pg_zvec_state->response_ready = true;
    LWLockRelease(pg_zvec_state->lock);

    sender_proc = BackendPidGetProc(req.sender_pid);
    if (sender_proc != NULL)
        SetLatch(&sender_proc->procLatch);
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

    /* Announce ourselves in shared memory */
    LWLockAcquire(pg_zvec_state->lock, LW_EXCLUSIVE);
    pg_zvec_state->worker_pid   = MyProcPid;
    pg_zvec_state->worker_ready = true;
    LWLockRelease(pg_zvec_state->lock);

    set_ps_display("idle");

    /* ----------------------------------------------------------------
     * Main loop
     * ---------------------------------------------------------------- */
    while (!worker_shutdown_requested)
    {
        int rc;

        rc = WaitLatch(MyLatch,
                       WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
                       1000L,
                       PG_WAIT_EXTENSION);
        ResetLatch(MyLatch);

        if (rc & WL_EXIT_ON_PM_DEATH)
            break;

        if (worker_shutdown_requested)
            break;

        /* Check for a pending request */
        LWLockAcquire(pg_zvec_state->lock, LW_EXCLUSIVE);
        if (pg_zvec_state->request_pending)
        {
            set_ps_display("processing request");
            /* process_request() releases and re-acquires the lock internally */
            process_request();
            set_ps_display("idle");
        }
        else
        {
            LWLockRelease(pg_zvec_state->lock);
        }
    }

    /* ----------------------------------------------------------------
     * Shutdown: flush and close all open collections
     * ---------------------------------------------------------------- */
    ereport(LOG, (errmsg("pg_zvec worker shutting down")));

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
