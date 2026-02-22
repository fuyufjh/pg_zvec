#ifndef PG_ZVEC_H
#define PG_ZVEC_H

#include "postgres.h"
#include "fmgr.h"

/* ----------------------------------------------------------------
 * GUC parameters (defined in pg_zvec.c)
 * ---------------------------------------------------------------- */
extern char *pg_zvec_data_dir;
extern int   pg_zvec_query_threads;
extern int   pg_zvec_optimize_threads;
extern int   pg_zvec_max_buffer_mb;

/* ----------------------------------------------------------------
 * SQL-callable functions
 * ---------------------------------------------------------------- */
PGDLLEXPORT Datum zvec_create_collection(PG_FUNCTION_ARGS);
PGDLLEXPORT Datum zvec_drop_collection(PG_FUNCTION_ARGS);
PGDLLEXPORT Datum zvec_optimize(PG_FUNCTION_ARGS);
PGDLLEXPORT Datum zvec_stats(PG_FUNCTION_ARGS);
PGDLLEXPORT Datum zvec_search(PG_FUNCTION_ARGS);
PGDLLEXPORT Datum zvec_search_filtered(PG_FUNCTION_ARGS);
PGDLLEXPORT Datum zvec_attach_table(PG_FUNCTION_ARGS);
PGDLLEXPORT Datum zvec_sync_trigger(PG_FUNCTION_ARGS);
PGDLLEXPORT Datum zvec_worker_status(PG_FUNCTION_ARGS);

/* ----------------------------------------------------------------
 * Background worker entry point
 * ---------------------------------------------------------------- */
PGDLLEXPORT void pg_zvec_worker_main(Datum main_arg);

#endif /* PG_ZVEC_H */
