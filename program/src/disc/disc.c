#include "disc/disc.h"
#include "disc/chd.h"
#include "core/sha1.h"

#include <stdlib.h>
#include <string.h>

bool disc_is_image_name(const char *name) { return fs_has_ext(name, ".cue") || fs_has_ext(name, ".chd"); }

/* A CHD's tracks as a cue sheet: one "file" per track holding its stored pregap then the track, like a
 * redump cue/bin set (INDEX 00 at 0, INDEX 01 after the pregap). */
static bool open_chd(disc *d, const char *path, char *err, size_t errlen)
{
    d->chd = chd_open(path, err, errlen);
    if (!d->chd) return false;
    d->cue.track_count = chd_track_count(d->chd);
    for (int i = 0; i < d->cue.track_count; i++) {
        const chd_track *ct = chd_get_track(d->chd, i);
        cue_track *t = &d->cue.tracks[i];
        int stored_pregap = ct->pregap_stored ? ct->pregap : 0;
        t->number = i + 1;
        switch (ct->type) {
        case CHD_TRACK_MODE1_RAW: t->mode = TRACK_MODE1_2352; t->sector_size = 2352; break;
        case CHD_TRACK_MODE1: t->mode = TRACK_MODE1_2048; t->sector_size = 2048; break;
        case CHD_TRACK_MODE2_RAW: t->mode = TRACK_MODE2_2352; t->sector_size = 2352; break;
        case CHD_TRACK_AUDIO: t->mode = TRACK_AUDIO; t->sector_size = 2352; break;
        default:
            snprintf(err, errlen, "track %d of the CHD has a type SBPC can't read", i + 1);
            chd_close(d->chd);
            d->chd = NULL;
            return false;
        }
        snprintf(t->path, sizeof t->path, "%s", path);
        t->data_offset = ct->first;
        t->index0 = stored_pregap > 0 ? 0 : -1;
        t->index1 = stored_pregap;
        t->sectors = ct->frames - stored_pregap;
    }
    return true;
}

bool disc_open(disc *d, const char *cue_path, char *err, size_t errlen)
{
    memset(d, 0, sizeof *d);
    if (fs_has_ext(cue_path, ".chd"))
        return open_chd(d, cue_path, err, errlen);
    return cue_load(cue_path, &d->cue, err, errlen);
}

void disc_close(disc *d)
{
    if (d->fp)
        fclose(d->fp);
    d->fp = NULL;
    d->fp_path[0] = 0;
    if (d->chd) {
        chd_close(d->chd);
        d->chd = NULL;
    }
}

/* CHD: one stored frame of track t (sector = relative to INDEX 01). */
static bool chd_sector(disc *d, const cue_track *t, int64_t sector, uint8_t frame[CHD_FRAME])
{
    return chd_read_frame(d->chd, (uint32_t)(t->data_offset + t->index1 + sector), frame, false);
}

int disc_first_data_track(const disc *d)
{
    for (int i = 0; i < d->cue.track_count; i++)
        if (track_is_data(&d->cue.tracks[i]))
            return i;
    return -1;
}

static FILE *track_file(disc *d, const cue_track *t)
{
    if (d->fp && strcmp(d->fp_path, t->path) == 0)
        return d->fp;
    disc_close(d);
    d->fp = fs_open(t->path, "rb");
    if (d->fp)
        strcpy(d->fp_path, t->path);
    return d->fp;
}

bool disc_read_user(disc *d, int track, int64_t sector, uint8_t out[2048])
{
    const cue_track *t;
    FILE *f;
    int64_t pos;
    if (track < 0 || track >= d->cue.track_count)
        return false;
    t = &d->cue.tracks[track];
    if (!track_is_data(t) || sector < 0 || sector >= t->sectors)
        return false;
    if (d->chd) {
        uint8_t frame[CHD_FRAME];
        if (!chd_sector(d, t, sector, frame)) return false;
        memcpy(out, frame + track_user_data_offset(t), 2048);
        return true;
    }
    f = track_file(d, t);
    if (!f)
        return false;
    pos = t->data_offset + (t->index1 + sector) * t->sector_size + track_user_data_offset(t);
    return fs_seek(f, pos) && fread(out, 1, 2048, f) == 2048;
}

bool disc_read_audio(disc *d, int track, int64_t sector, uint8_t out[2352])
{
    const cue_track *t;
    FILE *f;
    memset(out, 0, 2352);
    if (track < 0 || track >= d->cue.track_count)
        return false;
    t = &d->cue.tracks[track];
    if (track_is_data(t) || sector < 0 || sector >= t->sectors)
        return true;                       /* pregap / past the end: silence */
    if (d->chd) {                          /* CHD keeps CD audio big-endian */
        uint8_t frame[CHD_FRAME];
        if (!chd_sector(d, t, sector, frame)) return false;
        for (int i = 0; i < 2352; i += 2) { out[i] = frame[i + 1]; out[i + 1] = frame[i]; }
        return true;
    }
    f = track_file(d, t);
    if (!f)
        return false;
    return fs_seek(f, t->data_offset + (t->index1 + sector) * t->sector_size) && fread(out, 1, 2352, f) == 2352;
}

