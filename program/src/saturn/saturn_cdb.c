/* CD block: command interface, sector buffer with filters and selectors, ISO file-system commands
 * and host data transfer. Data tracks only (audio playback isn't implemented). Commands act at
 * once; "play" delivers sectors a few per frame (cdb_tick). */
#include "core/log.h"
#include "saturn/saturn.h"

#include <string.h>

enum {
    HIRQ_CMOK = 0x0001, HIRQ_DRDY = 0x0002, HIRQ_CSCT = 0x0004, HIRQ_BFUL = 0x0008,
    HIRQ_PEND = 0x0010, HIRQ_DCHG = 0x0020, HIRQ_ESEL = 0x0040, HIRQ_EHST = 0x0080,
    HIRQ_ECPY = 0x0100, HIRQ_EFLS = 0x0200, HIRQ_SCDQ = 0x0400
};

enum { ST_BUSY = 0, ST_PAUSE = 1, ST_STANDBY = 2, ST_PLAY = 3, ST_SEEK = 4, ST_NODISC = 7 };

#define SECTORS_PER_FRAME 6   /* about 6x speed: loading shouldn't make people wait */
#define FAD_OFFSET 150u       /* FAD = sector within the data track + 150 */

static void reset_buffers(cdb_state *c)
{
    memset(c->blk_used, 0, sizeof c->blk_used);
    memset(c->sel_count, 0, sizeof c->sel_count);
    for (int i = 0; i < CDB_SEL; i++) {
        memset(&c->filt[i], 0, sizeof c->filt[i]);
        c->filt[i].true_conn = (uint8_t)i;
        c->filt[i].false_conn = 0xFF;
    }
    c->cd_conn = 0xFF;
    c->last_dest = 0xFF;
}

void cdb_init(saturn *m)
{
    cdb_state *c = &m->cdb;
    /* power-on signature the CD library checks for */
    c->cr[0] = ('C' << 8) | 'D';
    c->cr[1] = ('B' << 8) | 'L';
    c->cr[2] = ('O' << 8) | 'C';
    c->cr[3] = ('K' << 8) | ' ';
    /* As the real BIOS leaves it after booting the disc: DCHG (disc changed) already acknowledged,
     * otherwise the CD library reports the tray as open (GFS error -23). */
    c->hirq = 0x0BE1 & ~HIRQ_DCHG;
    c->hirq_mask = 0xFFFF;
    c->status = ST_PAUSE;
    c->fad = FAD_OFFSET;
    c->get_len = 0;
    c->put_len = 0;
    reset_buffers(c);
}

/* ---- buffer management ----------------------------------------------------------------------------- */

static int free_blocks(const cdb_state *c)
{
    int n = 0;
    for (int i = 0; i < CDB_BLOCKS; i++) n += !c->blk_used[i];
    return n;
}

static void sel_remove(cdb_state *c, int sel, int start, int count)
{
    cdb_state *cc = c;
    int n = cc->sel_count[sel];
    if (start >= n) return;
    if (count < 0 || start + count > n) count = n - start;
    for (int i = start; i < start + count; i++)
        cc->blk_used[cc->sel_blk[sel][i]] = false;
    memmove(&cc->sel_blk[sel][start], &cc->sel_blk[sel][start + count], (size_t)(n - start - count) * sizeof(int16_t));
    cc->sel_count[sel] = n - count;
}

static bool filter_match(const cdb_filter *f, uint32_t fad)
{
    bool ok = true;
    if (f->mode & 0x40)                                 /* FAD range */
        ok = fad >= f->range_start && fad < f->range_start + f->range_count;
    /* Mode 1 data has no subheader: file/channel/submode/coding conditions see zeros. */
    if (ok && (f->mode & 0x01)) ok = (f->fid == 0);
    if (ok && (f->mode & 0x02)) ok = (f->chan == 0);
    if (ok && (f->mode & 0x04)) ok = ((0 & f->smmask) == (f->smval & f->smmask));
    if (ok && (f->mode & 0x08)) ok = ((0 & f->cimask) == (f->cival & f->cimask));
    return (f->mode & 0x10) ? !ok : ok;                  /* bit 4 inverts the subheader conditions */
}

