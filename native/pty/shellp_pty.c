#include "shellp_pty.h"
#include <pty.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

pty_session_t pty_start(
    int rows, int cols,
    const char *cmd,
    const char *const argv[],
    const char *cwd,
    const char *const envp[]
) {
    pty_session_t session = { -1, -1, 0 };
    int master = -1, slave = -1;

    struct winsize ws;
    memset(&ws, 0, sizeof(ws));
    ws.ws_row = (rows > 0) ? rows : 24;
    ws.ws_col = (cols > 0) ? cols : 80;

    struct termios term;
    memset(&term, 0, sizeof(term));
    cfmakeraw(&term);
    term.c_lflag |= (ECHO | ISIG | ICANON);
    term.c_iflag |= (ICRNL | IXON);
    term.c_oflag |= (OPOST | ONLCR);

    if (openpty(&master, &slave, NULL, &term, &ws) < 0) {
        session.error_code = errno;
        return session;
    }

    pid_t pid = fork();
    if (pid < 0) {
        session.error_code = errno;
        close(master);
        close(slave);
        return session;
    }

    if (pid == 0) {
        /* Child process */
        close(master);

        setsid();
        if (ioctl(slave, TIOCSCTTY, 0) < 0) {
            /* ignore if fails */
        }

        dup2(slave, STDIN_FILENO);
        dup2(slave, STDOUT_FILENO);
        dup2(slave, STDERR_FILENO);
        if (slave > STDERR_FILENO) {
            close(slave);
        }

        if (cwd && cwd[0] != '\0') {
            (void) chdir(cwd);
        }

        if (envp) {
            execve(cmd, (char *const *) argv, (char *const *) envp);
        } else {
            execv(cmd, (char *const *) argv);
        }

        /* If exec fails */
        fprintf(stderr, "shellp: failed to execute %s: %s\n", cmd, strerror(errno));
        _exit(127);
    }

    /* Parent process */
    close(slave);

    /* Set master to non-blocking */
    int flags = fcntl(master, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(master, F_SETFL, flags | O_NONBLOCK);
    }

    session.pty_fd = master;
    session.pid = pid;
    session.error_code = 0;
    return session;
}

ssize_t pty_read_data(int fd, char *buf, size_t max_len) {
    if (fd < 0 || !buf || max_len == 0) return -1;
    ssize_t r = read(fd, buf, max_len);
    if (r < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return 0; /* no data right now */
        }
        return -1; /* error or closed */
    }
    return r;
}

ssize_t pty_write_data(int fd, const char *buf, size_t len) {
    if (fd < 0 || !buf || len == 0) return -1;
    size_t written = 0;
    while (written < len) {
        ssize_t w = write(fd, buf + written, len - written);
        if (w < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                usleep(5000);
                continue;
            }
            return -1;
        }
        written += (size_t) w;
    }
    return (ssize_t) written;
}

int pty_set_size(int fd, int rows, int cols) {
    if (fd < 0) return -1;
    struct winsize ws;
    memset(&ws, 0, sizeof(ws));
    ws.ws_row = (rows > 0) ? rows : 24;
    ws.ws_col = (cols > 0) ? cols : 80;
    return ioctl(fd, TIOCSWINSZ, &ws);
}

int pty_kill_process(pid_t pid, int sig) {
    if (pid <= 0) return -1;
    return kill(pid, sig);
}

int pty_check_exit(pid_t pid, int *exit_code) {
    if (pid <= 0) return 1;
    int status = 0;
    pid_t res = waitpid(pid, &status, WNOHANG);
    if (res == pid) {
        if (exit_code) {
            if (WIFEXITED(status)) *exit_code = WEXITSTATUS(status);
            else if (WIFSIGNALED(status)) *exit_code = 128 + WTERMSIG(status);
            else *exit_code = 1;
        }
        return 1; /* Exited */
    } else if (res == 0) {
        return 0; /* Still running */
    }
    return -1; /* Error */
}

void pty_close_fd(int fd) {
    if (fd >= 0) {
        close(fd);
    }
}
