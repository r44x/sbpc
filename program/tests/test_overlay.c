/* Overlay menu logic and settings persistence. No game data. */
#include "launcher/overlay.h"
#include "saturn/saturn.h"
#include "test.h"

int main(void)
{
    overlay o;
    play_settings s;
    config c;
    char items[OV_MAX_ITEMS][OV_TEXT];
    const char *title;
    int first, n;

    settings_defaults(&s);
    CHECK(!s.fullscreen && s.vsync && s.scale == SCALE_SHARP && s.master == 100);
    overlay_init(&o, &s);

    /* closed: keys do nothing */
    overlay_key(&o, OV_OK);
    CHECK(!o.open && !o.quit);

    overlay_toggle(&o);
    CHECK(o.open && o.page == OV_PAGE_MAIN && o.sel == 0);
    n = overlay_items(&o, items, &title, &first);
    CHECK(n == 6 && first == 0);
    CHECK_STR(title, "Paused");
    CHECK_STR(items[3], "Input");
    CHECK_STR(items[5], "Quit game");

    /* wraps around */
    overlay_key(&o, OV_UP);
    CHECK(o.sel == 5);
    overlay_key(&o, OV_DOWN);
    CHECK(o.sel == 0);

    /* Video: toggle fullscreen, cycle scaling both ways */
    overlay_key(&o, OV_DOWN);
    overlay_key(&o, OV_OK);
    CHECK(o.page == OV_PAGE_VIDEO && o.sel == 0);
    overlay_key(&o, OV_RIGHT);
    CHECK(o.set.fullscreen && o.changed);
    o.changed = false;
    overlay_key(&o, OV_DOWN);
    overlay_key(&o, OV_LEFT);
    CHECK(o.set.scale == SCALE_SHARP_BILINEAR && o.changed);   /* wraps to the last mode */
    overlay_key(&o, OV_RIGHT);
    CHECK(o.set.scale == SCALE_SHARP);
    overlay_items(&o, items, &title, &first);
    CHECK(strstr(items[1], "< Sharp >") != NULL);
    overlay_key(&o, OV_DOWN);
    overlay_key(&o, OV_DOWN);                              /* Ratio */
    overlay_key(&o, OV_OK);
    CHECK(o.set.widescreen);
    overlay_items(&o, items, &title, &first);
    CHECK(strstr(items[3], "16:9") != NULL);

    /* Back returns to the main page on the Video line; Back again closes */
    overlay_key(&o, OV_BACK);
    CHECK(o.page == OV_PAGE_MAIN && o.sel == 1 && o.open);

    /* Audio: volumes move in steps of 10 and clamp */
    overlay_key(&o, OV_DOWN);
    overlay_key(&o, OV_OK);
    CHECK(o.page == OV_PAGE_AUDIO);
    overlay_key(&o, OV_RIGHT);
    CHECK(o.set.master == 100);
    for (int i = 0; i < 12; i++) overlay_key(&o, OV_LEFT);
    CHECK(o.set.master == 0);
    overlay_key(&o, OV_DOWN);
    overlay_key(&o, OV_LEFT);
    CHECK(o.set.music == 90);
    /* the last line is Back */
    overlay_key(&o, OV_DOWN);
    overlay_key(&o, OV_DOWN);
    overlay_key(&o, OV_OK);
    CHECK(o.page == OV_PAGE_MAIN && o.sel == 2);

    /* Input: player lines, then Ports / Rebind keyboard / Rebind gamepads / Reset / Back */
    snprintf(o.players[0], OV_TEXT, "P1 Keyboard");
    o.nplayers = 1;
    overlay_key(&o, OV_DOWN);
    overlay_key(&o, OV_OK);
    CHECK(o.page == OV_PAGE_INPUT && o.sel == 1);          /* on Ports */
    n = overlay_items(&o, items, &title, &first);
    CHECK(n == 6 && first == 1 && strstr(items[1], "Auto") != NULL);
    overlay_key(&o, OV_RIGHT);
    CHECK(o.set.ports == PORTS_DIRECT && o.changed);
    overlay_key(&o, OV_LEFT);
    CHECK(o.set.ports == PORTS_AUTO);
    o.changed = false;
    overlay_key(&o, OV_DOWN);                              /* Rebind keyboard */
    overlay_key(&o, OV_OK);
    CHECK(o.rebind == 1 && o.rebind_step == 0);
    n = overlay_items(&o, items, &title, &first);
    CHECK(strstr(items[0], "Up") != NULL && first == 3 && n == 4);
    for (int i = 0; i < SB_COUNT; i++) overlay_rebind_next(&o);
    CHECK(o.rebind == 0 && o.changed);
    o.changed = false;
    o.sel = 3;                                             /* Rebind gamepads, then cancel with Back */
    overlay_key(&o, OV_OK);
    CHECK(o.rebind == 2);
    overlay_key(&o, OV_BACK);
    CHECK(o.rebind == 0 && o.page == OV_PAGE_INPUT && !o.changed);
    o.sel = 4;                                             /* Reset bindings */
    overlay_key(&o, OV_OK);
    CHECK(o.reset_bindings && o.changed);
    o.reset_bindings = o.changed = false;
    overlay_key(&o, OV_BACK);
    CHECK(o.page == OV_PAGE_MAIN && o.sel == 3);

    /* Info: information lines aren't selectable; only Back is */
    snprintf(o.info[0], OV_TEXT, "line one");
    snprintf(o.info[1], OV_TEXT, "line two");
    o.ninfo = 2;
    overlay_key(&o, OV_DOWN);
    overlay_key(&o, OV_OK);
    CHECK(o.page == OV_PAGE_INFO && o.sel == 2);   /* lands on Back */
    n = overlay_items(&o, items, &title, &first);
    CHECK(n == 3 && first == 2);
    overlay_key(&o, OV_UP);
    CHECK(o.sel == 2);
    overlay_key(&o, OV_OK);
    CHECK(o.page == OV_PAGE_MAIN);

    /* Quit game */
    o.sel = 5;
    overlay_key(&o, OV_OK);
    CHECK(o.quit && !o.open);

    /* settings round trip through the config, with out-of-range values clamped */
    config_init(&c);
    settings_store(&o.set, &c);
    settings_load(&s, &c);
    CHECK(s.fullscreen && s.scale == SCALE_SHARP && s.master == 0 && s.music == 90 && s.sfx == 100);
    CHECK(s.ports == PORTS_AUTO && s.widescreen);
    config_set(&c, "audio_sfx", "250");
    config_set(&c, "video_scale", "9");
    settings_load(&s, &c);
    CHECK(s.sfx == 100 && s.scale == SCALE_COUNT - 1);

    TEST_DONE();
}
