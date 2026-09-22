#ifndef SHELLP_PTY_H
#define SHELLP_PTY_H

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int pty_fd;
    pid_t pid;
    int error_code;
} pty_session_t;

pty_session_t pty_start(
    int rows, int cols,
    const char *cmd,
    const char *const argv[],
    const char *cwd,
    const char *const envp[]
);

ssize_t pty_read_data(int fd, char *buf, size_t max_len);
ssize_t pty_write_data(int fd, const char *buf, size_t len);
int pty_set_size(int fd, int rows, int cols);
int pty_kill_process(pid_t pid, int sig);
int pty_check_exit(pid_t pid, int *exit_code);
void pty_close_fd(int fd);

#ifdef __cplusplus
}
#endif

#endif /* SHELLP_PTY_H */
