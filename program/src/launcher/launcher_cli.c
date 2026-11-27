#include "launcher/launcher.h"
#include "disc/chd.h"
#include "runtime/tcc_build.h"
#include "core/sha1.h"
#include "launcher/scan.h"
#include "analysis/analyse.h"
#include "analysis/boot.h"
#include "core/png.h"
#include "sh2/sh2_decode.h"
#include "sh2/sh2_interp.h"
#include "codegen/codegen.h"
#include "core/proc.h"
#include "runtime/loader.h"
#include "saturn/saturn.h"
#include "saturn/saturn_rec.h"
#include "disc/iso9660.h"
#include "core/config.h"
#include "core/log.h"
#include "core/paths.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *launcher_config_path(void)
{
    static char path[SBPC_PATH_MAX];
    if (!path[0])
        fs_join(path, paths_data_dir(), "sbpc.ini");
    return path;
}

static void print_identity(const char *cue, const disc_identity *id)
{
    printf("%s\n", cue);
    printf("  status : %s\n", id_status_name(id->status));
    if (id->version)
        printf("  game   : %s [%s]\n", id->version->name, id->version->id);
    if (id->have_header) {
        printf("  title  : %s\n", id->header.title);
        printf("  product: %s  version: %s  date: %s\n", id->header.product, id->header.version, id->header.date);
        printf("  areas  : %s (%s)  peripherals: %s\n", id->header.areas, id->region, id->header.peripherals);
        printf("  1st read: 0x%08X\n", (unsigned)id->header.first_read_addr);
    }
    if (id->data_sha1[0])
        printf("  data sha1: %s\n", id->data_sha1);
    printf("  tracks : %d\n", id->track_count);
    if (id->reason[0])
        printf("  note   : %s\n", id->reason);
}

/* Opens a disc and its filesystem for the files/analyse commands. */
/* SBPC_WATCHPC=addr,addr,...: prints the first hit of each address (frame, pr, r4-r7) while booting. */
static uint32_t g_watch[16];
static int g_nwatch;
static bool g_watch_hit[16];
static uint32_t *g_hist;
static bool g_hist_on;
/* SBPC_IHIST: interpreted instructions per PC over the whole run, both CPUs.
 * Slots 0-0x7FFFF cover low work RAM (0x00200000), 0x80000-0xFFFFF high work RAM (0x06000000). */
static uint32_t *g_ihist;
static uint64_t g_ihist_other;
/* Where interpreted runs begin: call, jump, longjmp or interrupt targets, and places native code
 * gave up, but not return sites or local branches. Each start is charged the instructions that run
 * until the next start. Starts without a recompiled entry are hint candidates (see versions.c). */
#define IH_STARTS 65536
static struct { uint32_t pc; uint64_t insns, hits; } g_ih_start[IH_STARTS];
static uint32_t g_ih_prev[2], g_ih_cur[2];
static uint16_t g_ih_op1[2], g_ih_op2[2];
static bool ih_is_local_branch(uint16_t op)
{
    return (op & 0xF000) == 0xA000 || (op & 0xF900) == 0x8900; /* bra, bt, bf, bt/s, bf/s */
}
static bool ih_is_call(uint16_t op)
{
    return (op & 0xF000) == 0xB000 || (op & 0xF0FF) == 0x400B || (op & 0xF0FF) == 0x0003; /* bsr, jsr, bsrf */
}
static uint32_t ih_slot(uint32_t pc)
{
    uint32_t h = (pc * 2654435761u) >> 16;
    while (g_ih_start[h].pc && g_ih_start[h].pc != pc) h = (h + 1) & (IH_STARTS - 1);
    g_ih_start[h].pc = pc;
    return h;
}
static void ihist_starts(sh2_state *s)
{
    int c = s->id & 1;
    uint32_t pc = s->pc & 0x1FFFFFFFu;
    if (pc != g_ih_prev[c] + 2 && pc != 0) {
        bool local = ih_is_local_branch(g_ih_op1[c]) || ih_is_local_branch(g_ih_op2[c]) ||
                     g_ih_op1[c] == 0x002B || g_ih_op2[c] == 0x002B; /* rte resumes interrupted code */
        if (!local && !ih_is_call(sh2_rd16(s, pc - 4))) {
            g_ih_cur[c] = ih_slot(pc);
            g_ih_start[g_ih_cur[c]].hits++;
        }
    }
    if (g_ih_start[g_ih_cur[c]].pc) g_ih_start[g_ih_cur[c]].insns++;
    g_ih_prev[c] = pc;
    g_ih_op2[c] = g_ih_op1[c];
    g_ih_op1[c] = sh2_rd16(s, pc);
}
static void ihist_probe(sh2_state *s)
{
    uint32_t a = s->pc & 0x1FFFFFFFu;
    ihist_starts(s);
    if ((a >> 20) == 0x002) g_ihist[(a & 0xFFFFF) >> 1]++;
    else if ((a >> 20) == 0x060) g_ihist[0x80000 + ((a & 0xFFFFF) >> 1)]++;
    else g_ihist_other++;
}
static uint32_t ihist_addr(uint32_t i) { return (i < 0x80000 ? 0x00200000u : 0x06000000u - 0x100000u) + i * 2; }
static void ihist_report(const rt_dispatch *d, int top)
{
    static uint64_t page[512];   /* 4 KB pages: 0-255 low, 256-511 high */
    uint64_t total = g_ihist_other;
    memset(page, 0, sizeof page);
    for (uint32_t i = 0; i < 0x100000; i++) { page[i >> 11] += g_ihist[i]; total += g_ihist[i]; }
    printf("ihist: %llu interpreted instructions (%llu outside work RAM)\n", (unsigned long long)total,
           (unsigned long long)g_ihist_other);
    for (int k = 0; k < 24; k++) {
        uint64_t best = 0; int bi = -1;
        for (int i = 0; i < 512; i++) if (page[i] > best) { best = page[i]; bi = i; }
        if (bi < 0) break;
        printf("ihist page %08X %5.1f%%\n", (unsigned)ihist_addr((uint32_t)bi << 11), 100.0 * (double)best / (double)total);
        page[bi] = 0;
    }
    for (int k = 0; k < top; k++) {
        uint32_t best = 0, bi = 0;
        for (uint32_t i = 0; i < 0x100000; i++) if (g_ihist[i] > best) { best = g_ihist[i]; bi = i; }
        if (!best) break;
        printf("ihist pc %08X %10u%s\n", (unsigned)ihist_addr(bi), (unsigned)best,
               rt_dispatch_find(d, ihist_addr(bi)) ? "  (has entry)" : "");
        g_ihist[bi] = 0;
    }
    for (int k = 0; k < top; k++) {
        uint64_t best = 0;
        int bi = -1;
        for (int i = 0; i < IH_STARTS; i++)
            if (g_ih_start[i].insns > best && !rt_dispatch_find(d, g_ih_start[i].pc)) { best = g_ih_start[i].insns; bi = i; }
        if (bi < 0) break;
        printf("ihist start %08X %5.1f%%  %llu entries\n", (unsigned)g_ih_start[bi].pc, 100.0 * (double)best / (double)total,
               (unsigned long long)g_ih_start[bi].hits);
        g_ih_start[bi].insns = 0;
    }
}
/* SBPC_WATCHMEM=addr: prints every change of the byte at addr (any address; SBPC_WATCHFROM=frame starts later),
 * with the PC that made it. */
