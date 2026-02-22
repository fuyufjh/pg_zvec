#include "pg_zvec_shmem.h"

#include "storage/shmem.h"
#include "utils/memutils.h"

/* The single shared-memory pointer, set by pg_zvec_shmem_startup(). */
PgZvecSharedState *pg_zvec_state = NULL;

Size
pg_zvec_shmem_size(void)
{
    return sizeof(PgZvecSharedState);
}

/*
 * pg_zvec_shmem_startup
 *
 * Called from the postmaster's shmem_startup_hook (after shared memory is
 * available).  Allocates and initialises our shared state struct.
 */
void
pg_zvec_shmem_startup(void)
{
    bool found;

    pg_zvec_state = ShmemInitStruct("pg_zvec",
                                    sizeof(PgZvecSharedState),
                                    &found);
    if (!found)
    {
        /*
         * First call: zero-fill and wire up the LWLock that was reserved via
         * RequestNamedLWLockTranche("pg_zvec", 1) in _PG_init.
         */
        MemSet(pg_zvec_state, 0, sizeof(PgZvecSharedState));
        pg_zvec_state->lock = &GetNamedLWLockTranche("pg_zvec")[0].lock;
    }
}
