#include "disc/identify.h"
#include "core/log.h"

#include <stdio.h>
#include <string.h>

const char *id_status_name(id_status s)
{
    switch (s) {
    case ID_SUPPORTED: return "supported";
    case ID_UNKNOWN_VERSION: return "unknown-version";
    case ID_OTHER_GAME: return "other-game";
    case ID_NOT_SATURN: return "not-saturn";
    case ID_BAD_IMAGE: return "bad-image";
    }
    return "?";
}

static const char *region_name(const char *areas)
{
    switch (areas[0]) {
    case 'J': return "Japan";
    case 'U': return "USA";
    case 'E': return "Europe";
    case 'T': return "Asia (NTSC)";
    case 'K': return "Korea";
    case 'B': return "Brazil";
    case 'A': return "Asia (PAL)";
    case 'L': return "Latin America";
    default: return "Unknown";
    }
}

static bool looks_like_bomberman(const saturn_header *h)
{
    return strstr(h->title, "BOMBERMAN") && !strstr(h->title, "FIGHT");
}

void identify_disc(const char *cue_path, disc_identity *id, disc_progress_cb progress, void *user)
{
    disc d;
    uint8_t sector[2048];
    int data;

    memset(id, 0, sizeof *id);
    id->status = ID_BAD_IMAGE;

    if (!disc_open(&d, cue_path, id->reason, sizeof id->reason))
        return;
    id->track_count = d.cue.track_count;
    data = disc_first_data_track(&d);
    if (data != 0) {
        snprintf(id->reason, sizeof id->reason, "track 1 is not a data track");
        goto done;
    }
    if (!disc_read_user(&d, data, 0, sector)) {
        snprintf(id->reason, sizeof id->reason, "cannot read sector 0 of the data track");
        goto done;
    }
    if (!saturn_header_parse(sector, &id->header)) {
        id->status = ID_NOT_SATURN;
        snprintf(id->reason, sizeof id->reason, "not a Sega Saturn disc");
        goto done;
    }
    id->have_header = true;
    snprintf(id->region, sizeof id->region, "%s", region_name(id->header.areas));
    LOGD(SUB_DISC, "%s: product=%s version=%s areas=%s title=%s", fs_basename(cue_path),
         id->header.product, id->header.version, id->header.areas, id->header.title);

    if (!looks_like_bomberman(&id->header)) {
        bool listed = false;
        for (int i = 0; i < GAME_VERSION_COUNT; i++)
            listed = listed || strcmp(GAME_VERSIONS[i].product, id->header.product) == 0;
        if (!listed) {
            id->status = ID_OTHER_GAME;
            snprintf(id->reason, sizeof id->reason, "this is \"%s\", not Saturn Bomberman", id->header.title);
            goto done;
        }
    }

    if (!disc_hash_track(&d, data, id->data_sha1, progress, user)) {
        snprintf(id->reason, sizeof id->reason, "could not read the whole data track (cancelled or damaged)");
        goto done;
    }
    LOGD(SUB_DISC, "%s: data sha1=%s", fs_basename(cue_path), id->data_sha1);

    for (int i = 0; i < GAME_VERSION_COUNT; i++) {
        const game_version *v = &GAME_VERSIONS[i];
        if (strcmp(v->data_sha1, id->data_sha1) != 0)
            continue;
        if (id->track_count != v->track_count) {
            snprintf(id->reason, sizeof id->reason,
                     "data track matches %s but the cue lists %d tracks instead of %d (music would be missing)",
                     v->name, id->track_count, v->track_count);
            goto done;
        }
        id->status = ID_SUPPORTED;
        id->version = v;
        id->reason[0] = 0;
        goto done;
    }

    for (int i = 0; i < GAME_VERSION_COUNT; i++) {
        const game_version *v = &GAME_VERSIONS[i];
        if (strcmp(v->product, id->header.product) == 0 && strcmp(v->version, id->header.version) == 0) {
            snprintf(id->reason, sizeof id->reason,
                     "header says %s %s but the data doesn't match a good dump (bad or modified image)",
                     v->product, v->version);
            goto done;
        }
    }
    id->status = ID_UNKNOWN_VERSION;
    snprintf(id->reason, sizeof id->reason,
             "Saturn Bomberman %s (%s %s) isn't supported yet - please report its product, version and SHA-1",
             id->region, id->header.product, id->header.version);
done:
    disc_close(&d);
}

bool identify_installed(const char *cue_path, const game_version *v, disc_identity *id)
{
    disc d;
    uint8_t sector[2048];
    bool ok = false;

    memset(id, 0, sizeof *id);
    id->status = ID_BAD_IMAGE;
    if (!disc_open(&d, cue_path, id->reason, sizeof id->reason))
        return false;
    id->track_count = d.cue.track_count;
    if (disc_first_data_track(&d) == 0 && disc_read_user(&d, 0, 0, sector) &&
        saturn_header_parse(sector, &id->header) && strcmp(id->header.product, v->product) == 0 &&
        strcmp(id->header.version, v->version) == 0 && id->track_count == v->track_count) {
        id->have_header = true;
        snprintf(id->region, sizeof id->region, "%s", region_name(id->header.areas));
        snprintf(id->data_sha1, sizeof id->data_sha1, "%s", v->data_sha1);   /* trusted: the build checked it */
        id->status = ID_SUPPORTED;
        id->version = v;
        ok = true;
    }
    disc_close(&d);
    return ok;
}
