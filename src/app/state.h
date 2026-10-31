#pragma once

#include "common/window-geometry.h"

/* Window layout and the last project, kept between launches */
typedef struct {
	WindowGeometry geometry;
	gboolean shows_sidebar;
	/* Whether the sidebar shows the plan instead of the project files */
	gboolean shows_plan;
	int sidebar_width, terminal_height;
	char *project;
	/* Paths of the files open when the window last closed, and the one in front */
	char **files;
	char *current_file;
} WindowState;

/* Rows open in a project's file tree and plan, kept for each project between launches */
typedef struct {
	/* Folder paths inside the project */
	char **open_folders;
	/* Keys of the plan's rows, which name each row by its file and text */
	char **open_plan_rows;
} ProjectState;

void load_window_state(WindowState *state);
void save_window_state(const WindowState *state);
void clear_window_state(WindowState *state);
void load_project_state(const char *project, ProjectState *state);
void save_project_state(const char *project, const ProjectState *state);
void clear_project_state(ProjectState *state);

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC(ProjectState, clear_project_state)
