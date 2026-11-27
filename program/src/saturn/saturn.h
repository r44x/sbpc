/* The Saturn machine around the two SH-2s: memory map, BIOS replacement, SCU, SMPC, and
 * (for now) stubs for VDP1/VDP2/SCSP/CD block that keep and log register accesses. */
#ifndef SBPC_SATURN_H
#define SBPC_SATURN_H

#include <stdbool.h>
#include <stdio.h>

#include "disc/iso9660.h"
#include "disc/versions.h"
#include "runtime/dispatch.h"
#include "sh2/sh2_state.h"
#include "m68k/m68k.h"

/* ---- I/O access log ------------------------------------------------------------------------- */
#define IOLOG_SIZE 8192
typedef struct {
    uint32_t addr;     /* 0 = empty slot */
    uint8_t write;
    uint8_t size;
    uint32_t count;
    uint32_t first_pc, last_value;
} io_stat;

/* ---- SCU ------------------------------------------------------------------------------------ */
typedef struct {
    uint32_t regs[0x100 / 4];
    uint32_t ims;          /* interrupt mask (1 = masked) */
    uint32_t ist;          /* interrupt status */
    uint32_t pending;      /* raised but not yet taken by the CPU */
    uint32_t dma_count;
} scu_state;

/* ---- SMPC ----------------------------------------------------------------------------------- */
/* What is plugged into a controller port. */
enum { SMPC_PORT_NONE, SMPC_PORT_PAD, SMPC_PORT_TAP };
#define SMPC_SLOTS 12       /* pad slots: port 1 = 0-5, port 2 = 6-11 (slot 0 / 6 = a direct pad) */

typedef struct {
    uint8_t ireg[7];
    uint8_t oreg[32];
    uint8_t sr, sf;
    uint8_t pdr[2], ddr[2], iosel, exle;
    uint16_t pad[SMPC_SLOTS]; /* active-low Saturn pad bits per slot (0xFFFF = nothing pressed) */
    uint8_t port_mode[2];   /* SMPC_PORT_*: default one pad on port 1, nothing on port 2 */
    uint8_t tap_pads[2];    /* pads plugged into a port's multitap (0-6); its other connectors are empty */
    uint8_t per[64];        /* INTBACK peripheral report being handed out 32 bytes at a time */
    int per_len, per_pos;
    int commands[32];       /* count per command */
} smpc_state;

/* ---- CD block ------------------------------------------------------------------------------- */
#define CDB_BLOCKS 200
#define CDB_CDDA_FRAMES 16384
#define CDB_SEL 24
#define CDB_DIR_MAX 512

typedef struct {
    uint8_t mode, fid, chan, smmask, cimask, smval, cival;
    uint8_t true_conn, false_conn;  /* true -> buffer partition, false -> another filter (0xFF = none) */
    uint32_t range_start, range_count;
} cdb_filter;

typedef struct {
    uint16_t hirq, hirq_mask;
    uint16_t cr[4];         /* response registers (what the host reads) */
    uint16_t cmd[4];        /* command registers (what the host writes; reads never return these) */
    uint8_t status;         /* 1 paused, 3 playing, ... */
    uint32_t fad;           /* current position */
    uint32_t play_end;      /* first FAD not to read */
    bool playing;
    int accum;              /* sector pacing */
    uint32_t play_start;    /* for repeats */
    int repeat_left;        /* plays left after this one; -1 = forever */
    int cur_track;          /* 1-based track under the pickup */
    bool cur_audio;
    int audio_acc;          /* audio sector pacing: 75 sectors/s */
    int16_t cdda[CDB_CDDA_FRAMES * 2];   /* CD audio on its way to the SCSP (stereo) */
    uint32_t cdda_w, cdda_r;
    uint8_t cd_conn;        /* filter the CD device is connected to (0xFF = none) */
    uint8_t last_dest;
    uint8_t get_len, put_len;
    cdb_filter filt[CDB_SEL];
    bool blk_used[CDB_BLOCKS];
    uint32_t blk_fad[CDB_BLOCKS];
    int sel_count[CDB_SEL];
    int16_t sel_blk[CDB_SEL][CDB_BLOCKS];
    uint8_t pool[CDB_BLOCKS][2048];
    /* host data transfer (DTR) */
    uint8_t xbuf[CDB_BLOCKS * 2048 + 4096];
    uint32_t xlen, xpos;
    bool xactive;
    int xdel_sel, xdel_start, xdel_count;   /* get-then-delete */
    /* file system */
    char cur_dir[ISO_NAME_MAX];   /* "" = root */
    int dir_entries[CDB_DIR_MAX]; /* indices into iso_fs.files */
    int dir_count;
    int command_count[256];
    uint32_t last_cmd[4];
} cdb_state;

/* ---- BIOS replacement ------------------------------------------------------------------------- */
#define BIOS_TRAP_SLOT  0x00010000u  /* + slot offset (0x200..0x3FF etc.): BIOS function slots */
#define BIOS_TRAP_UINT  0x00020000u  /* + vector*4: user interrupt wrapper entry */
#define BIOS_TRAP_URET  0x00021000u  /* + vector*4: user interrupt wrapper return */
#define BIOS_TRAP_EXC   0x00030000u  /* + vector*4: unhandled exception / interrupt */

