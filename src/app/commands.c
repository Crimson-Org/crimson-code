#include <string.h>

#include "commands.h"

static GPtrArray *commands, *sections;


static void prepare(void) {
	if (commands)
		return;
	commands = g_ptr_array_new();
	sections = g_ptr_array_new();
}


void register_commands(const Command *new_commands, guint count) {
	prepare();
	for (guint index = 0; index < count; index++) {
		if (find_command(new_commands[index].id))
			g_critical("Command %s is registered twice", new_commands[index].id);
		else
			g_ptr_array_add(commands, (gpointer)&new_commands[index]);
	}
}


void register_menu_sections(const MenuSection *new_sections, guint count) {
	prepare();
	for (guint index = 0; index < count; index++)
		g_ptr_array_add(sections, (gpointer)&new_sections[index]);
}


GPtrArray *list_commands(void) {
	prepare();
	return commands;
}


GPtrArray *list_menu_sections(void) {
	prepare();
	return sections;
}


const Command *find_command(const char *id) {
	prepare();
	for (guint index = 0; index < commands->len; index++)
		if (strcmp(((Command *)g_ptr_array_index(commands, index))->id, id) == 0)
			return g_ptr_array_index(commands, index);
	return NULL;
}


const Command *find_vim_command(const char *name) {
	prepare();
	gsize length = strlen(name) - g_str_has_suffix(name, "!");
	for (guint index = 0; index < commands->len; index++) {
		const Command *command = g_ptr_array_index(commands, index);
		for (int alias = 0; alias < 4 && command->vim_names[alias]; alias++)
			if (strlen(command->vim_names[alias]) == length && strncmp(command->vim_names[alias], name, length) == 0)
				return command;
	}
	return NULL;
}
