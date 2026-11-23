#include "vex.h"
#include "nio/job_memory.h"

;;DEFINITION
/**
 * ============================================================================
 * DEFINITION: JobMemory (job/nio/job_memory.c)
 * ============================================================================
 * Per-library MemoryArena owner for Job, backed by Relational Engine's
 * MemoryPool. Starts at zero OS pages and grows on demand until OOM.
 * ============================================================================
 */

;;OVERVIEW
/**
 * ============================================================================
 * MODULE: JobMemory (nio/job_memory)
 * ============================================================================
 * Lazily creates and shuts down Job's dedicated MemoryArena.
 * ============================================================================
 */

static MemoryArena *s_jobArena = nullptr;

/** Lazily create and return Job's page-pooled MemoryArena. */
MemoryArena *Job_arena() {
    if (s_jobArena == nullptr)
        s_jobArena = MemoryArena();
    return s_jobArena;
}

/** Destroy Job's MemoryArena and return all pages to MemoryPool. */
fn Job_arenaShutdown() {
    if (s_jobArena != nullptr) {
        MemoryArena_destroy(s_jobArena);
        s_jobArena = nullptr;
    }
}