static uint32_t g_wmem;
static uint64_t g_watch_from;   /* SBPC_WATCHFROM=frame: arm the PC watches from this frame on */
static int g_wmem_last = -1, g_wmem_n;
static saturn *g_wm;
static uint32_t g_ring[64];
static unsigned g_ring_pos;
static void watch_probe(sh2_state *s)
{
    if (s->id == 0)
        g_ring[g_ring_pos++ & 63] = s->pc;
    if (g_wmem && g_wmem_n < 40 && g_wm->frame >= g_watch_from) {
        int v = sh2_rd8(s, g_wmem);
        if (v != g_wmem_last) {
            if (g_wmem_last >= 0) {
                printf("mem %08X: %02X -> %02X  pc %08X pr %08X (%s) frame %u\n", (unsigned)g_wmem, g_wmem_last, v,
                       (unsigned)s->pc, (unsigned)s->pr, s->id ? "slave" : "master", (unsigned)g_wm->frame);
                g_wmem_n++;
            }
            g_wmem_last = v;
        }
    }
    if (g_hist_on && s->id == 0 && (s->pc >> 24) == 6) g_hist[(s->pc & 0xFFFFF) >> 1]++;
    for (int i = 0; i < g_nwatch; i++)
        if (!g_watch_hit[i] && s->pc == g_watch[i] && (!g_wm || g_wm->frame >= g_watch_from)) {
            g_watch_hit[i] = !getenv("SBPC_WATCHALL");   /* SBPC_WATCHALL=1: report every hit */
            if (getenv("SBPC_WATCHRING")) {
                printf("  last PCs:");
                for (unsigned k = 0; k < 64; k++) printf(" %08X", (unsigned)g_ring[(g_ring_pos + k) & 63]);
                printf("\n");
            }
            if (getenv("SBPC_WATCHREGS")) {
                printf("  regs:");
                for (int k = 0; k < 16; k++) printf(" r%d=%08X", k, (unsigned)s->r[k]);
                printf(" sr=%03X stack %08X %08X\n", (unsigned)s->sr, (unsigned)sh2_rd32(s, s->r[15]),
                       (unsigned)sh2_rd32(s, s->r[15] + 4));
            }
            printf("hit %08X: r0 %08X pr %08X r4 %08X r5 %08X r6 %08X r7 %08X r15 %08X\n", (unsigned)s->pc, (unsigned)s->r[0], (unsigned)s->pr,
                   (unsigned)s->r[4], (unsigned)s->r[5], (unsigned)s->r[6], (unsigned)s->r[7], (unsigned)s->r[15]);
        }
}

static bool open_fs(const char *cue, disc *d, iso_fs *fs)
{
    char err[256];
    int track;
    if (!disc_open(d, cue, err, sizeof err)) {
        fprintf(stderr, "%s\n", err);
        return false;
    }
    track = disc_first_data_track(d);
    if (track < 0 || !iso_open(fs, d, track, err, sizeof err)) {
        fprintf(stderr, "%s\n", track < 0 ? "no data track" : err);
        disc_close(d);
        return false;
    }
    return true;
}

static int cmd_files(const char *cue)
{
    disc d;
    iso_fs fs;
    if (!open_fs(cue, &d, &fs))
        return 1;
    printf("volume: %s\n%-32s %8s %10s\n", fs.volume_id, "path", "lba", "size");
    for (int i = 0; i < fs.count; i++)
        printf("%-32s %8u %10u%s\n", fs.files[i].path, (unsigned)fs.files[i].lba,
               (unsigned)fs.files[i].size, fs.files[i].is_dir ? "  <dir>" : "");
    iso_close(&fs);
    disc_close(&d);
    return 0;
}

static FILE *open_out(const char *dir, const char *name, char *path_out)
{
    if (!fs_join(path_out, dir, name))
        return NULL;
    return fs_open(path_out, "w");
}

/* Loads one module into a fresh image. Returns the entry point, or 0 on failure. */
static uint32_t load_module(iso_fs *fs, const game_module *m, mem_image *im, boot_info *bi)
{
    char err[256];
    const iso_file *f;
    uint8_t *buf;
    if (!m->path) {
        if (!boot_load(fs, im, bi, err, sizeof err)) {
            fprintf(stderr, "%s: %s\n", m->name, err);
            return 0;
        }
        printf("  %s: 1st read %s, %u bytes at %08X (IP %u bytes at %08X)\n", m->name, bi->first_file,
               (unsigned)bi->first_size, (unsigned)bi->first_addr, (unsigned)bi->ip_size, SATURN_IP_ADDR);
        return m->entry ? m->entry : bi->first_addr;
    }
    f = iso_find(fs, m->path);
    if (!f || f->is_dir) {
        fprintf(stderr, "%s: %s not found on the disc\n", m->name, m->path);
        return 0;
    }
    buf = malloc(f->size ? f->size : 1);
    if (!buf || !iso_read(fs, f, 0, buf, f->size) || !img_add(im, m->name, m->load_addr, buf, f->size)) {
        fprintf(stderr, "%s: cannot load %s\n", m->name, m->path);
        free(buf);
        return 0;
    }
    free(buf);
    printf("  %s: %s, %u bytes at %08X\n", m->name, m->path, (unsigned)f->size, (unsigned)m->load_addr);
    return m->entry ? m->entry : m->load_addr;
}

static void write_reports(analysis *a, const char *dir, const char *header)
{
    char path[SBPC_PATH_MAX];
    FILE *f;
    if ((f = open_out(dir, "summary.txt", path)) != NULL) {
        fprintf(f, "%s\n\n", header);
        an_write_summary(a, f);
        fclose(f);
    }
    if ((f = open_out(dir, "functions.txt", path)) != NULL) {
        an_write_functions(a, f);
        fclose(f);
    }
    if ((f = open_out(dir, "listing.txt", path)) != NULL) {
        an_write_listing(a, f);
        fclose(f);
    }
}

static int cmd_analyse(const char *cue)
{
    disc d;
    iso_fs fs;
    disc_identity id;
    const game_module *mods;
    int nmods, failed = 0;
    char root[SBPC_PATH_MAX];

    identify_disc(cue, &id, NULL, NULL);
    if (id.status == ID_NOT_SATURN || id.status == ID_BAD_IMAGE) {
        fprintf(stderr, "%s\n", id.reason);
        return 1;
    }
    mods = id.version ? id.version->modules : DEFAULT_MODULES;
    nmods = id.version ? id.version->module_count : DEFAULT_MODULE_COUNT;
    if (!open_fs(cue, &d, &fs))
        return 1;
    if (!fs_join(root, paths_data_dir(), "analysis") ||
        !fs_join(root, root, id.version ? id.version->id : id.header.product)) {
        fprintf(stderr, "path too long\n");
        return 1;
    }

    printf("%s (%s %s), %d module(s)\n", id.version ? id.version->name : id.header.title,
           id.header.product, id.header.version, nmods);

    for (int i = 0; i < nmods; i++) {
        const game_module *m = &mods[i];
        mem_image im;
        analysis a;
        boot_info bi;
        uint32_t entry;
        char dir[SBPC_PATH_MAX], header[256];

        printf("\n===== %s: %s =====\n", m->name, m->note ? m->note : "");
        img_init(&im);
        entry = load_module(&fs, m, &im, &bi);
        if (!entry) {
            img_free(&im);
            failed++;
            continue;
        }
        an_init(&a, &im);
        an_add_entry(&a, entry, FN_ENTRY);
        for (int h = 0; h < m->hint_count; h++) an_add_entry(&a, m->hints[h], FN_HINT);
        an_run(&a);
        an_write_summary(&a, stdout);

        snprintf(header, sizeof header, "module %s (%s), entry %08X", m->name,
                 m->path ? m->path : "1st read file", (unsigned)entry);
        if (fs_join(dir, root, m->name) && fs_mkdirs(dir))
            write_reports(&a, dir, header);
        else
            fprintf(stderr, "cannot create %s\n", dir);
        an_free(&a);
        img_free(&im);
    }

    printf("\nReports written to %s (one folder per module: summary.txt, functions.txt, listing.txt)\n", root);
    printf("These are derived from your disc: keep them local, never commit them.\n");
    iso_close(&fs);
    disc_close(&d);
    return failed ? 1 : 0;
}

/* Analyse every module and write generated C to <data>/generated/<id>/<module>/. */
/* ---- compiling generated modules with the bundled TinyCC (sbpc-tcc) ------------------------------ */

