#include "daemon.h"

#ifdef _WIN32
/* Windows 版 daemonize：以“无窗口、脱离父控制台”的方式后台运行。
 *
 * 做法：把自身用 CREATE_NO_WINDOW | DETACHED_PROCESS 重新拉起一个子进程，
 * 父进程随即退出，子进程在后台无窗口运行；用环境变量做标记防止无限递归。
 * 子进程没有控制台时，把标准输入输出重定向到 NUL，避免写无效句柄崩溃。
 */
#include <windows.h>
#include <stdlib.h>
#include <stdio.h>
#include <wchar.h>

#define DOCKER_DNS_DAEMON_ENV "DOCKER_DNS_DAEMONIZED"

void daemonize(void) {
    /* 子进程（已被守护过）：直接返回，不再递归拉起 */
    if (getenv(DOCKER_DNS_DAEMON_ENV) != NULL) {
        if (GetConsoleWindow() == NULL) {
            /* 无控制台：把日志重定向到 NUL，防止 fprintf 写无效句柄 */
            freopen("NUL", "w", stdout);
            freopen("NUL", "w", stderr);
            freopen("NUL", "w", stdin);
        } else {
            ShowWindow(GetConsoleWindow(), SW_HIDE);
        }
        return;
    }

    /* 复制命令行（CreateProcessW 可能就地修改该缓冲区） */
    wchar_t cmdline[32768];
    wcsncpy(cmdline, GetCommandLineW(), 32767);
    cmdline[32767] = L'\0';

    /* 打标记，子进程据此跳过二次守护（继承到子进程环境块） */
    SetEnvironmentVariableA(DOCKER_DNS_DAEMON_ENV, "1");

    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    if (CreateProcessW(NULL, cmdline, NULL, NULL, FALSE,
                       CREATE_NO_WINDOW | DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP,
                       NULL, NULL, &si, &pi)) {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        exit(0);   /* 父进程退出，把控制权交给后台子进程 */
    }

    /* 兜底：拉起失败就隐藏当前窗口后继续前台跑 */
    HWND hwnd = GetConsoleWindow();
    if (hwnd) ShowWindow(hwnd, SW_HIDE);
}

#else
/* POSIX 版 daemonize：标准双 fork + setsid */
#include <fcntl.h>     // for open, O_RDWR
#include <stdlib.h>    // for exit, EXIT_FAILURE, EXIT_SUCCESS
#include <sys/stat.h>  // for umask
#include <unistd.h>    // for close, fork, chdir, setsid, STDERR_FILENO, STD...

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

#endif
