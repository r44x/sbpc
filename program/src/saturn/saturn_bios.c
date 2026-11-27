/* BIOS replacement. Instead of running a BIOS image, the vector table and the BIOS function
 * slots in high work RAM point at "trap" addresses in the BIOS ROM range. When a CPU reaches
 * one, bios_hle() does the work in C and returns to the caller.
 *
 * Slots implemented (documented Saturn system calls):
 *   0x300 SETUINT  0x304 GETUINT  0x310 SETSINT  0x314 GETSINT  0x320 CHGSYSCK
 *   0x330 TASSEM   0x334 CLRSEM   0x340 SETSCUIM 0x344 CHGSCUIM(and, or)
 * Variables: 0x324 system clock (0 = 26 MHz), 0x348 SCU interrupt mask.
 * Anything else is logged on first use and returns 0, so the trace shows what's missing. */
#include "core/log.h"
#include "saturn/saturn.h"
#include "sh2/sh2_interp.h"
#include "disc/iso9660.h"
#include "saturn/saturn_bup.h"

#include <stdlib.h>
#include <string.h>

#define VTABLE 0x06000000u

static const char *slot_name(uint32_t off)
{
    switch (off) {
    case 0x300: return "SETUINT";
    case 0x304: return "GETUINT";
    case 0x310: return "SETSINT";
    case 0x314: return "GETSINT";
    case 0x320: return "CHGSYSCK";
    case 0x330: return "TASSEM";
    case 0x334: return "CLRSEM";
    case 0x340: return "SETSCUIM";
    case 0x344: return "CHGSCUIM";
    default: return "?";
    }
}

static void set_ims(saturn *m, uint32_t v)
{
    sh2_state *s = &m->cpu[0];
    m->scu.ims = v & 0xBFFF;
    sh2_wr32(s, VTABLE + 0x348, m->scu.ims);
}

void bios_install(saturn *m)
{
    sh2_state *s = &m->cpu[0];
    for (uint32_t v = 0; v < 0x80; v++)
        sh2_wr32(s, VTABLE + v * 4, BIOS_TRAP_EXC + v * 4);
    for (uint32_t off = 0x200; off < 0x400; off += 4)
        sh2_wr32(s, VTABLE + off, BIOS_TRAP_SLOT + off);
    sh2_wr32(s, VTABLE + 0x324, 0);  /* system clock: 26 MHz (320 mode) */
    set_ims(m, 0xBFFF);            /* BIOS leaves every source masked; the game unmasks what it wants */
}

static void rte(sh2_state *s)
{
    s->pc = sh2_rd32(s, s->r[15]);
    s->r[15] += 4;
    s->sr = sh2_rd32(s, s->r[15]) & SR_MASK;
    s->r[15] += 4;
}

