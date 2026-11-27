#include "disc/cue.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool fail(char *err, size_t errlen, const char *fmt, ...)
{
    va_list ap;
    if (err && errlen) {
        va_start(ap, fmt);
        vsnprintf(err, errlen, fmt, ap);
        va_end(ap);
    }
    return false;
}

const char *track_mode_name(track_mode m)
{
    switch (m) {
    case TRACK_MODE1_2048: return "MODE1/2048";
    case TRACK_MODE1_2352: return "MODE1/2352";
    case TRACK_MODE2_2352: return "MODE2/2352";
    case TRACK_AUDIO: return "AUDIO";
    }
    return "?";
}

bool track_is_data(const cue_track *t) { return t->mode != TRACK_AUDIO; }

int track_user_data_offset(const cue_track *t)
{
    switch (t->mode) {
    case TRACK_MODE1_2352: return 16;
    case TRACK_MODE2_2352: return 24; /* Form 1: sync(12) header(4) subheader(8) */
    default: return 0;
    }
}

static uint32_t rd32le(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

/* Find the "data" chunk of a RIFF WAVE file. Expects 16-bit stereo 44.1 kHz (CD audio). */
static bool wave_data_offset(const char *path, int64_t *offset, int64_t *size, char *err, size_t errlen)
{
    uint8_t hdr[12], ch[8], fmt[16];
    int64_t pos = 12;
    bool fmt_ok = false;
    FILE *f = fs_open(path, "rb");
    if (!f)
        return fail(err, errlen, "cannot open %s", fs_basename(path));
    if (fread(hdr, 1, 12, f) != 12 || memcmp(hdr, "RIFF", 4) || memcmp(hdr + 8, "WAVE", 4)) {
        fclose(f);
        return fail(err, errlen, "%s is not a WAVE file", fs_basename(path));
    }
    for (;;) {
        uint32_t len;
        if (!fs_seek(f, pos) || fread(ch, 1, 8, f) != 8)
            break;
        len = rd32le(ch + 4);
        if (memcmp(ch, "fmt ", 4) == 0 && len >= 16 && fread(fmt, 1, 16, f) == 16) {
            fmt_ok = fmt[0] == 1 && fmt[2] == 2 && rd32le(fmt + 4) == 44100 && fmt[14] == 16;
        } else if (memcmp(ch, "data", 4) == 0) {
            fclose(f);
            if (!fmt_ok)
                return fail(err, errlen, "%s must be 16-bit stereo 44100 Hz PCM", fs_basename(path));
            *offset = pos + 8;
            *size = len;
            return true;
        }
        pos += 8 + len + (len & 1);
    }
    fclose(f);
    return fail(err, errlen, "%s has no data chunk", fs_basename(path));
}

/* Reads one token: quoted string or run of non-space characters. */
static const char *token(const char *p, char *out, size_t cap)
{
    size_t n = 0;
    while (*p == ' ' || *p == '\t')
        p++;
    if (*p == '"') {
        p++;
        while (*p && *p != '"' && *p != '\n' && *p != '\r') {
            if (n + 1 < cap) out[n++] = *p;
            p++;
        }
        if (*p == '"')
            p++;
    } else {
        while (*p && !isspace((unsigned char)*p)) {
            if (n + 1 < cap) out[n++] = *p;
            p++;
        }
    }
    out[n] = 0;
    return p;
}

static bool parse_msf(const char *s, int64_t *out)
{
    int m, sec, fr;
    if (sscanf(s, "%d:%d:%d", &m, &sec, &fr) != 3 || sec < 0 || sec > 59 || fr < 0 || fr > 74 || m < 0)
        return false;
    *out = ((int64_t)m * 60 + sec) * 75 + fr;
    return true;
}

static bool ieq(const char *a, const char *b)
{
    for (; *a && *b; a++, b++)
        if (toupper((unsigned char)*a) != toupper((unsigned char)*b))
            return false;
    return *a == *b;
}

bool cue_parse(const char *text, const char *base_dir, cue_sheet *out, char *err, size_t errlen)
{
    char cur_file[SBPC_PATH_MAX] = "";
    bool cur_wave = false;
    int64_t cur_wave_off = 0, cur_wave_size = 0;
    const char *p = text;
    int line_no = 0;

    memset(out, 0, sizeof *out);

    while (*p) {
        char line[2048], kw[32], a[SBPC_PATH_MAX], b[64];
        const char *lp;
        size_t n = 0;

        while (p[n] && p[n] != '\n')
            n++;
        if (n >= sizeof line)
            return fail(err, errlen, "line %d too long", line_no + 1);
        memcpy(line, p, n);
        line[n] = 0;
        p += n + (p[n] == '\n');
        line_no++;
        if (n && line[n - 1] == '\r')
            line[n - 1] = 0;
        if (line_no == 1 && (unsigned char)line[0] == 0xEF) /* UTF-8 BOM */
            memmove(line, line + 3, strlen(line + 3) + 1);

        lp = token(line, kw, sizeof kw);
        if (!kw[0])
            continue;

        if (ieq(kw, "FILE")) {
            char name[SBPC_PATH_MAX];
            lp = token(lp, name, sizeof name);
            token(lp, b, sizeof b);
            if (!name[0])
                return fail(err, errlen, "line %d: FILE without a name", line_no);
            if (!fs_join(cur_file, base_dir, name))
                return fail(err, errlen, "line %d: path too long", line_no);
            cur_wave = ieq(b, "WAVE");
            if (!ieq(b, "BINARY") && !cur_wave)
                return fail(err, errlen, "line %d: unsupported file type '%s' (BINARY or WAVE only)", line_no, b);
            if (!fs_is_file(cur_file))
                return fail(err, errlen, "missing file: %s", name);
            cur_wave_off = 0;
            if (cur_wave && !wave_data_offset(cur_file, &cur_wave_off, &cur_wave_size, err, errlen))
                return false;
        } else if (ieq(kw, "TRACK")) {
            cue_track *t;
            lp = token(lp, a, sizeof a);
            token(lp, b, sizeof b);
            if (!cur_file[0])
                return fail(err, errlen, "line %d: TRACK before FILE", line_no);
            if (out->track_count == CUE_MAX_TRACKS)
                return fail(err, errlen, "too many tracks");
            t = &out->tracks[out->track_count++];
            t->number = atoi(a);
            t->index0 = t->index1 = -1;
            strcpy(t->path, cur_file);
            t->is_wave = cur_wave;
            t->data_offset = cur_wave_off;
            if (ieq(b, "MODE1/2352"))      { t->mode = TRACK_MODE1_2352; t->sector_size = 2352; }
            else if (ieq(b, "MODE2/2352")) { t->mode = TRACK_MODE2_2352; t->sector_size = 2352; }
            else if (ieq(b, "MODE1/2048")) { t->mode = TRACK_MODE1_2048; t->sector_size = 2048; }
            else if (ieq(b, "AUDIO"))      { t->mode = TRACK_AUDIO;      t->sector_size = 2352; }
            else
                return fail(err, errlen, "line %d: unsupported track mode '%s'", line_no, b);
            if (cur_wave && t->mode != TRACK_AUDIO)
                return fail(err, errlen, "line %d: WAVE file used for a data track", line_no);
            if (t->number < 1 || t->number > 99 ||
                (out->track_count > 1 && t->number != out->tracks[out->track_count - 2].number + 1))
                return fail(err, errlen, "line %d: bad track number %s", line_no, a);
        } else if (ieq(kw, "INDEX")) {
            cue_track *t;
            int64_t sector;
            int idx;
            lp = token(lp, a, sizeof a);
            token(lp, b, sizeof b);
            if (out->track_count == 0)
                return fail(err, errlen, "line %d: INDEX before TRACK", line_no);
            t = &out->tracks[out->track_count - 1];
            idx = atoi(a);
            if (!parse_msf(b, &sector))
                return fail(err, errlen, "line %d: bad time '%s'", line_no, b);
            if (idx == 0) t->index0 = sector;
            else if (idx == 1) t->index1 = sector;
        }
        /* PREGAP, POSTGAP, CATALOG, REM, FLAGS, ISRC, TITLE, PERFORMER: ignored for now. */
    }

    if (out->track_count == 0)
        return fail(err, errlen, "no tracks in cue sheet");

    /* Work out each track's length: up to the next track in the same file, or the file end. */
    for (int i = 0; i < out->track_count; i++) {
        cue_track *t = &out->tracks[i];
        int64_t end;
        if (t->index1 < 0)
            return fail(err, errlen, "track %d has no INDEX 01", t->number);
        if (i + 1 < out->track_count && strcmp(out->tracks[i + 1].path, t->path) == 0) {
            const cue_track *nx = &out->tracks[i + 1];
            end = nx->index0 >= 0 ? nx->index0 : nx->index1;
        } else {
            int64_t bytes = fs_size(t->path);
            if (t->is_wave) {
                int64_t off, sz;
                if (!wave_data_offset(t->path, &off, &sz, err, errlen))
                    return false;
                bytes = sz;
            }
            if (bytes % t->sector_size)
                return fail(err, errlen, "%s: size is not a multiple of %d", fs_basename(t->path), t->sector_size);
            end = bytes / t->sector_size;
        }
        if (end < t->index1)
            return fail(err, errlen, "track %d: index past end of file", t->number);
        t->sectors = end - t->index1;
    }
    return true;
}

bool cue_load(const char *cue_path, cue_sheet *out, char *err, size_t errlen)
{
    char dir[SBPC_PATH_MAX], *text;
    int64_t size = fs_size(cue_path);
    FILE *f;
    bool ok;

    if (size < 0)
        return fail(err, errlen, "cannot find %s", fs_basename(cue_path));
    if (size > 64 * 1024)
        return fail(err, errlen, "%s is too large to be a cue sheet", fs_basename(cue_path));
    if (!fs_dirname(dir, cue_path))
        return fail(err, errlen, "path too long");
    f = fs_open(cue_path, "rb");
    if (!f)
        return fail(err, errlen, "cannot open %s", fs_basename(cue_path));
    text = malloc((size_t)size + 1);
    if (!text) {
        fclose(f);
        return fail(err, errlen, "out of memory");
    }
    ok = fread(text, 1, (size_t)size, f) == (size_t)size;
    fclose(f);
    text[size] = 0;
    ok = ok ? cue_parse(text, dir, out, err, errlen) : fail(err, errlen, "read error");
    free(text);
    return ok;
}
