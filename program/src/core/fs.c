#include "core/fs.h"

#include <ctype.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#ifdef _WIN32
static bool widen(const char *s, wchar_t *out, int cap)
{
    return MultiByteToWideChar(CP_UTF8, 0, s, -1, out, cap) > 0;
}
static bool narrow(const wchar_t *s, char *out, int cap)
{
    return WideCharToMultiByte(CP_UTF8, 0, s, -1, out, cap, NULL, NULL) > 0;
}
#endif

FILE *fs_open(const char *path, const char *mode)
{
#ifdef _WIN32
    wchar_t wpath[SBPC_PATH_MAX], wmode[16];
    if (!widen(path, wpath, SBPC_PATH_MAX) || !widen(mode, wmode, 16))
        return NULL;
    return _wfopen(wpath, wmode);
#else
    return fopen(path, mode);
#endif
}

bool fs_seek(FILE *f, int64_t offset)
{
#ifdef _WIN32
    return _fseeki64(f, offset, SEEK_SET) == 0;
#else
    return fseeko(f, (off_t)offset, SEEK_SET) == 0;
#endif
}

int64_t fs_size(const char *path)
{
#ifdef _WIN32
    wchar_t w[SBPC_PATH_MAX];
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!widen(path, w, SBPC_PATH_MAX) || !GetFileAttributesExW(w, GetFileExInfoStandard, &d))
        return -1;
    if (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        return -1;
    return ((int64_t)d.nFileSizeHigh << 32) | d.nFileSizeLow;
#else
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode))
        return -1;
    return (int64_t)st.st_size;
#endif
}

bool fs_is_file(const char *path) { return fs_size(path) >= 0; }

bool fs_is_dir(const char *path)
{
#ifdef _WIN32
    wchar_t w[SBPC_PATH_MAX];
    DWORD a;
    if (!widen(path, w, SBPC_PATH_MAX))
        return false;
    a = GetFileAttributesW(w);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

static bool mkdir_one(const char *path)
{
    if (fs_is_dir(path))
        return true;
#ifdef _WIN32
    wchar_t w[SBPC_PATH_MAX];
    if (!widen(path, w, SBPC_PATH_MAX))
        return false;
    return CreateDirectoryW(w, NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
#else
    return mkdir(path, 0755) == 0 || errno == EEXIST;
#endif
}

static bool is_sep(char c) { return c == '/' || c == '\\'; }

bool fs_mkdirs(const char *path)
{
    char buf[SBPC_PATH_MAX];
    size_t n = strlen(path);
    if (n == 0 || n >= sizeof buf)
        return false;
    memcpy(buf, path, n + 1);
    for (size_t i = 1; i < n; i++) {
        if (is_sep(buf[i])) {
            char c = buf[i];
            buf[i] = 0;
            /* skip "C:" drive roots */
            if (!(i == 2 && buf[1] == ':'))
                mkdir_one(buf);
            buf[i] = c;
        }
    }
    return mkdir_one(buf);
}

bool fs_rename(const char *from, const char *to)
{
#ifdef _WIN32
    wchar_t a[SBPC_PATH_MAX], b[SBPC_PATH_MAX];
    if (!widen(from, a, SBPC_PATH_MAX) || !widen(to, b, SBPC_PATH_MAX))
        return false;
    return MoveFileExW(a, b, MOVEFILE_REPLACE_EXISTING) != 0;
#else
    return rename(from, to) == 0;
#endif
}

bool fs_list_dir(const char *dir, fs_dir_cb cb, void *user)
{
#ifdef _WIN32
    wchar_t pattern[SBPC_PATH_MAX];
    char pat8[SBPC_PATH_MAX], name[SBPC_PATH_MAX];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    if (!fs_join(pat8, dir, "*") || !widen(pat8, pattern, SBPC_PATH_MAX))
        return false;
    h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0)
            continue;
        if (!narrow(fd.cFileName, name, SBPC_PATH_MAX))
            continue;
        if (!cb(name, (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0, user))
            break;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return true;
#else
    DIR *d = opendir(dir);
    struct dirent *e;
    char full[SBPC_PATH_MAX];
    if (!d)
        return false;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        if (!fs_join(full, dir, e->d_name))
            continue;
        if (!cb(e->d_name, fs_is_dir(full), user))
            break;
    }
    closedir(d);
    return true;
#endif
}

bool fs_join(char *out, const char *a, const char *b)
{
    size_t la = strlen(a), lb = strlen(b);
    bool need_sep = la > 0 && !is_sep(a[la - 1]);
    if (la + lb + 2 > SBPC_PATH_MAX)
        return false;
    if (out != a)
        memmove(out, a, la);
    if (need_sep)
        out[la++] = SBPC_PATH_SEP;
    memmove(out + la, b, lb + 1);
    return true;
}

bool fs_dirname(char *out, const char *path)
{
    size_t n = strlen(path);
    if (n >= SBPC_PATH_MAX)
        return false;
    while (n > 0 && !is_sep(path[n - 1]))
        n--;
    while (n > 1 && is_sep(path[n - 1]))
        n--;
    if (n == 0) {
        strcpy(out, ".");
        return true;
    }
    memmove(out, path, n);
    out[n] = 0;
    return true;
}

const char *fs_basename(const char *path)
{
    const char *p = path, *last = path;
    for (; *p; p++)
        if (is_sep(*p))
            last = p + 1;
    return last;
}

bool fs_has_ext(const char *path, const char *ext)
{
    size_t lp = strlen(path), le = strlen(ext);
    if (lp < le)
        return false;
    for (size_t i = 0; i < le; i++)
        if (tolower((unsigned char)path[lp - le + i]) != tolower((unsigned char)ext[i]))
            return false;
    return true;
}

bool fs_exe_dir(char *out)
{
    char exe[SBPC_PATH_MAX];
#ifdef _WIN32
    wchar_t w[SBPC_PATH_MAX];
    DWORD n = GetModuleFileNameW(NULL, w, SBPC_PATH_MAX);
    if (n == 0 || n >= SBPC_PATH_MAX || !narrow(w, exe, SBPC_PATH_MAX))
        return false;
#else
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n <= 0)
        return false;
    exe[n] = 0;
#endif
    return fs_dirname(out, exe);
}