static void slot_call(saturn *m, sh2_state *s, uint32_t off)
{
    uint32_t a = s->r[4], b = s->r[5];
    if (off < 0x400)
        m->bios.calls[off / 4]++;
    s->r[0] = 0;
    switch (off) {
    case 0x300: /* SETUINT(vector, handler): handler is a normal C function */
        if (a < 128) {
            m->bios.uint_handler[a] = b;
            sh2_wr32(s, VTABLE + a * 4, b ? BIOS_TRAP_UINT + a * 4 : BIOS_TRAP_EXC + a * 4);
        }
        break;
    case 0x304:
        s->r[0] = a < 128 ? m->bios.uint_handler[a] : 0;
        break;
    case 0x310: /* SETSINT(vector, handler): handler ends with rte */
        LOGI(SUB_BIOS, "SETSINT vector %02X handler %08X (%s, pc %08X)", (unsigned)a, (unsigned)b, s->id ? "slave" : "master", (unsigned)s->pc);
        /* handler 0 restores the BIOS's own handler, which calls the function given to SETUINT (if any) */
        if (a < 256)
            sh2_wr32(s, VTABLE + a * 4, b ? b : (a < 128 && m->bios.uint_handler[a]) ? BIOS_TRAP_UINT + a * 4
                                                                                   : BIOS_TRAP_EXC + a * 4);
        break;
    case 0x314:
        s->r[0] = a < 256 ? sh2_rd32(s, VTABLE + a * 4) : 0;
        break;
    case 0x320:
        sh2_wr32(s, VTABLE + 0x324, a);
        saturn_set_clock(m, a != 0);
        break;
    case 0x330: /* TASSEM(n): non-zero = acquired */
        s->r[0] = m->bios.sem[a & 0xFF] ? 0 : 1;
        m->bios.sem[a & 0xFF] = 1;
        {   static int n;
            if (n++ < 60) LOGD(SUB_BIOS, "TASSEM(%u) -> %u by %s from %08X", (unsigned)a, (unsigned)s->r[0], s->id ? "slave" : "master", (unsigned)s->pr); }
        break;
    case 0x334:
        m->bios.sem[a & 0xFF] = 0;
        {   static int n;
            if (n++ < 60) LOGD(SUB_BIOS, "CLRSEM(%u) by %s from %08X", (unsigned)a, s->id ? "slave" : "master", (unsigned)s->pr); }
        break;
    case 0x340: /* SETSCUIM(mask) */
        set_ims(m, a);
        break;
    case 0x358: { /* BUP_Init(lib_addr, work_addr, config[3]): builds the backup library's function table */
        uint32_t lib = a, cfg = s->r[6];
        for (int i = 0; i < 16; i++)
            sh2_wr32(s, lib + (uint32_t)i * 4, BIOS_TRAP_SLOT + 0x800 + (uint32_t)i * 4);
        sh2_wr32(s, VTABLE + 0x354, lib);
        if (cfg) {   /* {unit id, partitions} per device: internal memory only, no cartridge or floppy */
            sh2_wr16(s, cfg + 0, 1); sh2_wr16(s, cfg + 2, 1);
            for (uint32_t i = 4; i < 12; i += 2) sh2_wr16(s, cfg + i, 0);
        }
        LOGI(SUB_BIOS, "BUP_Init: library table at %08X (internal memory, %d of %d blocks free)", (unsigned)lib,
             bup_free_blocks(m->backup), BUP_BLOCKS);
        break;
    }
    case 0x344: /* CHGSCUIM(and, or) - the game calls it with two arguments */
        set_ims(m, (m->scu.ims & a) | b);
        break;
    default:
        if (off < 0x400 && m->bios.calls[off / 4] == 1)
            LOGW(SUB_BIOS, "unknown BIOS call 06000%03X (r4=%08X r5=%08X r6=%08X r7=%08X) from %08X",
                 (unsigned)off, (unsigned)a, (unsigned)b, (unsigned)s->r[6], (unsigned)s->r[7], (unsigned)s->pr);
        break;
    }
    if (off < 0x400 && m->bios.calls[off / 4] == 1 && slot_name(off)[0] != '?')
        LOGI(SUB_BIOS, "%s(%08X, %08X) first called from %08X", slot_name(off), (unsigned)a, (unsigned)b,
             (unsigned)s->pr);
    s->pc = s->pr;
}


/* ---- booting an IP that the game itself put at 0x06002000 ---------------------------------------------
 * Some discs boot a loader first (here: the NetLink browser /0, then XS028000.EXE and FLD_KNL.BIN), which
 * loads the real game's IP to 0x06002000, tells the BIOS where to find the game (0x06000CCC = 1,
 * 0x06000CCE = file ID in the root directory, counting "." and ".." as 0 and 1) and jumps into the IP's
 * security code at 0x06002100. On a real console the security code then calls the BIOS ROM, which loads
 * the 1st read file and runs the AIP. We do that part directly. */
static const iso_file *nth_in_dir(const iso_fs *fs, const char *dir, int order)
{
    size_t n = strlen(dir);
    for (int i = 0; i < fs->count; i++) {
        const char *p = fs->files[i].path;
        const char *sl = strrchr(p, '/');
        if ((size_t)(sl - p) == n && strncmp(p, dir, n) == 0 && fs->files[i].order == order)
            return &fs->files[i];
    }
    return NULL;
}