typedef struct {
    uint32_t regs[8], mach, macl, pr;
} uint_frame;

typedef struct {
    uint32_t uint_handler[128];
    uint8_t sem[256];
    int calls[0x400 / 4];     /* per slot */
    int unhandled[256];       /* per vector */
    int rom_calls;
    uint_frame frames[2][16]; /* per CPU: nested user-interrupt frames */
    int depth[2];
} bios_state;

/* SCSP (sound chip) and its 68000. Registers are 16-bit; 0x000-0x3FF slots, 0x400-0x42F common,
 * 0x600-0x67F sound stack, 0x700-0xEE3 DSP. */
typedef struct {
    bool active;
    bool released;
    uint64_t pos;             /* sample index within the slot's data, 32.32 fixed point */
    int dir;                  /* +1, or -1 in reverse / alternating loops */
    int32_t env;              /* envelope level 0 (silent, -96 dB) .. 1023 (full), for reports */
    int eg_state;             /* SCSP_EG_* */
    int32_t eg_level;         /* envelope level, 10.16 fixed point */
} scsp_slot;

enum { SCSP_EG_ATTACK, SCSP_EG_DECAY1, SCSP_EG_DECAY2, SCSP_EG_RELEASE };

#define SCSP_OUT_FRAMES 16384

typedef struct {
    m68k cpu;
    bool cpu_on;
    scsp_slot slot[32];
    int16_t out[SCSP_OUT_FRAMES * 2];   /* stereo ring buffer for the audio device */
    uint32_t out_w, out_r;              /* SMPC SNDON/SNDOFF */
    uint16_t r[0x800];
    uint8_t timer[3];         /* timers A, B, C (count up, interrupt on overflow) */
    uint32_t timer_div[3];    /* samples left until the next tick */
    uint32_t scipd, mcipd;    /* pending interrupts: 68000 side, SH-2 side */
    bool mc_line;             /* SH-2 "sound request" currently asserted */
    double cyc_acc, smp_acc;  /* fractional 68000 cycles / samples per line */
    uint64_t samples;
    uint32_t irq_count, dma_count;
    uint32_t keyed;           /* slots currently playing */
    /* effects DSP (see saturn_scsp.c) */
    int32_t dsp_temp[128], dsp_mems[32], dsp_mixs[16];
    int16_t dsp_efreg[16], dsp_exts[2];
    uint32_t dsp_dec;
    int dsp_steps;            /* last non-zero microcode step + 1; 0 = stopped */
    uint64_t keyons;          /* diagnostics: key-ons, and how many used each feature (see saturn_report) */
    uint64_t use_eg[6];       /* attack < 31, decay1, decay2, KRS, EGHOLD, LPSLNK */
    uint64_t use_lfo_p, use_lfo_a, use_fm, use_efsdl, use_pcm8, use_loop[4];
    int gain_sfx, gain_music; /* 0-256 (256 = full): sample slots and CD audio, set by the overlay */
} scsp_state;

typedef struct saturn {
    sh2_state cpu[2];         /* 0 = master, 1 = slave */
    bool slave_on;
    uint8_t *wram_l, *wram_h; /* 1 MB each */
    uint8_t *vdp1_vram, *vdp1_fb, *vdp2_vram, *vdp2_cram, *sound_ram, *backup;
    char backup_path[1024];   /* where backup RAM persists; empty = memory only */
    bool backup_dirty;        /* changed since the last save to backup_path */
    int backup_quiet;         /* frames since the last change: saved after a short quiet spell */
    int fb_draw;              /* which 256 KB half of vdp1_fb the VDP1 draws into (the other is displayed) */
    /* Double-density interlace (VDP1 FBCR DIE): each frame draws only the even or odd lines (DIL) at row
     * y/2. fb_field[i] is the parity frame buffer i was drawn for (-1 = progressive); vdp1_field keeps the
     * previous field's lines (parity field_parity, -1 = none) so a full 448-line picture can be shown. */
    int fb_field[2];
    uint8_t *vdp1_field;
    int field_parity;
    int wide_extra;           /* widescreen: extra pixels drawn left and right of a 320/352 picture (0 = off) */
    int render_extra;         /* what the last saturn_render() actually added per side (0 in hi-res/8-bit modes) */
    int fb_xoff[2];           /* the widescreen shift frame buffer i was drawn with: switching 4:3/16:9 while the
                                 game is paused (overlay open) must not move the sprites already drawn */
    bool fb_erase_pending;    /* FBCR erase request (manual erase/write mode) */
    int vdp1_cmds;            /* commands executed in the last draw (diagnostics) */
    uint64_t vdp1_ccmode[8];  /* drawn commands by CMDPMOD colour calculation (diagnostics) */
    uint64_t vdp1_mesh;       /* drawn commands with mesh on (diagnostics) */
    uint64_t feat_first[16];  /* frame + 1 when each not-yet-rendered VDP2 feature was first enabled (diagnostics) */
    uint64_t feat_frames[16]; /* frames it was enabled */
    uint8_t vdp1_regs[0x20], vdp2_regs[0x200];
    uint8_t onchip[2][0x200];
    uint8_t cache_data[2][0x1000];
    uint32_t divu_q[2];       /* DIVU result bookkeeping */
    uint64_t frt_last[2];     /* free-running timer: tick count at the last update */
    uint16_t frc[2];          /* free-running counter value at the last update */
    uint64_t wdt_last[2];     /* watchdog timer: tick count at the last update */
    uint32_t wdt_cnt[2];      /* WTCNT */
    scu_state scu;
    smpc_state smpc;
    cdb_state cdb;
    scsp_state scsp;
    bios_state bios;
    rt_dispatch dispatch;

    /* timing */
    int line;                 /* 0..262 */
    uint64_t frame;
    int cycles_per_line;
    bool pal;                 /* 50 Hz, 313 lines (set from the version at boot) */
    int64_t rtc_start;        /* the SMPC clock at frame 0 (the PC's time when the machine is created); it then
                                 follows emulated frames, so an input replay sees the same date and time */
    bool vblank, hblank;

    /* disc */
    iso_fs *fs;
    const game_version *version;

    /* diagnostics */
    io_stat iolog[IOLOG_SIZE];
    uint32_t pc_samples[1024];
    int npc_samples;
    uint32_t recent_pc[2][32];
    int recent_pos[2];
} saturn;