/* Stores a sector in the partition reached from filter `fn` (following false outputs). */
static bool route_sector(cdb_state *c, int blk, uint32_t fad, int fn)
{
    for (int guard = 0; guard < CDB_SEL && fn >= 0 && fn < CDB_SEL; guard++) {
        const cdb_filter *f = &c->filt[fn];
        if (filter_match(f, fad)) {
            int sel = f->true_conn;
            if (sel >= CDB_SEL) return false;
            c->sel_blk[sel][c->sel_count[sel]++] = (int16_t)blk;
            c->blk_used[blk] = true;
            c->blk_fad[blk] = fad;
            c->last_dest = (uint8_t)sel;
            return true;
        }
        fn = f->false_conn == 0xFF ? -1 : f->false_conn;
    }
    return false;
}

/* ---- play ------------------------------------------------------------------------------------------- */

/* ---- disc layout: FAD of every track's INDEX 01 and its length, the same layout as the TOC ---------- */
static uint32_t track_fad(saturn *m, int idx, uint32_t *len)
{
    const cue_sheet *cs = &m->fs->d->cue;
    uint32_t fad = FAD_OFFSET;
    for (int i = 0; i < cs->track_count; i++) {
        const cue_track *t = &cs->tracks[i];
        if (i > 0 && t->index0 >= 0 && t->index1 > t->index0) fad += (uint32_t)(t->index1 - t->index0);
        if (i == idx) { if (len) *len = (uint32_t)t->sectors; return fad; }
        fad += (uint32_t)t->sectors;
    }
    if (len) *len = 0;
    return fad;   /* idx == track_count: the lead-out */
}

/* Track index (0-based) holding `fad`, and the sector relative to its INDEX 01. A FAD in a pregap
 * counts as past the end of the previous track (it reads as silence / zeros). */
static int track_at(saturn *m, uint32_t fad, int64_t *rel)
{
    for (int i = m->fs->d->cue.track_count - 1; i > 0; i--) {
        uint32_t start = track_fad(m, i, NULL);
        if (fad >= start) { *rel = (int64_t)fad - start; return i; }
    }
    *rel = (int64_t)fad - FAD_OFFSET;
    return 0;
}

static bool play_finished(cdb_state *c)
{
    if (c->repeat_left != 0) {                    /* loop the range (CD audio music does this) */
        if (c->repeat_left > 0) c->repeat_left--;
        c->fad = c->play_start;
        return false;
    }
    c->playing = false;
    c->status = ST_PAUSE;
    c->hirq |= HIRQ_PEND;
    return true;
}

