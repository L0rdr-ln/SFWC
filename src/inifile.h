/* Line-numbered readers on top of inih, shared by the config and theme parsers. */
#ifndef SFWC_INIFILE_H
#define SFWC_INIFILE_H

#include <stdio.h>

#include "ini.h"

enum { INI_WARNING = 0, INI_ERROR = 1 };
typedef void (*ini_log_fn)(int level, int line, const char *msg, void *data);

/* inih calls the reader once per line, so counting calls gives the current line number
 * while the handler runs (INI_ALLOW_MULTILINE is off). */
struct inifile_reader {
    FILE *fp;
    const char *text; /* string mode */
    size_t pos;
    int line;
};

char *inifile_file_reader(char *str, int num, void *stream);
char *inifile_string_reader(char *str, int num, void *stream);

#endif
