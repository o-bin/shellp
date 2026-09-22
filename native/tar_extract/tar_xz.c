#include "tar_xz.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <fcntl.h>

#include "xz.h"

#define IN_BUF_SIZE 65536
#define OUT_BUF_SIZE 65536
#define TAR_BLOCK_SIZE 512

struct tar_raw_header {
    char name[100];
    char mode[8];
    char uid[8];
    char gid[8];
    char size[12];
    char mtime[12];
    char chksum[8];
    char typeflag;
    char linkname[100];
    char magic[6];
    char version[2];
    char uname[32];
    char gname[32];
    char devmajor[8];
    char devminor[8];
    char prefix[155];
    char pad[12];
};

static void mkdir_p(const char *path, mode_t mode) {
    char tmp[1024];
    strncpy(tmp, path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = 0;
    size_t len = strlen(tmp);
    if (len > 0 && tmp[len - 1] == '/') tmp[len - 1] = 0;

    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            mkdir(tmp, mode);
            *p = '/';
        }
    }
    mkdir(tmp, mode);
}

static void mkdir_p_for_file(const char *file_path) {
    char tmp[1024];
    strncpy(tmp, file_path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = 0;
    char *last_slash = strrchr(tmp, '/');
    if (last_slash && last_slash != tmp) {
        *last_slash = 0;
        mkdir_p(tmp, 0755);
    }
}

typedef enum {
    STATE_HEADER,
    STATE_FILE_DATA,
    STATE_GNU_LONG_NAME,
    STATE_GNU_LONG_LINK,
    STATE_SKIP_DATA
} TarState;

bool extract_tar_xz(const char *xz_path, const char *dest_dir, tar_xz_progress_cb cb, void *userdata) {
    FILE *in_fp = fopen(xz_path, "rb");
    if (!in_fp) return false;

    xz_crc32_init();
#ifdef XZ_USE_CRC64
    xz_crc64_init();
#endif

    struct xz_dec *dec = xz_dec_init(XZ_DYNALLOC, 64U << 20);
    if (!dec) {
        fclose(in_fp);
        return false;
    }

    uint8_t *in_buf = (uint8_t *)malloc(IN_BUF_SIZE);
    uint8_t *out_buf = (uint8_t *)malloc(OUT_BUF_SIZE);
    if (!in_buf || !out_buf) {
        if (in_buf) free(in_buf);
        if (out_buf) free(out_buf);
        xz_dec_end(dec);
        fclose(in_fp);
        return false;
    }

    struct xz_buf b;
    b.in = in_buf;
    b.in_pos = 0;
    b.in_size = 0;
    b.out = out_buf;
    b.out_pos = 0;
    b.out_size = OUT_BUF_SIZE;

    TarState state = STATE_HEADER;
    uint8_t block[TAR_BLOCK_SIZE];
    size_t block_pos = 0;

    char gnu_long_name[1024] = {0};
    char gnu_long_link[1024] = {0};
    size_t special_data_rem = 0;
    char *special_data_buf = NULL;

    FILE *cur_out_file = NULL;
    size_t cur_file_rem = 0;
    size_t cur_pad_rem = 0;
    mode_t cur_file_mode = 0755;
    char cur_file_full_dest[1024] = {0};

    size_t total_decompressed = 0;
    size_t files_extracted = 0;
    int zero_blocks = 0;
    bool success = true;

    while (true) {
        if (b.in_pos == b.in_size) {
            b.in_size = fread(in_buf, 1, IN_BUF_SIZE, in_fp);
            b.in_pos = 0;
        }

        bool finish = (b.in_size == 0);
        enum xz_ret ret = xz_dec_catrun(dec, &b, finish);

        size_t produced = b.out_pos;
        if (produced > 0) {
            total_decompressed += produced;
            size_t out_idx = 0;

            while (out_idx < produced) {
                if (state == STATE_HEADER) {
                    size_t needed = TAR_BLOCK_SIZE - block_pos;
                    size_t avail = produced - out_idx;
                    size_t take = avail < needed ? avail : needed;
                    memcpy(block + block_pos, out_buf + out_idx, take);
                    block_pos += take;
                    out_idx += take;

                    if (block_pos == TAR_BLOCK_SIZE) {
                        block_pos = 0;
                        // Check if block is all zeros
                        bool is_zero = true;
                        for (int i = 0; i < TAR_BLOCK_SIZE; i++) {
                            if (block[i] != 0) { is_zero = false; break; }
                        }
                        if (is_zero) {
                            zero_blocks++;
                            if (zero_blocks >= 2) {
                                // End of tar archive
                                state = STATE_SKIP_DATA;
                                cur_file_rem = 0;
                            }
                            continue;
                        }
                        zero_blocks = 0;

                        struct tar_raw_header *hdr = (struct tar_raw_header *)block;
                        size_t file_size = 0;
                        for (int i = 0; i < 11; i++) {
                            char c = hdr->size[i];
                            if (c >= '0' && c <= '7') {
                                file_size = (file_size << 3) + (c - '0');
                            }
                        }

                        char type = hdr->typeflag;
                        if (type == 'L') {
                            state = STATE_GNU_LONG_NAME;
                            special_data_rem = file_size;
                            cur_pad_rem = (TAR_BLOCK_SIZE - (file_size % TAR_BLOCK_SIZE)) % TAR_BLOCK_SIZE;
                            gnu_long_name[0] = 0;
                            special_data_buf = gnu_long_name;
                        } else if (type == 'K') {
                            state = STATE_GNU_LONG_LINK;
                            special_data_rem = file_size;
                            cur_pad_rem = (TAR_BLOCK_SIZE - (file_size % TAR_BLOCK_SIZE)) % TAR_BLOCK_SIZE;
                            gnu_long_link[0] = 0;
                            special_data_buf = gnu_long_link;
                        } else if (type == 'x' || type == 'g') {
                            // PAX headers - skip payload
                            state = STATE_SKIP_DATA;
                            cur_file_rem = file_size;
                            cur_pad_rem = (TAR_BLOCK_SIZE - (file_size % TAR_BLOCK_SIZE)) % TAR_BLOCK_SIZE;
                        } else {
                            // Normal entry
                            char path_buf[1024];
                            if (gnu_long_name[0] != 0) {
                                strncpy(path_buf, gnu_long_name, sizeof(path_buf) - 1);
                                path_buf[sizeof(path_buf) - 1] = 0;
                                gnu_long_name[0] = 0;
                            } else if (hdr->prefix[0] != 0) {
                                snprintf(path_buf, sizeof(path_buf), "%.155s/%.100s", hdr->prefix, hdr->name);
                            } else {
                                snprintf(path_buf, sizeof(path_buf), "%.100s", hdr->name);
                            }

                            char link_buf[1024];
                            if (gnu_long_link[0] != 0) {
                                strncpy(link_buf, gnu_long_link, sizeof(link_buf) - 1);
                                link_buf[sizeof(link_buf) - 1] = 0;
                                gnu_long_link[0] = 0;
                            } else {
                                snprintf(link_buf, sizeof(link_buf), "%.100s", hdr->linkname);
                            }

                            mode_t mode = 0;
                            for (int i = 0; i < 8; i++) {
                                char c = hdr->mode[i];
                                if (c >= '0' && c <= '7') {
                                    mode = (mode << 3) + (c - '0');
                                }
                            }
                            if (mode == 0) {
                                mode = 0755;
                            }

                            // Build full destination path
                            char full_dest[1024];
                            snprintf(full_dest, sizeof(full_dest), "%s/%s", dest_dir, path_buf);

                            // Strip any trailing slash
                            size_t flen = strlen(full_dest);
                            if (flen > 0 && full_dest[flen - 1] == '/') full_dest[flen - 1] = 0;

                            if (type == '5') {
                                // Directory
                                mkdir_p(full_dest, 0755);
                                chmod(full_dest, 0755);
                                files_extracted++;
                                if (cb) cb(total_decompressed, path_buf, userdata);
                            } else if (type == '2') {
                                // Symlink
                                mkdir_p_for_file(full_dest);
                                unlink(full_dest);
                                symlink(link_buf, full_dest);
                                files_extracted++;
                                if (cb) cb(total_decompressed, path_buf, userdata);
                            } else if (type == '1') {
                                // Hardlink
                                char target_link[1024];
                                snprintf(target_link, sizeof(target_link), "%s/%s", dest_dir, link_buf);
                                mkdir_p_for_file(full_dest);
                                unlink(full_dest);
                                (void)link(target_link, full_dest);
                                files_extracted++;
                                if (cb) cb(total_decompressed, path_buf, userdata);
                            } else if (type == '3' || type == '4' || type == '6') {
                                // Device nodes or FIFO - skip cleanly
                                state = STATE_SKIP_DATA;
                                cur_file_rem = file_size;
                                cur_pad_rem = (TAR_BLOCK_SIZE - (file_size % TAR_BLOCK_SIZE)) % TAR_BLOCK_SIZE;
                            } else {
                                // Regular file ("0" or 0)
                                mkdir_p_for_file(full_dest);
                                unlink(full_dest);
                                cur_out_file = fopen(full_dest, "wb");
                                cur_file_rem = file_size;
                                cur_pad_rem = (TAR_BLOCK_SIZE - (file_size % TAR_BLOCK_SIZE)) % TAR_BLOCK_SIZE;

                                mode_t fmode = mode & 0777;
                                fmode |= 0600; // Always owner read/write
                                if ((mode & 0111) != 0 ||
                                    strstr(path_buf, "bin/") ||
                                    strstr(path_buf, "sbin/") ||
                                    strstr(path_buf, "lib/") ||
                                    strstr(path_buf, ".so")) {
                                    fmode |= 0755;
                                }
                                cur_file_mode = fmode;
                                strncpy(cur_file_full_dest, full_dest, sizeof(cur_file_full_dest) - 1);
                                cur_file_full_dest[sizeof(cur_file_full_dest) - 1] = 0;

                                if (cur_file_rem == 0) {
                                    if (cur_out_file) {
                                        fclose(cur_out_file);
                                        cur_out_file = NULL;
                                        chmod(cur_file_full_dest, cur_file_mode);
                                    }
                                    state = (cur_pad_rem > 0) ? STATE_SKIP_DATA : STATE_HEADER;
                                } else {
                                    state = STATE_FILE_DATA;
                                }
                                files_extracted++;
                                if (cb) cb(total_decompressed, path_buf, userdata);
                            }
                        }
                    }
                } else if (state == STATE_FILE_DATA) {
                    size_t avail = produced - out_idx;
                    size_t write_bytes = avail < cur_file_rem ? avail : cur_file_rem;
                    if (cur_out_file && write_bytes > 0) {
                        fwrite(out_buf + out_idx, 1, write_bytes, cur_out_file);
                    }
                    cur_file_rem -= write_bytes;
                    out_idx += write_bytes;

                    if (cur_file_rem == 0) {
                        if (cur_out_file) {
                            fclose(cur_out_file);
                            cur_out_file = NULL;
                            chmod(cur_file_full_dest, cur_file_mode);
                        }
                        if (cur_pad_rem > 0) {
                            state = STATE_SKIP_DATA;
                        } else {
                            state = STATE_HEADER;
                        }
                    }
                } else if (state == STATE_GNU_LONG_NAME || state == STATE_GNU_LONG_LINK) {
                    size_t avail = produced - out_idx;
                    size_t take = avail < special_data_rem ? avail : special_data_rem;
                    size_t cur_len = strlen(special_data_buf);
                    if (cur_len + take < 1023) {
                        memcpy(special_data_buf + cur_len, out_buf + out_idx, take);
                        special_data_buf[cur_len + take] = 0;
                    }
                    special_data_rem -= take;
                    out_idx += take;

                    if (special_data_rem == 0) {
                        if (cur_pad_rem > 0) {
                            state = STATE_SKIP_DATA;
                        } else {
                            state = STATE_HEADER;
                        }
                    }
                } else if (state == STATE_SKIP_DATA) {
                    size_t total_skip = cur_file_rem + cur_pad_rem;
                    if (total_skip == 0) {
                        state = STATE_HEADER;
                        continue;
                    }
                    size_t avail = produced - out_idx;
                    size_t take = avail < total_skip ? avail : total_skip;
                    if (take <= cur_file_rem) {
                        cur_file_rem -= take;
                    } else {
                        size_t rem_take = take - cur_file_rem;
                        cur_file_rem = 0;
                        cur_pad_rem = (cur_pad_rem > rem_take) ? cur_pad_rem - rem_take : 0;
                    }
                    out_idx += take;

                    if (cur_file_rem == 0 && cur_pad_rem == 0) {
                        state = STATE_HEADER;
                    }
                }
            }
            b.out_pos = 0;
        }

        if (ret == XZ_STREAM_END) {
            break;
        }
        if (ret != XZ_OK && ret != XZ_UNSUPPORTED_CHECK) {
            success = false;
            break;
        }
    }

    if (cur_out_file) {
        fclose(cur_out_file);
        cur_out_file = NULL;
    }

    free(in_buf);
    free(out_buf);
    xz_dec_end(dec);
    fclose(in_fp);

    return (success && files_extracted > 100);
}