void cdb_tick(saturn *m)
{
    cdb_state *c = &m->cdb;
    int64_t rel;
    int t;
    if (!c->playing || !m->fs)
        return;
    t = track_at(m, c->fad, &rel);
    c->cur_track = t + 1;
    c->cur_audio = !track_is_data(&m->fs->d->cue.tracks[t]);
    if (c->cur_audio) {
        /* CD audio: 75 sectors a second at 1x, each 588 stereo samples, straight to the SCSP */
        int fps = saturn_fps(m);
        c->audio_acc += 75;
        while (c->audio_acc >= fps) {
            uint8_t raw[2352];
            c->audio_acc -= fps;
            if (c->fad >= c->play_end && play_finished(c))
                return;
            t = track_at(m, c->fad, &rel);
            c->cur_track = t + 1;
            c->cur_audio = !track_is_data(&m->fs->d->cue.tracks[t]);
            if (!c->cur_audio) break;
            disc_read_audio(m->fs->d, t, rel, raw);
            for (int i = 0; i < 588; i++) {
                uint32_t w = c->cdda_w % CDB_CDDA_FRAMES;
                c->cdda[w * 2] = (int16_t)(raw[i * 4] | raw[i * 4 + 1] << 8);
                c->cdda[w * 2 + 1] = (int16_t)(raw[i * 4 + 2] | raw[i * 4 + 3] << 8);
                c->cdda_w++;
            }
            if (c->cdda_w - c->cdda_r > CDB_CDDA_FRAMES) c->cdda_r = c->cdda_w - CDB_CDDA_FRAMES;
            c->fad++;
            c->hirq |= HIRQ_SCDQ;
        }
        return;
    }
    c->audio_acc = 0;
    for (int n = 0; n < SECTORS_PER_FRAME; n++) {
        int blk = -1;
        if (c->fad >= c->play_end) {
            if (play_finished(c)) return;
            continue;
        }
        for (int i = 0; i < CDB_BLOCKS; i++)
            if (!c->blk_used[i]) { blk = i; break; }
        if (blk < 0) {                                    /* buffer full: stall */
            c->hirq |= HIRQ_BFUL;
            return;
        }
        t = track_at(m, c->fad, &rel);
        if (!track_is_data(&m->fs->d->cue.tracks[t]) || !disc_read_user(m->fs->d, t, rel, c->pool[blk]))
            memset(c->pool[blk], 0, 2048);
        if (c->cd_conn != 0xFF)
            route_sector(c, blk, c->fad, c->cd_conn);
        c->fad++;
        c->hirq |= HIRQ_CSCT | HIRQ_SCDQ;
    }
}

static void start_play(cdb_state *c, uint32_t start_fad, uint32_t end_fad)
{
    LOGD(SUB_CD, "play FAD %u-%u (LBA %u, %u sectors)", (unsigned)start_fad, (unsigned)end_fad,
         (unsigned)(start_fad - 150), (unsigned)(end_fad - start_fad));
    c->fad = start_fad;
    c->play_end = end_fad;
    c->playing = end_fad > start_fad;
    c->status = c->playing ? ST_PLAY : ST_PAUSE;
    if (!c->playing)
        c->hirq |= HIRQ_PEND;
}

/* ---- file system -------------------------------------------------------------------------------------- */

static bool parent_of(const char *path, char *out)
{
    const char *sl = strrchr(path, '/');
    size_t n = sl ? (size_t)(sl - path) : 0;
    memcpy(out, path, n);
    out[n] = 0;
    return true;
}

static void load_dir(saturn *m)
{
    cdb_state *c = &m->cdb;
    char par[ISO_NAME_MAX];
    c->dir_count = 0;
    if (!m->fs) return;
    for (int i = 0; i < m->fs->count && c->dir_count < CDB_DIR_MAX; i++) {
        parent_of(m->fs->files[i].path, par);
        if (strcmp(par, c->cur_dir) == 0)
            c->dir_entries[c->dir_count++] = i;
    }
}

static void change_dir(saturn *m, uint32_t id)
{
    cdb_state *c = &m->cdb;
    if (id == 0xFFFFFF) {
        c->cur_dir[0] = 0;
    } else if (id == 1) {
        char par[ISO_NAME_MAX];
        parent_of(c->cur_dir, par);
        memcpy(c->cur_dir, par, sizeof par);
    } else if (id >= 2 && (int)(id - 2) < c->dir_count) {
        const iso_file *f = &m->fs->files[c->dir_entries[id - 2]];
        if (f->is_dir) {
            memcpy(c->cur_dir, f->path, sizeof c->cur_dir);
            size_t n = strlen(c->cur_dir);
            if (n && c->cur_dir[n - 1] == '/') c->cur_dir[n - 1] = 0;
        }
    }
    load_dir(m);
}

static const iso_file *dir_file(saturn *m, uint32_t id)
{
    cdb_state *c = &m->cdb;
    if (id < 2 || (int)(id - 2) >= c->dir_count || !m->fs) return NULL;
    return &m->fs->files[c->dir_entries[id - 2]];
}

