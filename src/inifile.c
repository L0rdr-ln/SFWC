#include "inifile.h"

char *inifile_file_reader(char *str, int num, void *stream)
{
    struct inifile_reader *r = stream;
    char *res = fgets(str, num, r->fp);
    if (res) {
        r->line++;
    }
    return res;
}

char *inifile_string_reader(char *str, int num, void *stream)
{
    struct inifile_reader *r = stream;
    if (r->text[r->pos] == '\0') {
        return NULL;
    }
    int i = 0;
    while (i < num - 1 && r->text[r->pos] != '\0') {
        str[i++] = r->text[r->pos++];
        if (str[i - 1] == '\n') {
            break;
        }
    }
    str[i] = '\0';
    r->line++;
    return str;
}
