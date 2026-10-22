#pragma once

#include <gio/gio.h>

/* Reads the project's plan files and finds tagged comments in its other files, away from the main thread. With
 * only_path set, only that file is read. The result is a GPtrArray of PlanItem files */
void start_plan_scan(const char *root, const char *only_path, GCancellable *cancellable,
	GAsyncReadyCallback on_done, gpointer data);
GPtrArray *finish_plan_scan(GAsyncResult *result, GError **error);