static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }

static void file_info(saturn *m, uint32_t id, uint8_t *out)
{
    const iso_file *f = dir_file(m, id);
    memset(out, 0, 12);
    if (!f) return;
    put32(out, f->lba + FAD_OFFSET);
    put32(out + 4, f->size);
    out[10] = 0;                         /* XA file number: 0, this is a plain Mode 1 disc */
    out[11] = f->is_dir ? 2 : 0;
}

/* ---- TOC -------------------------------------------------------------------------------------------------- */

static void build_toc(saturn *m, uint8_t *out)
{
    const cue_sheet *cs = m->fs ? &m->fs->d->cue : NULL;
    uint32_t fad = FAD_OFFSET, leadout = FAD_OFFSET;
    int first = 1, last = 1;
    memset(out, 0xFF, 99 * 4);
    if (cs) {
        last = cs->track_count;
        for (int i = 0; i < cs->track_count; i++) {
            const cue_track *t = &cs->tracks[i];
            uint8_t ctl = track_is_data(t) ? 0x41 : 0x01;
            if (i > 0 && t->index0 >= 0 && t->index1 > t->index0) fad += (uint32_t)(t->index1 - t->index0);
            put32(out + (size_t)i * 4, ((uint32_t)ctl << 24) | fad);
            fad += (uint32_t)t->sectors;
        }
        leadout = fad;
    }
    put32(out + 99 * 4, (0x41u << 24) | ((uint32_t)first << 16));
    put32(out + 100 * 4, (0x01u << 24) | ((uint32_t)last << 16));
    put32(out + 101 * 4, (0x01u << 24) | leadout);
}

/* ---- host transfer -------------------------------------------------------------------------------------- */

static void end_xfer(cdb_state *c);

static void begin_xfer(cdb_state *c, uint32_t len)
{
    if (c->xactive)
        end_xfer(c);   /* a new transfer implicitly closes the previous one */
    c->xlen = len;
    c->xpos = 0;
    c->xactive = true;
    c->xdel_sel = -1;
    c->hirq |= HIRQ_DRDY;
}

/* Copies `count` sectors from partition `sel` (starting at `start`) into the transfer buffer. */
static uint32_t gather_sectors(cdb_state *c, int sel, int start, int count)
{
    int n = c->sel_count[sel];
    uint32_t len = 0;
    if (start >= n) return 0;
    if (count < 0 || start + count > n) count = n - start;
    for (int i = 0; i < count; i++) {
        memcpy(c->xbuf + len, c->pool[c->sel_blk[sel][start + i]], 2048);
        len += 2048;
    }
    return len;
}

static void end_xfer(cdb_state *c)
{
    if (c->xactive && c->xdel_sel >= 0)
        sel_remove(c, c->xdel_sel, c->xdel_start, c->xdel_count);
    c->xdel_sel = -1;
    c->xactive = false;
    c->hirq |= HIRQ_EHST;
}

/* ---- commands ----------------------------------------------------------------------------------------------- */

static void status_report(cdb_state *c)
{
    int t = c->cur_track > 0 ? c->cur_track : 1;
    c->cr[0] = (uint16_t)(c->status << 8);
    c->cr[1] = (uint16_t)((c->cur_audio ? 0x0100 : 0x4100) | (t & 0xFF));   /* CTRL/ADR, track */
    c->cr[2] = (uint16_t)(0x0100 | ((c->fad >> 16) & 0xFF));
    c->cr[3] = (uint16_t)c->fad;
}

static uint32_t pos24(uint16_t lo8, uint16_t w) { return ((uint32_t)(lo8 & 0xFF) << 16) | w; }

