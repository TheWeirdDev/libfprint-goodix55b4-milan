/*
 * Goodix 55b4 — Milan engine state: generation handling and persistence
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#pragma once

#include <glib.h>

#include "milan/preprocess/state.h"
#include "milan/runtime.h"

typedef struct
{
  GoodixMilanPreprocessState state;
  GoodixMilanProfileState    profile;
} Goodix55b4EngineState;

/* Fresh engine state (no history). */
void goodix55b4_state_init (Goodix55b4EngineState *s);

/* A new no-finger (setup) frame starts a new generation. Mirrors upstream
 * goodix53x5 goodix_milan_generation_transfer_process_state(): reset the
 * preprocessing state, keep the long-lived process fields (gain maps, coarse
 * reference, classifier history) and ask the engine to re-admit the setup. */
void goodix55b4_state_new_generation (Goodix55b4EngineState *s);

/* Carries the state forward from the engine output of the current action,
 * exactly as upstream device/enroll.c: the whole state when the run
 * produced one, else only the setup flags. */
void goodix55b4_state_apply_output (Goodix55b4EngineState          *s,
                                    const GoodixMilanRuntimeOutput *out);

/* Versioned on-disk format; a file from another engine build, size or
 * subtype is rejected (load returns FALSE and leaves *s untouched). */
gboolean goodix55b4_state_load (Goodix55b4EngineState *s,
                                const char            *path,
                                guint16                subtype,
                                GError               **error);
gboolean goodix55b4_state_save (const Goodix55b4EngineState *s,
                                const char                  *path,
                                guint16                      subtype,
                                GError                     **error);

/* Writes the state file off the main thread. Was a synchronous ~212 KB
 * write with fsync on the main loop after every engine run.
 *
 * Requests are copied and written in order by a worker thread; one not yet
 * started is replaced by a newer one (only the latest state matters), and
 * a state identical to the last one written or loaded is not written.
 * Write errors are logged; the next request retries. */
typedef struct _Goodix55b4StateSaver Goodix55b4StateSaver;

Goodix55b4StateSaver *goodix55b4_state_saver_new (const char *path,
                                                  guint16     subtype);

/* Waits for the pending write, then frees. */
void goodix55b4_state_saver_free (Goodix55b4StateSaver *saver);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (Goodix55b4StateSaver, goodix55b4_state_saver_free)

/* The file already holds s (e.g. it was just loaded from it). */
void goodix55b4_state_saver_mark_saved (Goodix55b4StateSaver        *saver,
                                        const Goodix55b4EngineState *s);

void goodix55b4_state_saver_queue (Goodix55b4StateSaver        *saver,
                                   const Goodix55b4EngineState *s);

/* Blocks until every queued state is written (or failed). */
void goodix55b4_state_saver_flush (Goodix55b4StateSaver *saver);

/* Files actually written so far (for tests and logs). */
guint goodix55b4_state_saver_writes (Goodix55b4StateSaver *saver);