static bool ip_boot(saturn *m, sh2_state *s)
{
    char sig[16];
    uint32_t first_addr, areas = 0, first_size;
    const iso_file *f;
    uint8_t *buf;
    for (int i = 0; i < 15; i++)
        sig[i] = (char)sh2_rd8(s, 0x06002000u + (uint32_t)i);
    sig[15] = 0;
    if (strcmp(sig, "SEGA SEGASATURN") != 0 || !m->fs)
        return false;
    first_addr = sh2_rd32(s, 0x060020F0u);
    for (uint32_t i = 0; i < 16; i++) {
        uint8_t c = sh2_rd8(s, 0x06002040u + i);
        areas += c != ' ' && c != 0;
    }
    /* which file: the BIOS boot variables, else the first file of the root directory */
    f = NULL;
    if (sh2_rd8(s, 0x06000CCCu) == 1) {
        int id = (int)sh2_rd16(s, 0x06000CCEu);
        f = id >= 2 ? nth_in_dir(m->fs, "", id - 2) : NULL;
        if (f && f->is_dir) {
            const iso_file *dir = f;
            f = NULL;
            for (int k = 0; k < 4096 && !f; k++) {
                const iso_file *e = nth_in_dir(m->fs, dir->path, k);
                if (!e) break;
                if (!e->is_dir) f = e;
            }
        }
    } else {
        f = iso_first_root_file(m->fs);
    }
    if (!f) {
        LOGE(SUB_BIOS, "IP boot: can't find the 1st read file");
        return false;
    }
    first_size = sh2_rd32(s, 0x060020F4u);
    if (!first_size || first_size > f->size) first_size = f->size;
    buf = malloc(first_size);
    if (!buf || !iso_read(m->fs, f, 0, buf, first_size)) {
        free(buf);
        LOGE(SUB_BIOS, "IP boot: can't read %s", f->path);
        return false;
    }
    for (uint32_t i = 0; i < first_size; i++)
        sh2_wr8(s, first_addr + i, buf[i]);
    free(buf);
    s->pc = 0x06002E00u + 0x20u * areas;
    rt_bump_epoch();
    LOGI(SUB_BIOS, "IP boot: %s (%u bytes) at %08X, AIP at %08X", f->path, (unsigned)first_size,
         (unsigned)first_addr, (unsigned)s->pc);
    return true;
}

/* ---- backup library (table built by BUP_Init) -----------------------------------------------------
 * Only device 0, the internal 32 KB memory, exists. Guest structures:
 *   BupDir  (0x24 bytes): name[12], comment[11], language, date BE32, size BE32, blocks BE16
 *   BupStat: total size, total blocks, block size, free size, free blocks, how many files of the
 *            asked size still fit (all BE32)
 *   BupDate: year - 1980, month, day, hour, minute, weekday (bytes) */
static void guest_name(sh2_state *s, uint32_t p, char *out, int max)
{
    int i = 0;
    for (; i < max && p; i++) {
        char c = (char)sh2_rd8(s, p + (uint32_t)i);
        if (!c) break;
        out[i] = c;
    }
    out[i] = 0;
}

static void put_dir(sh2_state *s, uint32_t p, const bup_dir *d)
{
    for (uint32_t i = 0; i < 12; i++) sh2_wr8(s, p + i, i < 11 ? (uint8_t)d->name[i] : 0);
    for (uint32_t i = 0; i < 11; i++) sh2_wr8(s, p + 12 + i, i < 10 ? (uint8_t)d->comment[i] : 0);
    sh2_wr8(s, p + 23, d->language);
    sh2_wr32(s, p + 24, d->date);
    sh2_wr32(s, p + 28, d->size);
    sh2_wr16(s, p + 32, d->blocks);
}

static void bup_changed(saturn *m)
{
    m->backup_dirty = true;
    m->backup_quiet = 0;
}