static void execute(saturn *m)
{
    cdb_state *c = &m->cdb;
    uint16_t r0 = c->cr[0], r1 = c->cr[1], r2 = c->cr[2], r3 = c->cr[3];
    uint8_t cmd = (uint8_t)(r0 >> 8);
    int sel = r2 >> 8;
    c->command_count[cmd]++;
    for (int i = 0; i < 4; i++)
        c->last_cmd[i] = c->cr[i];
    if (c->command_count[cmd] == 1)
        LOGI(SUB_CD, "command %02X  CR %04X %04X %04X %04X (first use, pc %08X)", cmd, r0, r1, r2, r3,
             (unsigned)m->cpu[0].pc);
    LOGD(SUB_CD, "cmd %02X  %04X %04X %04X %04X", cmd, r0, r1, r2, r3);

    status_report(c);
    c->hirq |= HIRQ_CMOK;

    switch (cmd) {
    case 0x00: break;                                           /* get status */
    case 0x01:                                                  /* get hardware info */
        c->cr[1] = 0x0201; c->cr[2] = 0; c->cr[3] = 0x0400;
        break;
    case 0x02:                                                  /* get TOC */
        build_toc(m, c->xbuf);
        begin_xfer(c, 408);
        c->cr[1] = 204; c->cr[2] = 0; c->cr[3] = 0;
        break;
    case 0x03: {                                                /* get session info */
        uint8_t toc[408];
        uint32_t leadout, fad;
        int tracks_or_sessions;
        build_toc(m, toc);
        leadout = (uint32_t)(toc[101 * 4 + 1] << 16 | toc[101 * 4 + 2] << 8 | toc[101 * 4 + 3]);
        if ((r0 & 0xFF) == 0) { fad = leadout; tracks_or_sessions = 1; }       /* whole disc: sessions, lead-out */
        else { fad = 0; tracks_or_sessions = 1; }      /* session 1: its start as an LBA (the library adds 150) */
        c->cr[1] = 0;
        c->cr[2] = (uint16_t)((tracks_or_sessions << 8) | ((fad >> 16) & 0xFF));
        c->cr[3] = (uint16_t)fad;
        break;
    }
    case 0x04:                                                  /* initialise CD system */
        c->playing = false;
        c->status = ST_PAUSE;
        c->fad = FAD_OFFSET;
        if (!(r0 & 0x80)) reset_buffers(c);
        c->hirq |= HIRQ_ESEL | HIRQ_EHST | HIRQ_ECPY | HIRQ_EFLS | HIRQ_SCDQ;
        status_report(c);
        break;
    case 0x06:                                                  /* end data transfer */
        {
            uint32_t words = c->xactive ? c->xpos / 2 : 0xFFFFFF;
            end_xfer(c);
            c->cr[0] = (uint16_t)((c->status << 8) | ((words >> 16) & 0xFF));
            c->cr[1] = (uint16_t)words; c->cr[2] = 0; c->cr[3] = 0;
        }
        break;
    case 0x10: {                                                /* play disc */
        uint32_t sp = pos24(r0, r1), ep = pos24(r2, r3), start, end;
        int ntracks = m->fs ? m->fs->d->cue.track_count : 1;
        int mode = r2 >> 8;                                     /* bits 3-0: repeats (15 = forever) */
        if (sp == 0xFFFFFF) {                                   /* resume from where we are */
            start = c->fad;
        } else if (sp & 0x800000) {
            start = sp & 0x7FFFFF;
        } else {                                                /* track mode: track << 8 | index */
            int t = (int)((sp >> 8) & 0xFF);
            if (t < 1) t = 1;
            if (t > ntracks) t = ntracks;
            start = m->fs ? track_fad(m, t - 1, NULL) : FAD_OFFSET;
        }
        if (ep == 0xFFFFFF) {
            end = c->play_end > start ? c->play_end : start;
        } else if (ep & 0x800000) {
            end = start + (ep & 0x7FFFFF);
        } else {
            int t = (int)((ep >> 8) & 0xFF);
            uint32_t len;
            if (t < 1 || t > ntracks) t = ntracks;
            end = m->fs ? track_fad(m, t - 1, &len) + len : start;
        }
        if (mode != 0xFF && !(mode & 0x80)) {
            c->repeat_left = (mode & 15) == 15 ? -1 : (mode & 15);
            c->play_start = start;
        }
        if (sp == 0xFFFFFF && ep == 0xFFFFFF) { c->playing = c->play_end > c->fad; c->status = c->playing ? ST_PLAY : ST_PAUSE; }
        else start_play(c, start, end);
        status_report(c);
        break;
    }
    case 0x11: {                                                /* seek disc */
        uint32_t sp = pos24(r0, r1);
        c->playing = false;
        c->status = ST_PAUSE;
        /* 0xFFFFFF = pause where we are: the game's own pause does this, then resumes with play
           0xFFFFFF/0xFFFFFF. It must be checked before the FAD bit (bit 23), which it also has set. */
        if (sp == 0xFFFFFF) { /* keep the position */ }
        else if (sp & 0x800000) c->fad = sp & 0x7FFFFF;
        else c->fad = FAD_OFFSET;
        status_report(c);
        break;
    }
    case 0x30: c->cd_conn = (uint8_t)(r2 >> 8); c->hirq |= HIRQ_ESEL; break;
    case 0x31: c->cr[2] = (uint16_t)(c->cd_conn << 8); break;
    case 0x32: c->cr[2] = (uint16_t)(c->last_dest << 8); break;
    case 0x40:                                                  /* set filter range */
        if ((r2 >> 8) < CDB_SEL) {
            cdb_filter *f = &c->filt[r2 >> 8];
            f->range_start = pos24(r0, r1);
            f->range_count = pos24(r2, r3);
        }
        c->hirq |= HIRQ_ESEL;
        break;
    case 0x41:
        if ((r2 >> 8) < CDB_SEL) {
            const cdb_filter *f = &c->filt[r2 >> 8];
            c->cr[0] = (uint16_t)((c->status << 8) | ((f->range_start >> 16) & 0xFF));
            c->cr[1] = (uint16_t)f->range_start;
            c->cr[2] = (uint16_t)((f->range_count >> 16) & 0xFF);
            c->cr[3] = (uint16_t)f->range_count;
        }
        break;
    case 0x42:                                                  /* set filter subheader conditions */
        if ((r2 >> 8) < CDB_SEL) {
            cdb_filter *f = &c->filt[r2 >> 8];
            f->chan = (uint8_t)r0; f->smmask = (uint8_t)(r1 >> 8); f->cimask = (uint8_t)r1;
            f->fid = (uint8_t)r2; f->smval = (uint8_t)(r3 >> 8); f->cival = (uint8_t)r3;
        }
        c->hirq |= HIRQ_ESEL;
        break;
    case 0x43:
        if ((r2 >> 8) < CDB_SEL) {
            const cdb_filter *f = &c->filt[r2 >> 8];
            c->cr[0] = (uint16_t)((c->status << 8) | f->chan);
            c->cr[1] = (uint16_t)(f->smmask << 8 | f->cimask);
            c->cr[2] = f->fid;
            c->cr[3] = (uint16_t)(f->smval << 8 | f->cival);
        }
        break;
    case 0x44:                                                  /* set filter mode */
        if ((r2 >> 8) < CDB_SEL) {
            cdb_filter *f = &c->filt[r2 >> 8];
            if (r0 & 0x80) {
                memset(f, 0, sizeof *f);
                f->true_conn = (uint8_t)(r2 >> 8); f->false_conn = 0xFF;
            }
            f->mode = (uint8_t)(r0 & 0x7F);
        }
        c->hirq |= HIRQ_ESEL;
        break;
    case 0x45:
        if ((r2 >> 8) < CDB_SEL) c->cr[0] = (uint16_t)((c->status << 8) | c->filt[r2 >> 8].mode);
        break;
    case 0x46:                                                  /* set filter connection */
        if ((r2 >> 8) < CDB_SEL) {
            cdb_filter *f = &c->filt[r2 >> 8];
            if (r0 & 1) f->true_conn = (uint8_t)(r1 >> 8);
            if (r0 & 2) f->false_conn = (uint8_t)r1;
        }
        c->hirq |= HIRQ_ESEL;
        break;
    case 0x47:
        if ((r2 >> 8) < CDB_SEL) c->cr[1] = (uint16_t)(c->filt[r2 >> 8].true_conn << 8 | c->filt[r2 >> 8].false_conn);
        break;
    case 0x48:                                                  /* reset selector */
        if ((r0 & 0xFF) == 0) {
            if (sel < CDB_SEL) sel_remove(c, sel, 0, -1);
        } else {
            for (int i = 0; i < CDB_SEL; i++) sel_remove(c, i, 0, -1);
            if (r0 & 0x80) { c->cd_conn = 0xFF; }
        }
        c->hirq |= HIRQ_ESEL;
        break;
    case 0x50:                                                  /* get buffer size */
        c->cr[1] = (uint16_t)free_blocks(c); c->cr[2] = (uint16_t)(CDB_SEL << 8); c->cr[3] = CDB_BLOCKS;
        break;
    case 0x51:                                                  /* get sector number */
        c->cr[3] = sel < CDB_SEL ? (uint16_t)c->sel_count[sel] : 0;
        break;
    case 0x52: case 0x53: {                                     /* calc / get actual size */
        static uint32_t actual;
        if (cmd == 0x52) {
            int start = r1 == 0xFFFF ? c->sel_count[sel < CDB_SEL ? sel : 0] - 1 : r1, count = r3 == 0xFFFF ? -1 : r3;
            int n = sel < CDB_SEL ? c->sel_count[sel] : 0;
            if (start < 0 || start >= n) start = n;
            if (count < 0 || start + count > n) count = n - start;
            actual = (uint32_t)count * 1024;                    /* words */
            c->hirq |= HIRQ_ESEL;
        } else {
            c->cr[0] = (uint16_t)((c->status << 8) | ((actual >> 16) & 0xFF));
            c->cr[1] = (uint16_t)actual;
        }
        break;
    }
    case 0x54:                                                  /* get sector info */
        if (sel < CDB_SEL && r1 < c->sel_count[sel]) {
            uint32_t fad = c->blk_fad[c->sel_blk[sel][r1]];
            c->cr[0] = (uint16_t)((c->status << 8) | ((fad >> 16) & 0xFF));
            c->cr[1] = (uint16_t)fad; c->cr[2] = 0; c->cr[3] = 0;
        }
        break;
    case 0x60:                                                  /* set sector length */
        if ((r0 & 0xFF) != 0xFF) c->get_len = (uint8_t)r0;
        if ((r1 >> 8) != 0xFF) c->put_len = (uint8_t)(r1 >> 8);
        c->hirq |= HIRQ_ESEL;
        break;
    case 0x61: case 0x63: {                                     /* get (and delete) sector data */
        int start = r1 == 0xFFFF ? 0 : r1, count = r3 == 0xFFFF ? -1 : r3;
        uint32_t len = sel < CDB_SEL ? gather_sectors(c, sel, start, count) : 0;
        if (c->get_len != 0) LOGW(SUB_CD, "sector length code %d requested; only 2048-byte data is supplied", c->get_len);
        begin_xfer(c, len);
        if (cmd == 0x63 && sel < CDB_SEL) {
            c->xdel_sel = sel; c->xdel_start = start;
            c->xdel_count = count < 0 ? c->sel_count[sel] - start : count;
        }
        break;
    }
    case 0x62:                                                  /* delete sector data */
        if (sel < CDB_SEL) sel_remove(c, sel, r1 == 0xFFFF ? 0 : r1, r3 == 0xFFFF ? -1 : r3);
        c->hirq |= HIRQ_EHST;
        break;
    case 0x70: case 0x71:                                       /* change / read directory */
        change_dir(m, pos24(r2, r3));
        c->hirq |= HIRQ_EFLS;
        break;
    case 0x72:                                                  /* get file system scope */
        c->cr[1] = (uint16_t)(c->dir_count + 2); c->cr[2] = 0x0100; c->cr[3] = 2;
        break;
    case 0x73: {                                                /* get file info */
        uint32_t id = pos24(r2, r3);
        if (id == 0xFFFFFF) {
            uint32_t n = (uint32_t)c->dir_count + 2;
            for (uint32_t i = 0; i < n; i++) file_info(m, i, c->xbuf + i * 12);
            begin_xfer(c, n * 12);
            c->cr[1] = (uint16_t)(n * 6);
        } else {
            file_info(m, id, c->xbuf);
            begin_xfer(c, 12);
            c->cr[1] = 6;
        }
        c->cr[2] = 0; c->cr[3] = 0;
        break;
    }
    case 0x74: {                                                /* read file */
        uint32_t off = pos24(r0, r1), id = pos24(r2, r3);
        const iso_file *f = dir_file(m, id);
        if (f) {
            uint32_t first = f->lba + FAD_OFFSET + off, sectors = (f->size + 2047) / 2048;
            c->cd_conn = (uint8_t)(r2 >> 8);
            start_play(c, first, f->lba + FAD_OFFSET + sectors);
        } else {
            LOGW(SUB_CD, "read file: bad file id %u", (unsigned)id);
        }
        status_report(c);
        break;
    }
    case 0x75:                                                  /* abort file */
        c->playing = false;
        c->status = ST_PAUSE;
        c->hirq |= HIRQ_EFLS;
        status_report(c);
        break;
    default:
        break;                                                  /* everything else: status only */
    }
}

