#ifndef TAR_XZ_H
#define TAR_XZ_H

#include <stdbool.h>
#include <stddef.h>

typedef void (*tar_xz_progress_cb)(size_t bytes_decompressed, const char *current_file, void *userdata);

bool extract_tar_xz(const char *xz_path, const char *dest_dir, tar_xz_progress_cb cb, void *userdata);

#endif