/* Compiles <root>/<module>/gen_<module>*.c (listed in gen_<module>.files) into <root>/<module>SBPC_MODULE_EXT. */
static bool compile_module(const char *root, const char *module, char *msg, size_t cap)
{
    char dir[SBPC_PATH_MAX], list[SBPC_PATH_MAX], out[SBPC_PATH_MAX], logp[SBPC_PATH_MAX], name[128], line[256],
        why[512];
    static char files[64][SBPC_PATH_MAX];
    const char *argv[64];
    int nfiles = 0;
    FILE *f;

    snprintf(name, sizeof name, "gen_%s.files", module);
    if (!fs_join(dir, root, module) || !fs_join(list, dir, name)) {
        snprintf(msg, cap, "path too long");
        return false;
    }
    snprintf(name, sizeof name, "%s%s", module, SBPC_MODULE_EXT);
    fs_join(out, root, name);
    snprintf(name, sizeof name, "%s.compile.log", module);
    fs_join(logp, root, name);
    if (!(f = fs_open(list, "r"))) {
        snprintf(msg, cap, "no file list for %s", module);
        return false;
    }
    while (nfiles < 64 && fgets(line, sizeof line, f)) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
        if (n && fs_join(files[nfiles], dir, line)) { argv[nfiles] = files[nfiles]; nfiles++; }
    }
    fclose(f);
    if (!tcc_build_module(argv, nfiles, out, root, logp, why, sizeof why)) {
        snprintf(msg, cap, "compiling %s failed: %.400s", module, why);
        LOGE(SUB_LAUNCHER, "%s", msg);
        return false;
    }
    LOGI(SUB_LAUNCHER, "compiled %s -> %s", module, out);
    return true;
}

/* A version's folder of built files: <built>/saturn-bomberman-<region>. Builds made before 2026-11-27 used
 * the short version id ("usa", "eur", "jpn", "jdemo"); such a folder is renamed the first time it's needed. */
bool launcher_game_dir(const game_version *v, char *out)
{
    char old[SBPC_PATH_MAX];
    if (!v || !fs_join(out, paths_built_dir(), v->folder))
        return false;
    if (!fs_is_dir(out) && fs_join(old, paths_built_dir(), v->id) && fs_is_dir(old)) {
        if (fs_rename(old, out))
            LOGI(SUB_LAUNCHER, "renamed %s to %s", old, out);
        else
            LOGW(SUB_LAUNCHER, "couldn't rename %s to %s", old, out);
    }
    return true;
}

bool launcher_load_modules(saturn *m, const disc_identity *id)
{
    char root[SBPC_PATH_MAX], err[512];
    int loaded = 0;
    if (!id->version || !launcher_game_dir(id->version, root))
        return false;
    for (int i = 0; i < id->version->module_count; i++) {
        if (rt_load_module(&m->dispatch, root, id->version->modules[i].name, id->data_sha1, err, sizeof err))
            loaded++;
        else
            LOGW(SUB_LAUNCHER, "recompiled module not used: %s", err);
    }
    LOGI(SUB_LAUNCHER, "%d of %d recompiled modules loaded", loaded, id->version->module_count);
    return loaded > 0;
}

/* A finished build leaves <built>/<version>/build.stamp naming the SBPC and generator versions and the disc's
 * data-track SHA-1. When it matches, the launcher offers "Play" instead of "Build & Play". */
static bool stamp_path(const disc_identity *id, char *out)
{
    return launcher_game_dir(id->version, out) &&
           fs_join(out, out, "build.stamp");
}

static void stamp_text(const disc_identity *id, char *out, size_t cap)
{
    snprintf(out, cap, "sbpc %s\ncodegen %d\ndisc %s\n", SBPC_VERSION_STRING, SBPC_CODEGEN_VERSION, id->data_sha1);
}

bool launcher_installed_image(const disc_identity *id, char *out, size_t cap)
{
    char dir[SBPC_PATH_MAX], name[256], path[SBPC_PATH_MAX];
    if (!id->version || !launcher_game_dir(id->version, dir) ||
        !fs_join(dir, dir, "disc"))
        return false;
    snprintf(name, sizeof name, "%s.cue", id->version->name);
    if (!fs_join(path, dir, name) || !fs_is_file(path))
        return false;
    snprintf(out, cap, "%s", path);
    return true;
}

/* Copies the disc into the recompiled folder as ISO+WAV, unless an identical copy is already there
 * (which is also the case when building from that copy). */
static bool install_disc(disc *d, const disc_identity *id, char *msg, size_t cap)
{
    char cue[SBPC_PATH_MAX], dir[SBPC_PATH_MAX], err[512];
    disc_identity have;
    if (launcher_installed_image(id, cue, sizeof cue)) {
        identify_disc(cue, &have, NULL, NULL);
        if (have.status == ID_SUPPORTED && strcmp(have.data_sha1, id->data_sha1) == 0)
            return true;
    }
    if (!launcher_game_dir(id->version, dir) || !fs_join(dir, dir, "disc")) {
        snprintf(msg, cap, "path too long");
        return false;
    }
    LOGI(SUB_LAUNCHER, "copying the game data to %s", dir);
    if (!disc_export_isowav(d, dir, id->version->name, NULL, NULL, err, sizeof err)) {
        snprintf(msg, cap, "copying the game data failed: %.400s", err);
        LOGE(SUB_LAUNCHER, "%s", msg);
        return false;
    }
    /* the copy must identify as the same disc */
    if (!launcher_installed_image(id, cue, sizeof cue)) {
        snprintf(msg, cap, "copying the game data failed");
        return false;
    }
    identify_disc(cue, &have, NULL, NULL);
    if (have.status != ID_SUPPORTED || strcmp(have.data_sha1, id->data_sha1) != 0) {
        snprintf(msg, cap, "the copied game data doesn't match the disc");
        LOGE(SUB_LAUNCHER, "%s (%s)", msg, cue);
        return false;
    }
    LOGI(SUB_LAUNCHER, "game data copied: %s", cue);
    return true;
}

bool launcher_build_current(const disc_identity *id)
{
    char inst[SBPC_PATH_MAX];
    char path[SBPC_PATH_MAX], want[128], have[128] = "";
    FILE *f;
    size_t n;
    if (id->status != ID_SUPPORTED || !id->version || !id->data_sha1[0] || !stamp_path(id, path))
        return false;
    f = fs_open(path, "rb");
    if (!f)
        return false;
    n = fread(have, 1, sizeof have - 1, f);
    fclose(f);
    have[n] = 0;
    stamp_text(id, want, sizeof want);
    /* built by this version, and the game's own copy of the disc is in place */
    return strcmp(have, want) == 0 && launcher_installed_image(id, inst, sizeof inst);
}

static void write_stamp(const disc_identity *id)
{
    char path[SBPC_PATH_MAX], text[128];
    FILE *f;
    if (!id->data_sha1[0] || !stamp_path(id, path) || !(f = fs_open(path, "wb")))
        return;
    stamp_text(id, text, sizeof text);
    fputs(text, f);
    fclose(f);
}

static bool art_none_path(const disc_identity *id, char *out);   /* launcher art, below */

static build_progress_cb g_build_cb;
static void *g_build_user;
void launcher_set_build_progress(build_progress_cb cb, void *user) { g_build_cb = cb; g_build_user = user; }
static void build_step(int step, int module, int modules)
{
    if (g_build_cb)
        g_build_cb(step, module, modules, g_build_user);
}

/* Analyses every module of the disc and writes the generated C to <built>/<version>.
 * Used by "--cli generate" and by the launcher's Build & Play. */
