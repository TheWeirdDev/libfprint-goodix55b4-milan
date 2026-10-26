/*
 * Goodix 55b4 — background file writer
 *
 * Writes files off the main loop: requests are queued per key and written
 * by one worker thread. A request not yet started is replaced by a newer
 * one for the same key (only the latest content matters). Used for the
 * engine state and the studied templates, both up to ~0.5 MB with fsync.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#pragma once

#include <glib.h>

/* Writes `item` for `key`, on the worker thread. Returns FALSE with error
 * set on failure; *written is FALSE when it chose not to write (e.g. the
 * content is unchanged). */
typedef gboolean (*Goodix55b4WriteFunc) (const char *key,
                                         gpointer    item,
                                         gpointer    user_data,
                                         gboolean   *written,
                                         GError    **error);

typedef struct _Goodix55b4Writer Goodix55b4Writer;

/* `what` names the files in log messages; user_data is freed with
 * user_data_free (may be NULL) after the last write. */
Goodix55b4Writer *goodix55b4_writer_new (const char         *what,
                                         Goodix55b4WriteFunc write,
                                         gpointer            user_data,
                                         GDestroyNotify      user_data_free);

/* Waits for the pending writes, then frees. */
void goodix55b4_writer_free (Goodix55b4Writer *writer);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (Goodix55b4Writer, goodix55b4_writer_free)

/* Takes ownership of item (freed with item_free once written or replaced). */
void goodix55b4_writer_queue (Goodix55b4Writer *writer,
                              const char       *key,
                              gpointer          item,
                              GDestroyNotify    item_free);

/* Blocks until every queued item is written (or failed). */
void goodix55b4_writer_flush (Goodix55b4Writer *writer);

/* Files actually written so far. */
guint goodix55b4_writer_writes (Goodix55b4Writer *writer);
