#include "disc/identify.h"
#include "fake_disc.h"

int main(void)
{
    disc_identity id;

    /* A Saturn disc that isn't Saturn Bomberman. */
    CHECK(make_fake_disc("t_id_other", "SOME OTHER GAME", "T-00002", 16, 0));
    identify_disc("t_id_other.cue", &id, NULL, NULL);
    CHECK(id.status == ID_OTHER_GAME);
    CHECK(id.have_header);
    CHECK(id.data_sha1[0] == 0); /* not hashed */

    /* Looks like Saturn Bomberman but isn't a recorded version. */
    CHECK(make_fake_disc("t_id_bm", "SATURN BOMBERMAN", "T-99999", 16, 2));
    identify_disc("t_id_bm.cue", &id, NULL, NULL);
    CHECK(id.status == ID_UNKNOWN_VERSION);
    CHECK_STR(id.region, "Japan");
    CHECK(strlen(id.data_sha1) == 40);
    CHECK(id.track_count == 3);

    /* The sequel is a different game. */
    CHECK(make_fake_disc("t_id_fight", "SATURN BOMBERMAN FIGHT!!", "T-99998", 16, 0));
    identify_disc("t_id_fight.cue", &id, NULL, NULL);
    CHECK(id.status == ID_OTHER_GAME);

    /* Missing files */
    identify_disc("does_not_exist.cue", &id, NULL, NULL);
    CHECK(id.status == ID_BAD_IMAGE);

    /* Version table sanity */
    CHECK(GAME_VERSION_COUNT >= 1);
    for (int i = 0; i < GAME_VERSION_COUNT; i++) {
        CHECK(strlen(GAME_VERSIONS[i].data_sha1) == 40);
        CHECK(GAME_VERSIONS[i].track_count >= 1);
    }

    remove("t_id_other.bin"); remove("t_id_other.cue");
    remove("t_id_bm.bin"); remove("t_id_bm.cue");
    remove("t_id_fight.bin"); remove("t_id_fight.cue");
    TEST_DONE();
}