bool launcher_build(const char *cue, bool verbose, char *msg, size_t cap)
{
    disc d;
    iso_fs fs;
    disc_identity id;
    const game_module *mods;
    int nmods, failed = 0, functions = 0, insns = 0;
    char root[SBPC_PATH_MAX], cmsg[512] = "";

    identify_disc(cue, &id, NULL, NULL);
    if (id.status != ID_SUPPORTED) {
        snprintf(msg, cap, "%s", id.reason[0] ? id.reason : "unsupported disc");
        return false;
    }
    mods = id.version->modules;
    nmods = id.version->module_count;
    build_step(BUILD_STEP_READ, 0, nmods);
    if (!open_fs(cue, &d, &fs)) {
        snprintf(msg, cap, "can't read the disc");
        return false;
    }
    if (!launcher_game_dir(id.version, root)) {
        snprintf(msg, cap, "path too long");
        iso_close(&fs);
        disc_close(&d);
        return false;
    }
    for (int i = 0; i < nmods; i++) {
        const game_module *m = &mods[i];
        mem_image im;
        analysis a;
        boot_info bi;
        cg_stats st;
        cg_options opt = {m->name, id.data_sha1, 400};
        uint32_t entry;
        char dir[SBPC_PATH_MAX];

        img_init(&im);
        build_step(BUILD_STEP_FIND, i, nmods);
        if (verbose) printf("%s:\n", m->name);
        entry = load_module(&fs, m, &im, &bi);
        if (!entry || !fs_join(dir, root, m->name)) {
            img_free(&im);
            failed++;
            continue;
        }
        an_init(&a, &im);
        an_add_entry(&a, entry, FN_ENTRY);
        for (int h = 0; h < m->hint_count; h++) an_add_entry(&a, m->hints[h], FN_HINT);
        an_run(&a);
        build_step(BUILD_STEP_TRANSLATE, i, nmods);
        if (!cg_generate(&a, &opt, dir, &st)) {
            LOGE(SUB_LAUNCHER, "code generation failed for %s", m->name);
            failed++;
        } else if (build_step(BUILD_STEP_COMPILE, i, nmods), !compile_module(root, m->name, cmsg, sizeof cmsg)) {
            failed++;
        } else {
            functions += st.functions;
            insns += st.instructions;
            LOGI(SUB_LAUNCHER, "built %s: %d functions, %d instructions", m->name, st.functions, st.instructions);
            if (verbose)
                printf("  %d functions, %d instructions, %d files, %d resume points\n"
                       "  calls: %d direct, %d guarded, %d dynamic; %d dynamic jumps; %d interpreter fallbacks\n",
                       st.functions, st.instructions, st.files, st.resume_points, st.direct_calls,
                       st.guarded_calls, st.dynamic_calls, st.dynamic_jumps, st.fallbacks);
        }
        an_free(&a);
        img_free(&im);
    }
    if (!failed)
        build_step(BUILD_STEP_INSTALL, nmods, nmods);
    if (!failed && !install_disc(&d, &id, cmsg, sizeof cmsg))   /* the game's own copy: no dump needed to play */
        failed++;
    iso_close(&fs);
    disc_close(&d);
    if (!failed) {
        char amsg[SBPC_PATH_MAX + 32];
        char none[SBPC_PATH_MAX];
        write_stamp(&id);
        if (art_none_path(&id, none))
            remove(none);
        build_step(BUILD_STEP_ART, nmods, nmods);
        launcher_extract_art(cue, amsg, sizeof amsg);   /* optional: the launcher just shows no wallpaper */
    }
    if (failed)
        snprintf(msg, cap, "%d of %d modules failed to build: %.400s", failed, nmods, cmsg[0] ? cmsg : "see sbpc.log");
    else
        snprintf(msg, cap, "Built %d module(s): %d functions, %d instructions -> %s", nmods, functions, insns, root);
    build_step(BUILD_STEP_DONE, nmods, nmods);
    return failed == 0;
}

/* ---- launcher art -------------------------------------------------------------------------------
 * The launcher's details panel shows the Battle Game menu's bomb wallpaper behind a built game. The picture is
 * the game's own: after a build, the recompiled game runs headless along a scripted route to the Battle Game
 * setup screen, where VDP2 layer NBG2 holds the pattern (16 colours, 8x8 cells, two-word pattern names, a
 * 128x64 tile repeated over its 512x512 page). It is saved next to the build as launcher_bg.bmp; nothing of
 * the game is in SBPC itself. */
#define ART_W 128
#define ART_H 64

static bool art_path(const disc_identity *id, char *out)
{
    return id->version && launcher_game_dir(id->version, out) && fs_join(out, out, "launcher_bg.bmp");
}

/* Left next to the build when the wallpaper never showed up, so the launcher's scan doesn't run the game
 * again every time. A rebuild clears it. */
static bool art_none_path(const disc_identity *id, char *out)
{
    return id->version && launcher_game_dir(id->version, out) && fs_join(out, out, "launcher_bg.none");
}

bool launcher_art_tried(const disc_identity *id)
{
    char path[SBPC_PATH_MAX];
    return art_none_path(id, path) && fs_is_file(path);
}

bool launcher_art_path(const disc_identity *id, char *out, size_t cap)
{
    char path[SBPC_PATH_MAX];
    if (!art_path(id, path) || !fs_is_file(path))
        return false;
    snprintf(out, cap, "%s", path);
    return true;
}

static uint16_t vdp2_reg(const saturn *m, int off) { return (uint16_t)(m->vdp2_regs[off] << 8 | m->vdp2_regs[off + 1]); }

/* Renders NBG2's page (512x512, RGB) when the layer is set up as on the Battle Game screens. */
static bool art_render_nbg2(const saturn *m, uint8_t *rgb)
{
    uint16_t bgon = vdp2_reg(m, 0x20), chctlb = vdp2_reg(m, 0x2A), pncn2 = vdp2_reg(m, 0x34);
    uint16_t plsz = vdp2_reg(m, 0x3A), mpofn = vdp2_reg(m, 0x3C), mpab = vdp2_reg(m, 0x48);
    uint32_t map, paloff;
    if (!(bgon & 4) || (chctlb & 3) || (pncn2 & 0x8000) || ((plsz >> 4) & 3) || ((vdp2_reg(m, 0x0E) >> 12) & 3) > 1)
        return false;   /* off, not 16-colour 8x8 cells with two-word names on a 1x1 plane, or 24-bit colour RAM */
    map = ((((uint32_t)(mpofn >> 8) & 7) << 6 | (mpab & 0x3F)) * 0x4000u) & 0x7FFFF;
    paloff = (uint32_t)((vdp2_reg(m, 0xE4) >> 8) & 7) << 8;
    for (int ny = 0; ny < 64; ny++)
        for (int nx = 0; nx < 64; nx++) {
            const uint8_t *pn = m->vdp2_vram + map + (uint32_t)(ny * 64 + nx) * 4;
            uint16_t t1 = (uint16_t)(pn[0] << 8 | pn[1]), t2 = (uint16_t)(pn[2] << 8 | pn[3]);
            uint32_t chara = (t2 & 0x7FFFu) * 32u, pal = (t1 & 0x7Fu) << 4;
            for (int cy = 0; cy < 8; cy++)
                for (int cx = 0; cx < 8; cx++) {
                    int sx = (t1 & 0x4000) ? 7 - cx : cx, sy = (t1 & 0x8000) ? 7 - cy : cy;
                    uint8_t b = m->vdp2_vram[(chara + (uint32_t)(sy * 4 + sx / 2)) & 0x7FFFF];
                    uint32_t ci = (paloff + pal + (uint32_t)((sx & 1) ? (b & 15) : (b >> 4))) & 0x7FF;
                    uint16_t w = (uint16_t)(m->vdp2_cram[ci * 2] << 8 | m->vdp2_cram[ci * 2 + 1]);
                    uint8_t *o = rgb + ((size_t)(ny * 8 + cy) * 512 + (size_t)(nx * 8 + cx)) * 3;
                    o[0] = (uint8_t)((w & 31) << 3);
                    o[1] = (uint8_t)(((w >> 5) & 31) << 3);
                    o[2] = (uint8_t)(((w >> 10) & 31) << 3);
                }
        }
    return true;
}

/* The wallpaper: a 128x64 tile that repeats over the whole page and isn't one flat colour. */
static bool art_is_wallpaper(const uint8_t *rgb)
{
    bool flat = true;
    for (int y = 0; y < 512; y++)
        for (int x = 0; x < 512; x++) {
            const uint8_t *p = rgb + ((size_t)y * 512 + (size_t)x) * 3;
            const uint8_t *t = rgb + ((size_t)(y % ART_H) * 512 + (size_t)(x % ART_W)) * 3;
            if (memcmp(p, t, 3) != 0)
                return false;
            if (memcmp(p, rgb, 3) != 0)
                flat = false;
        }
    return !flat;
}

static bool art_write_bmp(const char *path, const uint8_t *rgb)
{
    uint8_t h[54] = {'B', 'M'};
    uint32_t size = 54 + ART_W * ART_H * 3;
    FILE *f = fs_open(path, "wb");
    if (!f)
        return false;
    h[2] = (uint8_t)size; h[3] = (uint8_t)(size >> 8); h[4] = (uint8_t)(size >> 16);
    h[10] = 54; h[14] = 40;
    h[18] = ART_W; h[22] = ART_H;          /* positive height: bottom-up rows */
    h[26] = 1; h[28] = 24;
    fwrite(h, 1, sizeof h, f);
    for (int y = ART_H - 1; y >= 0; y--)
        for (int x = 0; x < ART_W; x++) {
            const uint8_t *p = rgb + ((size_t)y * 512 + (size_t)x) * 3;
            uint8_t bgr[3] = {p[2], p[1], p[0]};
            fwrite(bgr, 1, 3, f);
        }
    return fclose(f) == 0;
}

