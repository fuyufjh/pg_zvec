/*
 * pg_zvec_fdw.h
 *
 * Internal declarations shared between the FDW source files.
 */
#ifndef PG_ZVEC_FDW_H
#define PG_ZVEC_FDW_H

#include "postgres.h"
#include "fmgr.h"

/* ----------------------------------------------------------------
 * GUC parameters (defined in pg_zvec_fdw.c)
 * ---------------------------------------------------------------- */
extern char *pg_zvec_data_dir;
extern int   pg_zvec_query_threads;
extern int   pg_zvec_optimize_threads;
extern int   pg_zvec_max_buffer_mb;

/* ----------------------------------------------------------------
 * FDW handler entry points (registered in pg_zvec--2.0.sql)
 * ---------------------------------------------------------------- */
PGDLLEXPORT Datum zvec_fdw_handler(PG_FUNCTION_ARGS);
PGDLLEXPORT Datum zvec_fdw_validator(PG_FUNCTION_ARGS);

/* ----------------------------------------------------------------
 * Background worker entry point
 * ---------------------------------------------------------------- */
PGDLLEXPORT void pg_zvec_worker_main(Datum main_arg);

/* ----------------------------------------------------------------
 * Per-scan execution state
 * ---------------------------------------------------------------- */
typedef struct ZvecFdwScanState
{
    /* filled in BeginForeignScan, used by IterateForeignScan */
    bool    done;
} ZvecFdwScanState;

#endif /* PG_ZVEC_FDW_H */
