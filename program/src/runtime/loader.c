#include "runtime/loader.h"
#include "core/fs.h"
#include "core/log.h"

#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif

typedef const sbpc_module *(*module_fn)(const sbpc_rt *rt);

bool rt_load_module(rt_dispatch *d, const char *dir, const char *name, const char *source_hash,
                    char *err, size_t errlen)
{
    char file[128], path[SBPC_PATH_MAX], sym[160];
    module_fn get = NULL;
    const sbpc_module *m;
    snprintf(file, sizeof file, "%s%s", name, SBPC_MODULE_EXT);
    snprintf(sym, sizeof sym, "sbpc_module_%s", name);
    if (!fs_join(path, dir, file)) {
        snprintf(err, errlen, "path too long");
        return false;
    }
#ifdef _WIN32
    {
        wchar_t wpath[SBPC_PATH_MAX];
        HMODULE h;
        if (!MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, SBPC_PATH_MAX)) {
            snprintf(err, errlen, "bad path %s", path);
            return false;
        }
        h = LoadLibraryW(wpath);
        if (!h) {
            snprintf(err, errlen, "can't load %s (error %lu)", path, (unsigned long)GetLastError());
            return false;
        }
        get = (module_fn)(void (*)(void))GetProcAddress(h, sym);
    }
#else
    {
        void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
        if (!h) {
            snprintf(err, errlen, "can't load %s: %s", path, dlerror());
            return false;
        }
        *(void **)&get = dlsym(h, sym);
    }
#endif
    if (!get) {
        snprintf(err, errlen, "%s has no %s", path, sym);
        return false;
    }
    m = get(rt_api());
    if (!m || m->abi != SBPC_MODULE_ABI) {
        snprintf(err, errlen, "%s was built for another SBPC version (ABI %d, want %d)", path, m ? m->abi : -1,
                 SBPC_MODULE_ABI);
        return false;
    }
    if (source_hash && source_hash[0] && strcmp(m->source_hash, source_hash) != 0) {
        snprintf(err, errlen, "%s was built from a different disc", path);
        return false;
    }
    if (!rt_dispatch_add(d, m)) {
        snprintf(err, errlen, "out of memory");
        return false;
    }
    LOGI(SUB_CORE, "loaded recompiled module %s: %d entry points", name, m->count);
    return true;
}