static void bup_call(saturn *m, sh2_state *s, int fn)
{
    static uint8_t data[BUP_SIZE];
    static bup_dir list[64];
    static int logged[16];
    uint32_t dev = s->r[4];
    char name[12];
    int32_t r = BUP_OK;

    if (fn >= 1 && fn <= 8 && dev != 0) {
        r = BUP_NON;
    } else switch (fn) {
    case 1: /* SelPart(device, partition) */
        break;
    case 2: /* Format(device) */
        bup_format(m->backup);
        bup_changed(m);
        LOGI(SUB_BIOS, "BUP_Format");
        break;
    case 3: { /* Stat(device, datasize, stat) */
        uint32_t p = s->r[6], need = (uint32_t)bup_blocks_for(s->r[5]), freeb = (uint32_t)bup_free_blocks(m->backup);
        sh2_wr32(s, p + 0, BUP_SIZE);
        sh2_wr32(s, p + 4, BUP_BLOCKS - 2);
        sh2_wr32(s, p + 8, BUP_BLOCK);
        sh2_wr32(s, p + 12, freeb * BUP_BLOCK);
        sh2_wr32(s, p + 16, freeb);
        sh2_wr32(s, p + 20, s->r[5] ? freeb / need : 0);
        break;
    }
    case 4: { /* Write(device, dir, data, keep_existing) */
        uint32_t dp = s->r[5], src = s->r[6];
        bup_dir d;
        memset(&d, 0, sizeof d);
        guest_name(s, dp, d.name, 11);
        guest_name(s, dp + 12, d.comment, 10);
        d.language = (uint8_t)sh2_rd8(s, dp + 23);
        d.date = sh2_rd32(s, dp + 24);
        d.size = sh2_rd32(s, dp + 28);
        if (d.size > BUP_SIZE) {
            r = BUP_NOT_ENOUGH_MEMORY;
        } else {
            for (uint32_t i = 0; i < d.size; i++) data[i] = (uint8_t)sh2_rd8(s, src + i);
            r = bup_write(m->backup, &d, data, (s->r[7] & 0xFF) != 0);
            if (r == BUP_OK) bup_changed(m);
        }
        LOGI(SUB_BIOS, "BUP_Write \"%s\" (%u bytes) -> %d", d.name, (unsigned)d.size, (int)r);
        break;
    }
    case 5: { /* Read(device, name, data) */
        uint32_t size = 0, dst = s->r[6];
        guest_name(s, s->r[5], name, 11);
        r = bup_read(m->backup, name, data, sizeof data, &size);
        if (r == BUP_OK)
            for (uint32_t i = 0; i < size; i++) sh2_wr8(s, dst + i, data[i]);
        LOGI(SUB_BIOS, "BUP_Read \"%s\" -> %d (%u bytes)", name, (int)r, (unsigned)size);
        break;
    }
    case 6: /* Delete(device, name) */
        guest_name(s, s->r[5], name, 11);
        r = bup_delete(m->backup, name);
        if (r == BUP_OK) bup_changed(m);
        LOGI(SUB_BIOS, "BUP_Delete \"%s\" -> %d", name, (int)r);
        break;
    case 7: { /* Dir(device, name prefix, table size, table): number found, negative if the table is too small.
               * The USA kernel calls it with size 0 to ask whether its save exists (0x06039394). */
        uint32_t max = s->r[6] & 0xFFFF, tb = s->r[7];
        int n;
        guest_name(s, s->r[5], name, 11);
        n = bup_list(m->backup, name, list, 64);
        for (int i = 0; i < n && i < 64 && (uint32_t)i < max && tb; i++)
            put_dir(s, tb + (uint32_t)i * 0x24, &list[i]);
        r = (max && (uint32_t)n > max) ? -n : n;
        break;
    }
    case 8: { /* Verify(device, name, data) */
        uint32_t size = 0, src = s->r[6];
        guest_name(s, s->r[5], name, 11);
        r = bup_read(m->backup, name, data, sizeof data, &size);
        for (uint32_t i = 0; r == BUP_OK && i < size; i++)
            if (data[i] != (uint8_t)sh2_rd8(s, src + i)) r = BUP_NO_MATCH;
        break;
    }
    case 9: { /* GetDate(date, out) */
        int y, mo, d, h, mi, wd;
        uint32_t p = s->r[5];
        bup_date_decode(s->r[4], &y, &mo, &d, &h, &mi, &wd);
        sh2_wr8(s, p + 0, (uint8_t)(y - 1980)); sh2_wr8(s, p + 1, (uint8_t)mo); sh2_wr8(s, p + 2, (uint8_t)d);
        sh2_wr8(s, p + 3, (uint8_t)h); sh2_wr8(s, p + 4, (uint8_t)mi); sh2_wr8(s, p + 5, (uint8_t)wd);
        break;
    }
    case 10: { /* SetDate(in): returns the packed date */
        uint32_t p = s->r[4];
        r = (int32_t)bup_date_encode(1980 + sh2_rd8(s, p), sh2_rd8(s, p + 1), sh2_rd8(s, p + 2), sh2_rd8(s, p + 3),
                                     sh2_rd8(s, p + 4));
        break;
    }
    default:
        if (!logged[fn & 15]++)
            LOGW(SUB_BIOS, "unknown backup library function %d (r4=%08X r5=%08X) from %08X", fn, (unsigned)s->r[4],
                 (unsigned)s->r[5], (unsigned)s->pr);
        break;
    }
    s->r[0] = (uint32_t)r;
}

