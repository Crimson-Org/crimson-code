#pragma once

#include <glib.h>

typedef struct Window Window;

/* Where a command's shortcut works, so the terminal and Vim keep the keys they need */
typedef enum {
	/* Works wherever the focus is */
	COMMAND_EVERYWHERE,
	/* Left to the shell while the terminal has focus */
	COMMAND_OUTSIDE_TERMINAL,
	/* Also left to Vim while a Vim mode editor has focus, unless the person gives those keys to the editor */
	COMMAND_EASY_EDITING,
} CommandScope;

/* What a command was started with, from a shortcut, the menu or a : command such as ":w! name" */
typedef struct {
	const char *argument;
	gboolean is_forced;
	/* A message the command keeps for the status bar, owned by the caller */
	char *message;
} CommandInput;

typedef void (*CommandFunction)(Window *window, CommandInput *input);
typedef gboolean (*CommandAvailableFunction)(Window *window);
typedef gboolean (*CommandCheckedFunction)(Window *window);

/* A command any component lists, from which the menu, shortcuts, : commands and shortcut list are made */
typedef struct {
	const char *id, *label;
	/* Menu section id, or NULL to keep the command out of the main menu */
	const char *menu_section;
	const char *shortcuts[3];
	CommandScope scope;
	/* Names a : command can use, such as "w" and "write" */
	const char *vim_names[4];
	/* What it does, or NULL when it only toggles the switch setting below */
	CommandFunction run;
	/* A switch setting the command turns on and off, shown with a check in the menu */
	const char *setting;
	/* Whether the command can run now, or NULL for always, since unusable commands leave the menu and buttons */
	CommandAvailableFunction is_available;
	/* Whether something the command shows and hides is showing, for a check in the menu, or NULL */
	CommandCheckedFunction is_checked;
} Command;

/* Menu sections in the order they appear, which later components may add to. A section with a label becomes a
 * submenu, and neighbouring submenus share one group, so the main menu stays short */
typedef struct {
	const char *id, *label;
} MenuSection;

void register_commands(const Command *commands, guint count);
void register_menu_sections(const MenuSection *sections, guint count);
GPtrArray *list_commands(void);
GPtrArray *list_menu_sections(void);
const Command *find_command(const char *id);
/* Finds a command by its : name, ignoring a trailing ! */
const Command *find_vim_command(const char *name);
