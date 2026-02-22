#include "pg_zvec.h"
#include "pg_zvec_shmem.h"

#include "miscadmin.h"
#include "postmaster/bgworker.h"
#include "storage/ipc.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"
#include "utils/guc.h"

PG_MODULE_MAGIC;

/* Forward declaration to suppress -Wmissing-prototypes */
void _PG_init(void);

/* ----------------------------------------------------------------
 * GUC variables
 * ---------------------------------------------------------------- */
char *pg_zvec_data_dir      = NULL;
int   pg_zvec_query_threads    = 4;
int   pg_zvec_optimize_threads = 2;
int   pg_zvec_max_buffer_mb    = 64;

/* ----------------------------------------------------------------
 * Hook chain
 * ---------------------------------------------------------------- */
static shmem_startup_hook_type prev_shmem_startup_hook = NULL;

static void
pg_zvec_shmem_startup_hook(void)
{
    if (prev_shmem_startup_hook)
        prev_shmem_startup_hook();
    pg_zvec_shmem_startup();
}

/* ----------------------------------------------------------------
 * Background worker registration
 * ---------------------------------------------------------------- */
static void
register_background_worker(void)
{
    BackgroundWorker worker;

    MemSet(&worker, 0, sizeof(worker));

    /*
     * BGWORKER_SHMEM_ACCESS: the worker needs access to shared memory.
     * No database connection is required in Phase 1; the worker manages
     * zvec Collection file handles which are filesystem-based.
     */
    worker.bgw_flags      = BGWORKER_SHMEM_ACCESS;
    worker.bgw_start_time = BgWorkerStart_ConsistentState;
    worker.bgw_restart_time = 5;    /* seconds before auto-restart on crash */

    snprintf(worker.bgw_library_name,  BGW_MAXLEN, "pg_zvec");
    snprintf(worker.bgw_function_name, BGW_MAXLEN, "pg_zvec_worker_main");
    snprintf(worker.bgw_name,          BGW_MAXLEN, "pg_zvec worker");
    snprintf(worker.bgw_type,          BGW_MAXLEN, "pg_zvec");

    worker.bgw_notify_pid = 0;

    RegisterBackgroundWorker(&worker);
}

/* ----------------------------------------------------------------
 * _PG_init
 *
 * Called once when the shared library is loaded (via
 * shared_preload_libraries).  Registers GUCs, reserves shared memory,
 * and registers the background worker.
 * ---------------------------------------------------------------- */
void
_PG_init(void)
{
    if (!process_shared_preload_libraries_in_progress)
        ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("pg_zvec must be loaded via shared_preload_libraries")));

    /* --- GUCs --------------------------------------------------- */

    DefineCustomStringVariable(
        "pg_zvec.data_dir",
        "Directory where zvec collections are stored.",
        "Empty string defaults to $PGDATA/pg_zvec.",
        &pg_zvec_data_dir,
        "",
        PGC_POSTMASTER,
        0,
        NULL, NULL, NULL);

    DefineCustomIntVariable(
        "pg_zvec.query_threads",
        "Number of threads used for vector search queries.",
        NULL,
        &pg_zvec_query_threads,
        4,          /* boot default */
        1,          /* min          */
        64,         /* max          */
        PGC_SIGHUP,
        0,
        NULL, NULL, NULL);

    DefineCustomIntVariable(
        "pg_zvec.optimize_threads",
        "Number of threads used for index optimization.",
        NULL,
        &pg_zvec_optimize_threads,
        2,
        1,
        32,
        PGC_SIGHUP,
        0,
        NULL, NULL, NULL);

    DefineCustomIntVariable(
        "pg_zvec.max_buffer_mb",
        "Maximum write-buffer size per collection in MiB.",
        NULL,
        &pg_zvec_max_buffer_mb,
        64,
        8,
        8192,
        PGC_SIGHUP,
        0,
        NULL, NULL, NULL);

    /* --- Shared memory ------------------------------------------ */

    RequestAddinShmemSpace(pg_zvec_shmem_size());
    RequestNamedLWLockTranche("pg_zvec", 1);

    prev_shmem_startup_hook = shmem_startup_hook;
    shmem_startup_hook      = pg_zvec_shmem_startup_hook;

    /* --- Background worker -------------------------------------- */

    register_background_worker();
}