bool bios_hle(saturn *m, sh2_state *s)
{
    uint32_t pc = s->pc & 0x1FFFFFFFu;
    int id = s->id;
    s->cycles -= 20;
    s->total_cycles += 20;

    if (pc == 0x06002100u)
        return id == 0 && ip_boot(m, s);

    if (pc >= BIOS_TRAP_SLOT && pc < BIOS_TRAP_SLOT + 0x1000) {
        uint32_t off = pc - BIOS_TRAP_SLOT;
        if (off == 0x3F0 && id == 1) { /* slave parked after SSHON */
            s->sleeping = 1;
            LOGI(SUB_BIOS, "slave SH-2 waiting (no slave entry protocol yet)");
            return true;
        }
        if (off == 0 && id == 0) {     /* the game returned to the BIOS */
            LOGW(SUB_BIOS, "game returned to the BIOS");
            s->sleeping = 1;
            return true;
        }
        if (off >= 0x800 && off < 0x840) {      /* backup library function */
            bup_call(m, s, (int)(off - 0x800) / 4);
            s->pc = s->pr;
            return true;
        }
        slot_call(m, s, off);
        return true;
    }
    if (pc >= BIOS_TRAP_UINT && pc < BIOS_TRAP_UINT + 0x200) {
        uint32_t vec = (pc - BIOS_TRAP_UINT) / 4;
        uint_frame *fr;
        if (m->bios.depth[id] >= 16) {
            LOGE(SUB_BIOS, "user interrupt nesting too deep");
            rte(s);
            return true;
        }
        fr = &m->bios.frames[id][m->bios.depth[id]++];
        for (int i = 0; i < 8; i++) fr->regs[i] = s->r[i];
        fr->mach = s->mach;
        fr->macl = s->macl;
        fr->pr = s->pr;
        s->pr = BIOS_TRAP_URET + vec * 4;
        s->pc = m->bios.uint_handler[vec];
        return true;
    }
    if (pc >= BIOS_TRAP_URET && pc < BIOS_TRAP_URET + 0x200) {
        uint_frame *fr;
        if (m->bios.depth[id] <= 0) {
            LOGE(SUB_BIOS, "user interrupt return without entry");
            s->sleeping = 1;
            return true;
        }
        fr = &m->bios.frames[id][--m->bios.depth[id]];
        for (int i = 0; i < 8; i++) s->r[i] = fr->regs[i];
        s->mach = fr->mach;
        s->macl = fr->macl;
        s->pr = fr->pr;
        rte(s);
        return true;
    }
    if (pc >= BIOS_TRAP_EXC && pc < BIOS_TRAP_EXC + 0x400) {
        uint32_t vec = (pc - BIOS_TRAP_EXC) / 4;
        if (m->bios.unhandled[vec]++ == 0)
            LOGW(SUB_BIOS, "%s vector 0x%02X with no handler (%s)", vec < 0x40 ? "exception" : "interrupt",
                 (unsigned)vec, id ? "slave" : "master");
        if (vec < 0x40) {              /* CPU exception: stop this CPU so the report shows it */
            LOGE(SUB_BIOS, "CPU exception %u, stopping the %s (return pc %08X)", (unsigned)vec,
                 id ? "slave" : "master", (unsigned)sh2_rd32(s, s->r[15]));
            s->sleeping = 1;
            return true;
        }
        rte(s);
        return true;
    }
    if (m->bios.rom_calls++ == 0)
        LOGW(SUB_BIOS, "call into BIOS ROM at %08X from %08X: returning 0", (unsigned)pc, (unsigned)s->pr);
    s->r[0] = 0;
    s->pc = s->pr;
    return true;
}