/* Pad presses (frame ranges, Start/Down) that reach the Battle Game setup from power-on. Japan's title opens
 * GAME MODE first and needs longer presses; Europe matches the USA frame for frame from the intro skip on. */
typedef struct { int from, to; uint16_t mask; } art_press;
static const art_press ART_ROUTE_WEST[] = {
    {600, 604, 0x0800}, {720, 724, 0x0800}, {840, 844, 0x0800}, {960, 964, 0x0800}, {1080, 1084, 0x0800},
    {1150, 1154, 0x2000}, {1250, 1254, 0x0800}, {0, 0, 0}};
static const art_press ART_ROUTE_JAPAN[] = {
    {600, 607, 0x0800}, {700, 707, 0x0800}, {800, 807, 0x0800}, {900, 907, 0x0800}, {1000, 1007, 0x0800},
    {1650, 1657, 0x2000}, {1700, 1707, 0x0800}, {0, 0, 0}};

bool launcher_extract_art(const char *cue, char *msg, size_t cap)
{
    disc d;
    iso_fs fs;
    disc_identity id;
    saturn *m;
    char err[256], path[SBPC_PATH_MAX];
    int slot[SMPC_SLOTS];
    uint8_t *rgb;
    const art_press *route;
    bool found = false;

    identify_disc(cue, &id, NULL, NULL);
    if (id.status != ID_SUPPORTED || !art_path(&id, path)) {
        snprintf(msg, cap, "unsupported disc");
        return false;
    }
    route = id.version->id[0] == 'j' ? ART_ROUTE_JAPAN : ART_ROUTE_WEST;   /* every Japanese build */
    if (!open_fs(cue, &d, &fs)) {
        snprintf(msg, cap, "can't read the disc");
        return false;
    }
    m = saturn_create();
    rgb = malloc(512 * 512 * 3);
    if (!m || !rgb) {
        snprintf(msg, cap, "out of memory");
        free(rgb); saturn_destroy(m); iso_close(&fs); disc_close(&d);
        return false;
    }
    m->version = id.version;
    if (!saturn_boot(m, &fs, err, sizeof err)) {
        snprintf(msg, cap, "%s", err);
        free(rgb); saturn_destroy(m); iso_close(&fs); disc_close(&d);
        return false;
    }
    launcher_load_modules(m, &id);
    smpc_layout(m, 1, PORTS_AUTO, slot);
    for (int i = 0; i < 3000 && !found; i++) {
        uint16_t held = 0;
        for (const art_press *p = route; p->mask; p++)
            if (i >= p->from && i <= p->to) held |= p->mask;
        if (slot[0] >= 0)
            m->smpc.pad[slot[0]] = (uint16_t)~held | 0x0007;
        saturn_run_frame(m);
        if (i >= 1300 && i % 10 == 0 && art_render_nbg2(m, rgb) && art_is_wallpaper(rgb))
            found = true;
    }
    if (found && !art_write_bmp(path, rgb)) {
        snprintf(msg, cap, "can't write %s", path);
        found = false;
    } else if (found) {
        snprintf(msg, cap, "launcher art: %s", path);
    } else {
        char none[SBPC_PATH_MAX];
        FILE *f;
        snprintf(msg, cap, "the Battle Game wallpaper didn't show up");
        if (art_none_path(&id, none) && (f = fs_open(none, "wb")) != NULL)
            fclose(f);
    }
    free(rgb);
    saturn_destroy(m);
    iso_close(&fs);
    disc_close(&d);
    LOGI(SUB_LAUNCHER, "%s", msg);
    return found;
}

static int cmd_generate(const char *cue)
{
    char msg[512];
    bool ok = launcher_build(cue, true, msg, sizeof msg);
    printf("\n%s\n", msg);
    if (ok)
        printf("It is derived from your disc: keep it local, never commit it.\n");
    else
        fprintf(stderr, "build failed\n");
    return ok ? 0 : 1;
}

