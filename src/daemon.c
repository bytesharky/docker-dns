#include "daemon.h"
#include <fcntl.h>     // for open, O_RDONLY, O_RDWR, O_WRONLY, pid_t
#include <stdlib.h>    // for exit, EXIT_FAILURE, EXIT_SUCCESS
#include <sys/stat.h>  // for umask
#include <unistd.h>    // for close, fork, chdir, setsid, STDERR_FILENO, STD...

// 守护进程
void daemonize(void) {
    pid_t pid = fork();
    if (pid < 0) exit(EXIT_FAILURE);
    if (pid > 0) exit(EXIT_SUCCESS);
    if (setsid() < 0) exit(EXIT_FAILURE);
    pid = fork();
    if (pid < 0) exit(EXIT_FAILURE);
    if (pid > 0) exit(EXIT_SUCCESS);
    umask(0);
    chdir("/");
    close(STDIN_FILENO);
    close(STDOUT_FILENO);
    close(STDERR_FILENO);
    int fd = open("/dev/null", O_RDWR);
    dup2(fd, STDIN_FILENO);
    dup2(fd, STDOUT_FILENO);
    dup2(fd, STDERR_FILENO);
    if (fd > 2) close(fd);

}
