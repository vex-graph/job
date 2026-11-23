#ifndef NIO_JOB_MEMORY_H
#define NIO_JOB_MEMORY_H

#include "vex.h"
#include "nio/mem.h"

MemoryArena *Job_arena();
fn Job_arenaShutdown();

#endif