bool disc_hash_track(disc *d, int track, char hex_out[41], disc_progress_cb progress, void *user)
{
    enum { BATCH = 256 };
    const cue_track *t;
    uint8_t *buf, digest[20];
    sha1_ctx ctx;
    FILE *f;
    int off;

    if (track < 0 || track >= d->cue.track_count || !track_is_data(&d->cue.tracks[track]))
        return false;
    t = &d->cue.tracks[track];
    if (d->chd) {
        uint8_t sec[2048];
        sha1_init(&ctx);
        for (int64_t s = 0; s < t->sectors; s++) {
            if (!disc_read_user(d, track, s, sec)) return false;
            sha1_update(&ctx, sec, 2048);
            if (progress && (s & 1023) == 0 && !progress((float)s / (float)t->sectors, user)) return false;
        }
        sha1_final(&ctx, digest);
        sha1_hex(digest, hex_out);
        return true;
    }
    f = track_file(d, t);
    if (!f || !fs_seek(f, t->data_offset + t->index1 * t->sector_size))
        return false;
    buf = malloc((size_t)BATCH * t->sector_size);
    if (!buf)
        return false;
    off = track_user_data_offset(t);
    sha1_init(&ctx);

    for (int64_t s = 0; s < t->sectors; s += BATCH) {
        int64_t n = t->sectors - s < BATCH ? t->sectors - s : BATCH;
        if (fread(buf, (size_t)t->sector_size, (size_t)n, f) != (size_t)n) {
            free(buf);
            return false;
        }
        for (int64_t i = 0; i < n; i++)
            sha1_update(&ctx, buf + i * t->sector_size + off, 2048);
        if (progress && !progress((float)(s + n) / (float)t->sectors, user)) {
            free(buf);
            return false;
        }
    }
    free(buf);
    sha1_final(&ctx, digest);
    sha1_hex(digest, hex_out);
    return true;
}

static void put_le32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

/* 44-byte RIFF header for 16-bit stereo 44.1 kHz PCM. */
static void wave_header(uint8_t h[44], uint32_t data_bytes)
{
    memcpy(h, "RIFF", 4);
    put_le32(h + 4, 36 + data_bytes);
    memcpy(h + 8, "WAVEfmt ", 8);
    put_le32(h + 16, 16);
    h[20] = 1; h[21] = 0;                  /* PCM */
    h[22] = 2; h[23] = 0;                  /* stereo */
    put_le32(h + 24, 44100);
    put_le32(h + 28, 44100 * 4);
    h[32] = 4; h[33] = 0;                  /* block align */
    h[34] = 16; h[35] = 0;                 /* bits per sample */
    memcpy(h + 36, "data", 4);
    put_le32(h + 40, data_bytes);
}

bool disc_export_isowav(disc *d, const char *dir, const char *base, disc_progress_cb progress, void *user,
                        char *err, size_t errlen)
{
    char name[SBPC_PATH_MAX], path[SBPC_PATH_MAX], cue_path[SBPC_PATH_MAX];
    static char cue_text[16384];
    size_t cue_len = 0;
    int64_t total = 0, done = 0;
    FILE *cue;

    if (!fs_mkdirs(dir)) { snprintf(err, errlen, "can't create %.400s", dir); return false; }
    for (int t = 0; t < d->cue.track_count; t++) total += d->cue.tracks[t].index1 + d->cue.tracks[t].sectors;
    for (int t = 0; t < d->cue.track_count; t++) {
        const cue_track *tr = &d->cue.tracks[t];
        bool data = track_is_data(tr);
        int64_t pregap = tr->index1 > 0 ? tr->index1 : 0;
        FILE *f;
        snprintf(name, sizeof name, "%s (Track %02d).%s", base, t + 1, data ? "iso" : "wav");
        if (!fs_join(path, dir, name) || !(f = fs_open(path, "wb"))) {
            snprintf(err, errlen, "can't write %.400s", name);
            return false;
        }
        if (!data) {
            uint8_t h[44];
            wave_header(h, (uint32_t)((pregap + tr->sectors) * 2352));
            fwrite(h, 1, sizeof h, f);
        }
        for (int64_t s = -pregap; s < tr->sectors; s++) {
            uint8_t buf[2352];
            bool ok;
            if (data) ok = s < 0 ? (memset(buf, 0, 2048), true) : disc_read_user(d, t, s, buf);
            else ok = disc_read_audio(d, t, s, buf);       /* the pregap reads as silence */
            if (!ok || fwrite(buf, 1, data ? 2048 : 2352, f) != (data ? 2048u : 2352u)) {
                fclose(f);
                snprintf(err, errlen, "can't copy track %d (disk full?)", t + 1);
                return false;
            }
            if (progress && (++done & 2047) == 0 && !progress((float)done / (float)total, user)) {
                fclose(f);
                snprintf(err, errlen, "cancelled");
                return false;
            }
        }
        if (fclose(f) != 0) { snprintf(err, errlen, "can't write %.400s (disk full?)", name); return false; }
        cue_len += (size_t)snprintf(cue_text + cue_len, sizeof cue_text - cue_len, "FILE \"%s\" %s\n  TRACK %02d %s\n", name,
                                    data ? "BINARY" : "WAVE", t + 1, data ? "MODE1/2048" : "AUDIO");
        if (pregap > 0)
            cue_len += (size_t)snprintf(cue_text + cue_len, sizeof cue_text - cue_len,
                                        "    INDEX 00 00:00:00\n    INDEX 01 %02d:%02d:%02d\n",
                                        (int)(pregap / 4500), (int)(pregap / 75 % 60), (int)(pregap % 75));
        else
            cue_len += (size_t)snprintf(cue_text + cue_len, sizeof cue_text - cue_len, "    INDEX 01 00:00:00\n");
        if (cue_len >= sizeof cue_text) { snprintf(err, errlen, "too many tracks"); return false; }
    }
    snprintf(name, sizeof name, "%s.cue", base);
    if (!fs_join(cue_path, dir, name) || !(cue = fs_open(cue_path, "w")) || fputs(cue_text, cue) < 0) {
        snprintf(err, errlen, "can't write the cue sheet");
        return false;
    }
    fclose(cue);
    if (progress) progress(1.0f, user);
    return true;
}