saturn *saturn_create(void);
void saturn_destroy(saturn *m);

/* Loads IP + 1st read file like the BIOS, installs the BIOS replacement and points the
 * master SH-2 at the 1st read address. */
bool saturn_boot(saturn *m, iso_fs *fs, char *err, size_t errlen);

/* Runs one video frame (263 lines, 313 on PAL). */
void saturn_run_frame(saturn *m);

/* Human-readable report of what the game did: BIOS calls, I/O, CD commands, where it is. */
void saturn_report(saturn *m, FILE *f);

/* Video: renders the current VDP2+VDP1 state to 0xAARRGGBB pixels. `out` must hold 704*512 words;
 * the picture size goes to w and h. */
void saturn_render(saturn *m, uint32_t *out, int *w, int *h);

/* ---- internal (shared between the saturn_*.c files) ------------------------------------------ */
void scu_raise(saturn *m, int bit);
void scu_deliver(saturn *m);
uint32_t scu_read(saturn *m, uint32_t off, int size);
void scu_write(saturn *m, uint32_t off, uint32_t v, int size);
void scu_trigger_dma(saturn *m, int factor);

uint8_t smpc_read(saturn *m, uint32_t off);
void smpc_write(saturn *m, uint32_t off, uint8_t v);

uint16_t cdb_read(saturn *m, uint32_t off);
void cdb_write(saturn *m, uint32_t off, uint16_t v);
void cdb_init(saturn *m);
void cdb_tick(saturn *m);   /* once per frame: delivers sectors while playing */

/* saturn_scsp.c */
void scsp_init(saturn *m);
uint16_t scsp_read16(saturn *m, uint32_t off);
void scsp_write16(saturn *m, uint32_t off, uint16_t v, uint16_t mask);
void scsp_sound_cpu(saturn *m, bool on);   /* SMPC SNDON / SNDOFF */
void scsp_run_line(saturn *m, double lines_per_sec);
/* Lines per frame and frames per second for the machine's TV standard. */
int saturn_lines(const saturn *m);
int saturn_fps(const saturn *m);
/* SMPC clock change / BIOS CHGSYSCK: cycles per line for 320 or 352 dot mode on this standard. */
void saturn_set_clock(saturn *m, bool mode352);

void bios_install(saturn *m);
/* Controller layouts for 1-12 players: where each player's pad goes. */
enum { PORTS_AUTO, PORTS_DIRECT, PORTS_TAP1, PORTS_TAP2, PORTS_TAP_BOTH, PORTS_COUNT };
/* Sets the SMPC ports for `players` pads in layout `mode`; slot[i] gets player i's pad slot (-1 = no
 * room). Returns how many players got a slot. */
int smpc_layout(saturn *m, int players, int mode, int slot[SMPC_SLOTS]);
const char *smpc_layout_name(int mode);
/* Backup RAM persistence: loads path (32 KB raw, or 64 KB with data in the odd bytes) or formats a
 * new image; changes are written back to it. */
bool saturn_backup_attach(saturn *m, const char *path, char *err, size_t errlen);
void saturn_backup_flush(saturn *m);
bool bios_hle(saturn *m, sh2_state *s); /* true if s->pc was a BIOS address and was handled */

void vdp1_draw(saturn *m);          /* run the command list now */
void vdp1_frame_end(saturn *m);     /* V-blank-out: frame buffer swap / erase */
void vdp1_fbcr_write(saturn *m);
uint8_t *vdp1_display_fb(saturn *m);
uint8_t *vdp1_draw_fb(saturn *m);

void io_note(saturn *m, sh2_state *s, uint32_t addr, int write, int size, uint32_t value);

#endif