/* Boots the disc headless on the interpreter and reports what the game did. */
static int cmd_boot(const char *cue, int frames, const char *shot, int shot_every)
{
    disc d;
    iso_fs fs;
    disc_identity id;
    saturn *m;
    char err[256], path[SBPC_PATH_MAX];
    FILE *f;
    int shot_from = getenv("SBPC_SHOT_FROM") ? atoi(getenv("SBPC_SHOT_FROM")) : 0;   /* developer: skip early shots */

    identify_disc(cue, &id, NULL, NULL);
    if (id.status != ID_SUPPORTED) {
        fprintf(stderr, "%s\n", id.reason[0] ? id.reason : "unsupported disc");
        return 1;
    }
    if (!open_fs(cue, &d, &fs))
        return 1;
    m = saturn_create();
    if (!m) {
        fprintf(stderr, "out of memory\n");
        return 1;
    }
    m->version = id.version;
    if (!saturn_boot(m, &fs, err, sizeof err)) {
        fprintf(stderr, "%s\n", err);
        saturn_destroy(m);
        return 1;
    }
    if (getenv("SBPC_WIDE"))   /* developer: widescreen, extra pixels per side (53 = 16:9) */
        m->wide_extra = atoi(getenv("SBPC_WIDE"));
    if (getenv("SBPC_BACKUP")) {   /* developer: SBPC_BACKUP=file keeps backup RAM in that file (default: memory only) */
        if (!saturn_backup_attach(m, getenv("SBPC_BACKUP"), err, sizeof err))
            fprintf(stderr, "%s\n", err);
    }
    {   /* SBPC_INTERP=1 forces the interpreter even when recompiled modules exist */
        bool native = !getenv("SBPC_INTERP") && launcher_load_modules(m, &id);
        printf("Booting %s for %d frames (%s)...\n", id.version->name, frames,
               native ? "recompiled code + interpreter" : "interpreter only");
    }
    if (getenv("SBPC_WATCHPC")) {
        const char *p = getenv("SBPC_WATCHPC");
        while (p && *p && g_nwatch < 16) {
            { char *end; g_watch[g_nwatch++] = (uint32_t)strtoul(p, &end, 16); p = end; }
            if (*p == ',') p++;
        }
        m->dispatch.probe = watch_probe;
        g_wm = m;
        if (getenv("SBPC_WATCHFROM")) g_watch_from = strtoull(getenv("SBPC_WATCHFROM"), NULL, 10);
    }
    if (getenv("SBPC_WATCHMEM")) {
        g_wmem = (uint32_t)strtoul(getenv("SBPC_WATCHMEM"), NULL, 16);
        g_wm = m;
        if (getenv("SBPC_WATCHFROM")) g_watch_from = strtoull(getenv("SBPC_WATCHFROM"), NULL, 10);
        m->dispatch.probe = watch_probe;
    }
    if (getenv("SBPC_PCHIST")) {
        g_hist = calloc(0x80000, sizeof *g_hist);
        m->dispatch.probe = watch_probe;
    }
    if (getenv("SBPC_IHIST")) {
        g_ihist = calloc(0x100000, sizeof *g_ihist);
        if (g_ihist) m->dispatch.on_interp = ihist_probe;
    }
    FILE *wav = getenv("SBPC_WAV") ? fopen(getenv("SBPC_WAV"), "wb") : NULL;
    /* developer: SBPC_PLAYERS=n (1-12) and SBPC_PORTS=auto|direct|tap1|tap2|both plug in that many pads */
    int slot[SMPC_SLOTS];
    {
        const char *pm = getenv("SBPC_PORTS");
        int mode = !pm ? PORTS_AUTO : strcmp(pm, "direct") == 0 ? PORTS_DIRECT : strcmp(pm, "tap1") == 0 ? PORTS_TAP1
                 : strcmp(pm, "tap2") == 0 ? PORTS_TAP2 : strcmp(pm, "both") == 0 ? PORTS_TAP_BOTH : PORTS_AUTO;
        int players = getenv("SBPC_PLAYERS") ? atoi(getenv("SBPC_PLAYERS")) : 1;
        if (id.version->max_players > 0 && players > id.version->max_players)
            players = id.version->max_players;   /* the game's limit (10) */
        int placed = smpc_layout(m, players, mode, slot);
        if (getenv("SBPC_PLAYERS") || pm)
            printf("Controllers: %d player(s), %s\n", placed, smpc_layout_name(mode));
    }
    /* developer: SBPC_REPLAY=file plays back a recording made by the game window (<logs>/input.rec) */
    input_rec replay = {0}, record = {0};
    int replay_players = -1, replay_ports = -1, layout_players = getenv("SBPC_PLAYERS") ? atoi(getenv("SBPC_PLAYERS")) : 1;
    int layout_ports = PORTS_AUTO;
    if (getenv("SBPC_PORTS")) {
        const char *pm = getenv("SBPC_PORTS");
        layout_ports = strcmp(pm, "direct") == 0 ? PORTS_DIRECT : strcmp(pm, "tap1") == 0 ? PORTS_TAP1
                     : strcmp(pm, "tap2") == 0 ? PORTS_TAP2 : strcmp(pm, "both") == 0 ? PORTS_TAP_BOTH : PORTS_AUTO;
    }
    if (getenv("SBPC_RECORD") && !rec_create(&record, getenv("SBPC_RECORD"), m->rtc_start))   /* developer: as the game window does */
        fprintf(stderr, "can't write %s\n", getenv("SBPC_RECORD"));
    if (getenv("SBPC_REPLAY")) {
        int64_t t0;
        if (rec_open(&replay, getenv("SBPC_REPLAY"), &t0)) {
            m->rtc_start = t0;
            printf("Replaying %s\n", getenv("SBPC_REPLAY"));
        } else {
            fprintf(stderr, "can't read the recording %s\n", getenv("SBPC_REPLAY"));
        }
    }
    for (int i = 0; i < frames; i++) {
        bool replayed = false;
        g_hist_on = g_hist && i >= frames - 30;
        if (replay.f) {
            rec_frame fr;
            if (!rec_read(&replay, &fr)) {
                printf("Recording ended at frame %d\n", i);
                rec_close(&replay);
            } else {
                if (fr.players != replay_players || fr.ports != replay_ports) {
                    replay_players = fr.players;
                    replay_ports = fr.ports;
                    smpc_layout(m, replay_players, replay_ports, slot);
                }
                memcpy(m->smpc.pad, fr.pad, sizeof fr.pad);
                replayed = true;
            }
        }
        if (!replayed) {   /* developer: SBPC_INPUT="[player@]first-last:mask,..." holds buttons (hex, active high:
             * 0800 Start, 0400 A, 0100 B, 0200 C, 1000 Up, 2000 Down, 4000 Left, 8000 Right);
             * player is 1-12 (default 1), mapped through the SBPC_PLAYERS layout */
            const char *e = getenv("SBPC_INPUT");
            uint16_t held[SMPC_SLOTS] = {0};
            while (e && *e) {
                char *end;
                int player = 1;
                long a = strtol(e, &end, 10), b;
                if (*end == '@') { player = (int)a; a = strtol(end + 1, &end, 10); }
                b = a;
                if (*end == '-') b = strtol(end + 1, &end, 10);
                if (*end != ':') break;
                {
                    unsigned long mask = strtoul(end + 1, &end, 16);
                    if (i >= a && i <= b && player >= 1 && player <= SMPC_SLOTS) held[player - 1] |= (uint16_t)mask;
                }
                e = *end == ',' ? end + 1 : NULL;
            }
            for (int pl = 0; pl < SMPC_SLOTS; pl++)
                if (slot[pl] >= 0) m->smpc.pad[slot[pl]] = (uint16_t)~held[pl] | 0x0007;
        }
        if (record.f) {
            rec_frame fr;
            fr.players = (uint8_t)(replay_players >= 0 ? replay_players : layout_players);
            fr.ports = (uint8_t)(replay_ports >= 0 ? replay_ports : layout_ports);
            memcpy(fr.pad, m->smpc.pad, sizeof fr.pad);
            rec_write(&record, &fr);
        }
        if (getenv("SBPC_POKE")) {   /* developer: SBPC_POKE=frame:hexaddr=hexbyte,... writes high work RAM */
            const char *e = getenv("SBPC_POKE");
            while (e && *e) {
                unsigned long fr = 0, addr = 0, val = 0;
                if (sscanf(e, "%lu:%lx=%lx", &fr, &addr, &val) == 3 && fr == (unsigned long)m->frame &&
                    addr >= 0x06000000ul && addr < 0x06100000ul)
                    m->wram_h[addr & 0xFFFFF] = (uint8_t)val;
                e = strchr(e, ',');
                if (e) e++;
            }
        }
        saturn_run_frame(m);
        if (wav) {   /* developer: SBPC_WAV=file.raw captures the sound output (s16le stereo 44.1 kHz) */
            scsp_state *sp = &m->scsp;
            while (sp->out_r != sp->out_w) {
                fwrite(&sp->out[(sp->out_r % SCSP_OUT_FRAMES) * 2], 4, 1, wav);
                sp->out_r++;
            }
        }
        if (shot && (shot_every > 0 ? (i + 1) % shot_every == 0 && i + 1 >= shot_from : i + 1 == frames)) {
            static uint32_t pix[704 * 512];
            int w, h;
            char name[SBPC_PATH_MAX], file[64];
            saturn_render(m, pix, &w, &h);
            snprintf(file, sizeof file, shot_every > 0 ? "frame_%04d.png" : "screenshot.png", i + 1);
            if (fs_join(name, paths_log_dir(), file) && png_write(name, pix, w, h))
                printf("Screenshot (%dx%d): %s\n", w, h, name);
        }
        if (m->cpu[0].sleeping && !m->slave_on)
            break;
    }
    if (g_hist) {
        for (int k = 0; k < (atoi(getenv("SBPC_PCHIST")) > 1 ? atoi(getenv("SBPC_PCHIST")) : 40); k++) {
            uint32_t best = 0, bi = 0;
            for (uint32_t i = 0; i < 0x80000; i++) if (g_hist[i] > best) { best = g_hist[i]; bi = i; }
            if (!best) break;
            printf("hist %08X %u\n", 0x06000000u + bi * 2, (unsigned)best);
            g_hist[bi] = 0;
        }
    }
    if (g_ihist) {
        ihist_report(&m->dispatch, atoi(getenv("SBPC_IHIST")) > 1 ? atoi(getenv("SBPC_IHIST")) : 40);
        free(g_ihist);
        g_ihist = NULL;
    }
    saturn_report(m, stdout);
    if (fs_join(path, paths_log_dir(), "boot_report.txt") && (f = fs_open(path, "w")) != NULL) {
        saturn_report(m, f);
        fclose(f);
        printf("\nReport saved to %s\n", path);
    }
    if (wav) fclose(wav);
    rec_close(&replay);
    rec_close(&record);
    if (getenv("SBPC_DUMP_WRAM")) {   /* developer: high work RAM at the end of the run (with scripted input) */
        FILE *df = fopen(getenv("SBPC_DUMP_WRAM"), "wb");
        if (df) { fwrite(m->wram_h, 1, 0x100000, df); fclose(df); }
    }
    if (getenv("SBPC_DUMP_WRAML")) {  /* developer: low work RAM (where PROGRAM.BIN chunks load) */
        FILE *df = fopen(getenv("SBPC_DUMP_WRAML"), "wb");
        if (df) { fwrite(m->wram_l, 1, 0x100000, df); fclose(df); }
    }
    if (getenv("SBPC_DUMP_VDP1")) {   /* developer: VDP1 VRAM (sprite graphics and command tables) */
        FILE *df = fopen(getenv("SBPC_DUMP_VDP1"), "wb");
        if (df) { fwrite(m->vdp1_vram, 1, 0x80000, df); fclose(df); }
    }
    if (getenv("SBPC_DUMP_VDP2")) {   /* developer: VDP2 VRAM (512 KB), colour RAM (4 KB), registers (512 B) */
        FILE *df = fopen(getenv("SBPC_DUMP_VDP2"), "wb");
        if (df) {
            fwrite(m->vdp2_vram, 1, 0x80000, df);
            fwrite(m->vdp2_cram, 1, 0x1000, df);
            fwrite(m->vdp2_regs, 1, sizeof m->vdp2_regs, df);
            fclose(df);
        }
    }
    saturn_destroy(m);
    iso_close(&fs);
    disc_close(&d);
    return 0;
}

