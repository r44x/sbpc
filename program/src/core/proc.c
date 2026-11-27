#include "core/proc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static wchar_t *widen(const char *s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    wchar_t *w = n > 0 ? malloc((size_t)n * sizeof *w) : NULL;
    if (w) MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    return w;
}

/* Quotes one argument the way CommandLineToArgvW / the MS C runtime parse it. */
static void append_arg(char *cmd, size_t cap, const char *a)
{
    size_t n = strlen(cmd);
    int bs = 0;
    if (n && n + 1 < cap) cmd[n++] = ' ';
    if (n + 1 < cap) cmd[n++] = '"';
    for (const char *p = a; *p && n + 4 < cap; p++) {
        if (*p == '\\') { bs++; cmd[n++] = '\\'; continue; }
        if (*p == '"') { for (int i = 0; i < bs + 1 && n + 2 < cap; i++) cmd[n++] = '\\'; }
        bs = 0;
        cmd[n++] = *p;
    }
    for (int i = 0; i < bs && n + 2 < cap; i++) cmd[n++] = '\\';   /* backslashes before the closing quote */
    if (n + 1 < cap) cmd[n++] = '"';
    cmd[n] = 0;
}

int proc_run(const char *const *argv, const char *log_path)
{
    static char cmd[32768];
    wchar_t *wcmd, *wlog = NULL;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
    HANDLE logh = INVALID_HANDLE_VALUE;
    DWORD code = (DWORD)-1;
    cmd[0] = 0;
    for (int i = 0; argv[i]; i++)
        append_arg(cmd, sizeof cmd, argv[i]);
    wcmd = widen(cmd);
    if (!wcmd)
        return -1;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    if (log_path && (wlog = widen(log_path)) != NULL) {
        logh = CreateFileW(wlog, GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        free(wlog);
    }
    if (logh != INVALID_HANDLE_VALUE) {
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = logh;
        si.hStdError = logh;
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    }
    if (!CreateProcessW(NULL, wcmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        free(wcmd);
        if (logh != INVALID_HANDLE_VALUE) CloseHandle(logh);
        return -1;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (logh != INVALID_HANDLE_VALUE) CloseHandle(logh);
    free(wcmd);
    return (int)code;
}

#else
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

int proc_run(const char *const *argv, const char *log_path)
{
    int status;
    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        int fd = log_path ? open(log_path, O_WRONLY | O_CREAT | O_TRUNC, 0644) : open("/dev/null", O_WRONLY);
        if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); close(fd); }
        execv(argv[0], (char *const *)argv);
        _exit(127);
    }
    if (waitpid(pid, &status, 0) < 0)
        return -1;
    if (WIFEXITED(status)) {
        int rc = WEXITSTATUS(status);
        return rc == 127 ? -1 : rc;
    }
    return -1;
}
#endif
