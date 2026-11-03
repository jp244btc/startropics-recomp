/*
 * rewind.h - history of machine states for the rewind feature.
 *
 * Each record is the XOR difference to the previous state, run-length coded
 * (most bytes of a NES state do not change between frames), kept in a ring of
 * fixed size; when it is full the oldest records go. Stepping back restores the
 * previous state exactly, record by record. No SDL, no emulator dependencies.
 */
#ifndef REWIND_H
#define REWIND_H
#include <stddef.h>

int    rewind_init(size_t state_size, size_t budget_bytes);   /* 1 on success */
void   rewind_shutdown(void);
void   rewind_clear(void);                  /* forget the history (new game, a state loaded, ROM changed) */
void   rewind_push(const void *state);      /* record the current state (every few frames) */
int    rewind_pop(void *state_out);         /* step back one record: 1 and the state, 0 when the history is empty */
int    rewind_count(void);                  /* records available to step back through */
size_t rewind_bytes_used(void);             /* ring bytes in use (statistics) */

#endif