/* Developer tool: boots for `frames` frames, then calls a game function once per record of `in`, on the
 * interpreter, and writes what it left in low work RAM. Used to run the game's own graphics unpacker over
 * data it never loads. Records in: u32 r6, u32 out_len, u32 blob_len, blob (big-endian). The blob is
 * placed at 0x002C0000 (r4), r5 = 0x00200000 (output); out: u32 status (0 ok, 1 too many steps),
 * then out_len bytes from 0x00200000. */
static int cmd_call(const char *cue, uint32_t func, int frames, const char *in_path, const char *out_path)
{
    disc d;
    iso_fs fs;
    disc_identity id;
    saturn *m;
    char err[256];
    FILE *in, *out;
    uint8_t hdr[12];
    int n = 0;

    identify_disc(cue, &id, NULL, NULL);
    if (id.status != ID_SUPPORTED || !open_fs(cue, &d, &fs))
        return 1;
    m = saturn_create();
    m->version = id.version;
    if (!saturn_boot(m, &fs, err, sizeof err)) { fprintf(stderr, "%s\n", err); return 1; }
    for (int i = 0; i < frames; i++)
        saturn_run_frame(m);
    in = fopen(in_path, "rb");
    out = fopen(out_path, "wb");
    if (!in || !out) { fprintf(stderr, "cannot open %s or %s\n", in_path, out_path); return 1; }
    while (fread(hdr, 1, 12, in) == 12) {
        uint32_t r6 = (uint32_t)hdr[0] << 24 | (uint32_t)hdr[1] << 16 | (uint32_t)hdr[2] << 8 | hdr[3];
        uint32_t olen = (uint32_t)hdr[4] << 24 | (uint32_t)hdr[5] << 16 | (uint32_t)hdr[6] << 8 | hdr[7];
        uint32_t blen = (uint32_t)hdr[8] << 24 | (uint32_t)hdr[9] << 16 | (uint32_t)hdr[10] << 8 | hdr[11];
        sh2_state *s = &m->cpu[0];
        uint8_t st[4] = {0, 0, 0, 0};
        long steps = 0;
        if (blen > 0x40000u || olen > 0xC0000u) { fprintf(stderr, "record %d too large\n", n); break; }
        memset(m->wram_l, 0, 0x100000);
        if (fread(m->wram_l + 0xC0000, 1, blen, in) != blen) break;
        s->r[4] = 0x002C0000u;
        s->r[5] = 0x00200000u;
        s->r[6] = r6;
        s->r[15] = 0x060FF000u;     /* a stack clear of the game's own */
        s->pr = 0x06FFFFF0u;        /* return address nothing else uses */
        s->pc = func;
        s->sr |= SR_IMASK;          /* no interrupts during the call */
        while (s->pc != 0x06FFFFF0u && steps < 5000000L) { sh2_step(s); steps++; }
        if (s->pc != 0x06FFFFF0u) st[3] = 1;
        fwrite(st, 1, 4, out);
        fwrite(m->wram_l, 1, olen, out);
        fflush(out);
        n++;
    }
    fclose(in);
    fclose(out);
    printf("%d calls\n", n);
    saturn_destroy(m);
    iso_close(&fs);
    disc_close(&d);
    return 0;
}

/* Developer tool: boots for `frames` frames, then disassembles high work RAM. */
static int cmd_disasm(const char *cue, uint32_t from, int count, int frames)
{
    disc d;
    iso_fs fs;
    disc_identity id;
    saturn *m;
    char err[256];

    identify_disc(cue, &id, NULL, NULL);
    if (id.status != ID_SUPPORTED || !open_fs(cue, &d, &fs))
        return 1;
    m = saturn_create();
    m->version = id.version;
    if (!saturn_boot(m, &fs, err, sizeof err)) { fprintf(stderr, "%s\n", err); return 1; }
    for (int i = 0; i < frames; i++)
        saturn_run_frame(m);
    if (getenv("SBPC_DUMP_WRAM")) {
        FILE *df = fopen(getenv("SBPC_DUMP_WRAM"), "wb");
        if (df) { fwrite(m->wram_h, 1, 0x100000, df); fclose(df); }
    }
    for (int i = 0; i < count; i++) {
        uint32_t a = from + (uint32_t)i * 2;
        const uint8_t *p = (a >= 0x06000000u && a < 0x06100000u) ? m->wram_h + (a & 0xFFFFF)
                         : (a >= 0x00200000u && a < 0x00300000u) ? m->wram_l + (a & 0xFFFFF) : NULL;
        sh2_insn in;
        char text[96];
        if (!p) break;
        uint16_t w = (uint16_t)(p[0] << 8 | p[1]);
        sh2_decode(w, a, &in);
        sh2_disasm(&in, text, sizeof text);
        printf("%08X  %04X  %s\n", (unsigned)a, w, text);
    }
    saturn_destroy(m);
    iso_close(&fs);
    disc_close(&d);
    return 0;
}

/* Writes a CHD's tracks as a redump-style cue/bin set: one .bin per track holding its pregap (INDEX 00)
 * then the track, raw 2352-byte sectors (data sectors with sync/header/ECC, audio little-endian). */
static int cmd_extract(const char *image, const char *cue_out)
{
    disc d;
    char err[256], dir[SBPC_PATH_MAX], base[SBPC_PATH_MAX], name[SBPC_PATH_MAX], path[SBPC_PATH_MAX];
    FILE *cue;
    size_t n;
    if (!disc_open(&d, image, err, sizeof err)) { fprintf(stderr, "%s\n", err); return 1; }
    if (!d.chd) { fprintf(stderr, "only .chd images can be extracted (a .cue already is cue/bin)\n"); disc_close(&d); return 1; }
    if (!fs_dirname(dir, cue_out)) snprintf(dir, sizeof dir, ".");
    snprintf(base, sizeof base, "%s", fs_basename(cue_out));
    n = strlen(base);
    if (n > 4 && fs_has_ext(base, ".cue")) base[n - 4] = 0;
    if (!(cue = fs_open(cue_out, "w"))) { fprintf(stderr, "can't write %s\n", cue_out); disc_close(&d); return 1; }
    for (int t = 0; t < d.cue.track_count; t++) {
        const cue_track *tr = &d.cue.tracks[t];
        int64_t total = tr->index1 + tr->sectors;
        bool audio = !track_is_data(tr);
        FILE *bin;
        snprintf(name, sizeof name, "%s (Track %02d).bin", base, t + 1);
        if (!fs_join(path, dir, name) || !(bin = fs_open(path, "wb"))) {
            fprintf(stderr, "can't write %s\n", name);
            fclose(cue);
            disc_close(&d);
            return 1;
        }
        for (int64_t sct = 0; sct < total; sct++) {
            uint8_t frame[CHD_FRAME];
            if (!chd_read_frame(d.chd, (uint32_t)(tr->data_offset + sct), frame, true)) {
                fprintf(stderr, "read error in track %d\n", t + 1);
                fclose(bin);
                fclose(cue);
                disc_close(&d);
                return 1;
            }
            if (audio)   /* CHD keeps CD audio big-endian; .bin is little-endian */
                for (int i = 0; i < 2352; i += 2) { uint8_t x = frame[i]; frame[i] = frame[i + 1]; frame[i + 1] = x; }
            fwrite(frame, 1, 2352, bin);
        }
        fclose(bin);
        fprintf(cue, "FILE \"%s\" BINARY\n  TRACK %02d %s\n", name, t + 1,
                audio ? "AUDIO" : tr->mode == TRACK_MODE2_2352 ? "MODE2/2352" : "MODE1/2352");
        if (tr->index1 > 0) {
            int64_t f = tr->index1;
            fprintf(cue, "    INDEX 00 00:00:00\n    INDEX 01 %02d:%02d:%02d\n", (int)(f / 4500), (int)(f / 75 % 60), (int)(f % 75));
        } else {
            fprintf(cue, "    INDEX 01 00:00:00\n");
        }
        printf("track %2d: %lld sectors -> %s\n", t + 1, (long long)total, name);
    }
    fclose(cue);
    disc_close(&d);
    printf("wrote %s\n", cue_out);
    return 0;
}