/* ---- registers ---------------------------------------------------------------------------------------------- */

/* The register block repeats every 0x40 bytes through 0x25890000-0x2589FFFF (only the low 6 bits of the
 * address are decoded); the game's NetLink probe relies on the aliasing. */
static bool is_regs(uint32_t off) { return ((off >> 16) & 0xF) == 9 && (off & 0x8000) == 0; }
static bool is_data(uint32_t off) { return (((off >> 16) & 0xF) == 9 || ((off >> 16) & 0xF) == 1) && (off & 0x8000); }

uint16_t cdb_read(saturn *m, uint32_t off)
{
    cdb_state *c = &m->cdb;
    off &= 0xFFFFF;
    if (is_data(off)) {
        uint16_t v = 0;
        /* The transfer stays open after the last word: the host closes it with command 06, which reports
         * how many words were read (the CD library checks that count). */
        if (c->xactive && c->xpos + 1 < c->xlen) {
            v = (uint16_t)(c->xbuf[c->xpos] << 8 | c->xbuf[c->xpos + 1]);
            c->xpos += 2;
        }
        return v;
    }
    if (!is_regs(off))
        return 0;
    switch (off & 0x3C) {
    case 0x08: return c->hirq;
    case 0x0C: return c->hirq_mask;
    case 0x18: return c->cr[0];
    case 0x1C: return c->cr[1];
    case 0x20: return c->cr[2];
    case 0x24: return c->cr[3];
    default: return 0;
    }
}

void cdb_write(saturn *m, uint32_t off, uint16_t v)
{
    cdb_state *c = &m->cdb;
    off &= 0xFFFFF;
    if (!is_regs(off))
        return;
    switch (off & 0x3C) {
    case 0x08: c->hirq &= v; break;  /* writing 0 clears flags */
    case 0x0C: c->hirq_mask = v; break;
    case 0x18: c->cmd[0] = v; break;
    case 0x1C: c->cmd[1] = v; break;
    case 0x20: c->cmd[2] = v; break;
    case 0x24: c->cmd[3] = v; for (int i = 0; i < 4; i++) c->cr[i] = c->cmd[i]; execute(m); break; /* CR4 write issues the command */
    default: break;
    }
}
