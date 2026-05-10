#ifndef DEEPAGENT_TOOLS_FS_H
#define DEEPAGENT_TOOLS_FS_H

#include "tools.h"

void tools_fs_register(void);
int fs_mkdir_p(const char *path);
int fs_copy_file_path(const char *src, const char *dst, char **err);

#endif
