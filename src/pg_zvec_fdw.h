/*
 * pg_zvec_fdw.h
 *
 * Internal declarations shared between the FDW source files.
 */
#ifndef PG_ZVEC_FDW_H
#define PG_ZVEC_FDW_H

#include "postgres.h"
#include "access/attnum.h"
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

/* ----------------------------------------------------------------
 * Per-modify execution state (INSERT / DELETE)
 *
 * Convention: attno 1 (the first column) is the primary key (text).
 * The vector column is the first float4[] column found in the tuple.
 * ---------------------------------------------------------------- */
typedef struct ZvecFdwModifyState
{
    char        collection_name[128];   /* zvec collection name (= relation name) */
    int         pk_attno;               /* 1-based attno of pk column */
    int         vec_attno;              /* 1-based attno of float4[] column */
    int         dimension;              /* expected vector length */
    AttrNumber  pk_junk_attno;          /* junk attno in planSlot (DELETE only) */
} ZvecFdwModifyState;

#endif /* PG_ZVEC_FDW_H */
