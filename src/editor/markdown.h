#pragma once

#include <glib.h>

/* Turns the common Markdown marks of one line into Pango markup, escaping the text first so stray symbols stay
 * text. Headings come out bold */
char *convert_markdown_line(const char *line);