static void usage(void)
{
    printf("usage: sbpc --cli <command>      (<image> is a .cue, for cue/bin or ISO+WAV, or a .chd)\n"
           "  scan [folder] [--strict]  identify every disc image (default: saved games folder)\n"
           "                            --strict: exit 1 unless at least one disc is supported\n"
           "  identify <image>          identify one disc and print its details\n"
           "  files <image>             list the files on the disc\n"
           "  analyse <image>           find the game's code; reports go to the data folder\n"
           "  generate <image>          write and compile the recompiled code for every module\n"
           "  art <image>               save the launcher wallpaper from a built game (runs it headless)\n"
           "  boot <image> [frames] [--screenshot] [--screenshot-every=N]\n"
           "                            run the game headless and report; screenshots go to the\n"
           "                            logs folder\n"
           "  disasm <image> <hexaddr> <count> [frames]  disassemble work RAM after booting (developer)\n"
           "  chdverify <file.chd>      decode every hunk of a CHD and check its CRC\n"
           "  extract <file.chd> <out.cue>  write a CHD as a cue/bin set\n"
           "  tracksha <image> <track> [out.raw]  SHA-1 (and dump) of an audio track's PCM\n"
           "  set-games-folder <folder> remember the games folder\n"
           "  info                      print data/log folders\n");
}

int launcher_cli(int argc, char **argv)
{
    config cfg;
    config_load(&cfg, launcher_config_path());

    if (argc < 1 || strcmp(argv[0], "help") == 0 || strcmp(argv[0], "--help") == 0) {
        usage();
        return argc < 1 ? 2 : 0;
    }

    if (strcmp(argv[0], "info") == 0) {
        printf("version : %s\n", SBPC_VERSION_STRING);
        printf("data    : %s%s\n", paths_data_dir(), paths_is_portable() ? " (portable)" : "");
        printf("logs    : %s\n", paths_log_dir());
        printf("built   : %s\n", paths_built_dir());
        printf("games   : %s\n", config_get(&cfg, "games_dir", paths_games_dir() ? paths_games_dir() : "(not set)"));
        return 0;
    }

    if (strcmp(argv[0], "identify") == 0) {
        disc_identity id;
        if (argc < 2) {
            usage();
            return 2;
        }
        identify_disc(argv[1], &id, NULL, NULL);
        print_identity(argv[1], &id);
        return id.status == ID_SUPPORTED ? 0 : 1;
    }

    if ((strcmp(argv[0], "files") == 0 || strcmp(argv[0], "analyse") == 0 ||
         strcmp(argv[0], "analyze") == 0 || strcmp(argv[0], "generate") == 0) && argc < 2) {
        usage();
        return 2;
    }
    if (strcmp(argv[0], "extract") == 0 && argc >= 3)
        return cmd_extract(argv[1], argv[2]);
    if (strcmp(argv[0], "tracksha") == 0 && argc >= 3) {   /* SHA-1 of an audio track as the game hears it */
        disc d;
        char err[256], hex[41];
        int t = atoi(argv[2]) - 1;
        uint8_t raw[2352], digest[20];
        sha1_ctx ctx;
        if (!disc_open(&d, argv[1], err, sizeof err)) { fprintf(stderr, "%s\n", err); return 1; }
        if (t < 0 || t >= d.cue.track_count) { fprintf(stderr, "no track %s\n", argv[2]); disc_close(&d); return 1; }
        FILE *dump = argc >= 4 ? fopen(argv[3], "wb") : NULL;   /* optional: write the PCM out */
        sha1_init(&ctx);
        for (int64_t sct = 0; sct < d.cue.tracks[t].sectors; sct++) {
            if (!disc_read_audio(&d, t, sct, raw)) { fprintf(stderr, "read error at sector %lld\n", (long long)sct); disc_close(&d); return 1; }
            sha1_update(&ctx, raw, 2352);
            if (dump) fwrite(raw, 1, 2352, dump);
        }
        if (dump) fclose(dump);
        sha1_final(&ctx, digest);
        sha1_hex(digest, hex);
        printf("track %d: %lld sectors, sha1 %s\n", t + 1, (long long)d.cue.tracks[t].sectors, hex);
        disc_close(&d);
        return 0;
    }
    if (strcmp(argv[0], "chdverify") == 0 && argc >= 2) {   /* decode every hunk and check its CRC-16 */
        char err[256];
        long bad;
        chd *c = chd_open(argv[1], err, sizeof err);
        if (!c) { fprintf(stderr, "%s\n", err); return 1; }
        for (int i = 0; i < chd_track_count(c); i++) {
            const chd_track *t = chd_get_track(c, i);
            printf("track %2d: type %d, %6d frames, pregap %3d%s, first frame %u\n", i + 1, t->type, t->frames, t->pregap,
                   t->pregap_stored ? " (stored)" : "", (unsigned)t->first);
        }
        bad = chd_verify(c, NULL, NULL);
        printf("%s: %ld bad hunk(s)\n", bad == 0 ? "OK" : "FAILED", bad);
        chd_close(c);
        return bad == 0 ? 0 : 1;
    }
    if (strcmp(argv[0], "files") == 0)
        return cmd_files(argv[1]);
    if (strcmp(argv[0], "analyse") == 0 || strcmp(argv[0], "analyze") == 0)
        return cmd_analyse(argv[1]);
    if (strcmp(argv[0], "generate") == 0)
        return cmd_generate(argv[1]);
    if (strcmp(argv[0], "boot") == 0 && argc >= 2)
    {
        int frames = 300, every = 0;
        const char *shot = NULL;
        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "--screenshot") == 0) shot = "";
            else if (strncmp(argv[i], "--screenshot-every=", 19) == 0) { shot = ""; every = atoi(argv[i] + 19); }
            else frames = atoi(argv[i]);
        }
        return cmd_boot(argv[1], frames, shot, every);
    }

    if (strcmp(argv[0], "disasm") == 0 && argc >= 4)
        return cmd_disasm(argv[1], (uint32_t)strtoul(argv[2], NULL, 16), atoi(argv[3]), argc >= 5 ? atoi(argv[4]) : 60);
    if (strcmp(argv[0], "art") == 0 && argc >= 2) {   /* the launcher wallpaper, from a built game */
        char amsg[SBPC_PATH_MAX + 32];
        bool ok = launcher_extract_art(argv[1], amsg, sizeof amsg);
        printf("%s\n", amsg);
        return ok ? 0 : 1;
    }
    if (strcmp(argv[0], "call") == 0 && argc >= 6)   /* developer: call <image> <func> <frames> <in> <out> */
        return cmd_call(argv[1], (uint32_t)strtoul(argv[2], NULL, 16), atoi(argv[3]), argv[4], argv[5]);
    if (strcmp(argv[0], "set-games-folder") == 0) {
        if (argc < 2 || !fs_is_dir(argv[1])) {
            fprintf(stderr, "not a folder: %s\n", argc < 2 ? "(none)" : argv[1]);
            return 2;
        }
        config_set(&cfg, "games_dir", argv[1]);
        if (!config_save(&cfg, launcher_config_path())) {
            fprintf(stderr, "could not write %s\n", launcher_config_path());
            return 1;
        }
        printf("games folder set to %s\n", argv[1]);
        return 0;
    }

    if (strcmp(argv[0], "scan") == 0) {
        const char *dir = NULL;
        bool strict = false;
        int supported = 0;
        scan_result r;
        for (int i = 1; i < argc; i++) {
            if (strcmp(argv[i], "--strict") == 0) strict = true;
            else dir = argv[i];
        }
        if (!dir)
            dir = config_get(&cfg, "games_dir", paths_games_dir());
        if (!dir) {
            fprintf(stderr, "no games folder: pass one or run 'set-games-folder' first\n");
            return 2;
        }
        if (!scan_folder(dir, &r, NULL, NULL)) {
            fprintf(stderr, "cannot read folder %s\n", dir);
            return 1;
        }
        if (r.count == 0)
            printf("no disc images found (.cue, .chd) in %s\n", dir);
        scan_add_installed(&r);   /* built games play from their own copy, with or without the dump */
        for (int i = 0; i < r.count; i++) {
            if (r.items[i].installed) printf("(installed: built, the original dump isn't needed)\n");
            print_identity(r.items[i].cue_path, &r.items[i].id);
            supported += r.items[i].id.status == ID_SUPPORTED;
        }
        printf("\n%d disc(s), %d supported\n", r.count, supported);
        scan_free(&r);
        return strict && supported == 0 ? 1 : 0;
    }

    usage();
    return 2;
}
