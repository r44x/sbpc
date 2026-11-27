/* SMPC: system commands (slave/sound/CD on-off, clock change, reset) and INTBACK
 * (status + peripheral data). Registers sit on odd byte addresses 0x20100001-0x2010007F. */
#include "core/log.h"
#include "saturn/saturn.h"
#include "sh2/sh2_interp.h"

#include <string.h>
#include <time.h>

enum {
    CMD_MSHON = 0x00, CMD_SSHON = 0x02, CMD_SSHOFF = 0x03, CMD_SNDON = 0x06, CMD_SNDOFF = 0x07,
    CMD_CDON = 0x08, CMD_CDOFF = 0x09, CMD_SYSRES = 0x0D, CMD_CKCHG352 = 0x0E, CMD_CKCHG320 = 0x0F,
    CMD_INTBACK = 0x10, CMD_SETTIME = 0x16, CMD_SETSMEM = 0x17, CMD_NMIREQ = 0x18,
    CMD_RESENAB = 0x19, CMD_RESDISA = 0x1A, CMD_NETLINKON = 0x0A, CMD_NETLINKOFF = 0x0B
};

static uint8_t bcd(int v) { return (uint8_t)((v / 10) << 4 | (v % 10)); }

/* Area code for the SMPC from the disc's region letter. */
static uint8_t area_code(saturn *m)
{
    const char *id = m->version ? m->version->id : "usa";
    if (id[0] == 'j') return 0x1;
    if (id[0] == 'e') return 0xC;
    return 0x4; /* North America */
}

/* Peripheral report for both ports. Per port: 0xF0 = nothing, 0xF1 + a device = one direct pad, or
 * 0x16 (multitap ID 1, 6 connectors) + 6 devices. A device is 0x02 (standard pad, 2 data bytes) +
 * the active-low buttons, or 0xFF for an empty connector. Port 2 is empty unless a layout says
 * otherwise: an always-present second pad made Battle Game's character select wait for a player 2
 * who never came. Two multitaps make 38 bytes, more than the 32 OREGs: see deliver_pads(). */
static int build_pads(saturn *m, uint8_t *out)
{
    smpc_state *p = &m->smpc;
    int n = 0;
    for (int port = 0; port < 2; port++) {
        const uint16_t *pad = &p->pad[port * 6];
        switch (p->port_mode[port]) {
        case SMPC_PORT_PAD:
            out[n++] = 0xF1;
            out[n++] = 0x02;
            out[n++] = (uint8_t)(pad[0] >> 8);
            out[n++] = (uint8_t)pad[0];
            break;
        case SMPC_PORT_TAP:
            out[n++] = 0x16;
            for (int c = 0; c < 6; c++) {
                if (c >= p->tap_pads[port]) { out[n++] = 0xFF; continue; }
                out[n++] = 0x02;
                out[n++] = (uint8_t)(pad[c] >> 8);
                out[n++] = (uint8_t)pad[c];
            }
            break;
        default:
            out[n++] = 0xF0;
            break;
        }
    }
    return n;
}

/* Hands out the next 32 bytes of the peripheral report. SR: bit 7 = peripheral data, bit 6 = first
 * block, bit 5 = more follows (the game asks with INTBACK "continue", IREG0 bit 7). */
static void deliver_pads(saturn *m, bool first)
{
    smpc_state *p = &m->smpc;
    int left, take;
    if (first) {
        p->per_len = build_pads(m, p->per);
        p->per_pos = 0;
    }
    left = p->per_len - p->per_pos;
    take = left > 32 ? 32 : left;
    memset(p->oreg, 0, sizeof p->oreg);
    memcpy(p->oreg, p->per + p->per_pos, (size_t)take);
    p->per_pos += take;
    p->sr = (uint8_t)(0x80 | (first ? 0x40 : 0) | (p->per_pos < p->per_len ? 0x20 : 0));
    scu_raise(m, 7);
}

const char *smpc_layout_name(int mode)
{
    static const char *const N[PORTS_COUNT] = {"Auto", "Direct", "Multitap 1", "Multitap 2", "Two multitaps"};
    return mode >= 0 && mode < PORTS_COUNT ? N[mode] : "?";
}

int smpc_layout(saturn *m, int players, int mode, int slot[SMPC_SLOTS])
{
    smpc_state *p = &m->smpc;
    int placed = 0;
    if (players < 1) players = 1;
    if (players > SMPC_SLOTS) players = SMPC_SLOTS;
    /* Auto: direct pads for 1-2 players; player 1 direct plus a multitap on port 2 for 3-7 (the usual
     * Saturn set-up); a multitap on each port for 8 or more. */
    if (mode == PORTS_AUTO) mode = players <= 2 ? PORTS_DIRECT : players <= 7 ? PORTS_TAP2 : PORTS_TAP_BOTH;
    for (int i = 0; i < SMPC_SLOTS; i++) slot[i] = -1;
    p->tap_pads[0] = p->tap_pads[1] = 0;
    switch (mode) {
    case PORTS_TAP1:
        p->port_mode[0] = SMPC_PORT_TAP;
        p->tap_pads[0] = (uint8_t)(players < 6 ? players : 6);
        for (int i = 0; i < p->tap_pads[0]; i++) slot[placed++] = i;
        p->port_mode[1] = players > 6 ? SMPC_PORT_PAD : SMPC_PORT_NONE;
        if (players > 6) slot[placed++] = 6;
        break;
    case PORTS_TAP2:
        p->port_mode[0] = SMPC_PORT_PAD;
        slot[placed++] = 0;
        p->port_mode[1] = SMPC_PORT_TAP;
        p->tap_pads[1] = (uint8_t)(players - 1 < 6 ? players - 1 : 6);
        for (int i = 0; i < p->tap_pads[1]; i++) slot[placed++] = 6 + i;
        break;
    case PORTS_TAP_BOTH:
        p->port_mode[0] = p->port_mode[1] = SMPC_PORT_TAP;
        p->tap_pads[0] = (uint8_t)(players < 6 ? players : 6);
        p->tap_pads[1] = (uint8_t)(players > 6 ? players - 6 : 0);
        for (int i = 0; i < p->tap_pads[0]; i++) slot[placed++] = i;
        for (int i = 0; i < p->tap_pads[1]; i++) slot[placed++] = 6 + i;
        break;
    default:   /* direct */
        p->port_mode[0] = SMPC_PORT_PAD;
        slot[placed++] = 0;
        p->port_mode[1] = players >= 2 ? SMPC_PORT_PAD : SMPC_PORT_NONE;
        if (players >= 2) slot[placed++] = 6;
        break;
    }
    return placed;
}

static void intback(saturn *m)
{
    smpc_state *p = &m->smpc;
    bool want_status = p->ireg[0] & 1;
    bool want_pads = (p->ireg[1] & 0x08) != 0;
    if (want_status) {
        time_t t = (time_t)(m->rtc_start + (int64_t)(m->frame / (m->pal ? 50u : 60u)));
        struct tm tmv;
#ifdef _WIN32
        localtime_s(&tmv, &t);
#else
        localtime_r(&t, &tmv);
#endif
        p->oreg[0] = 0x80;                       /* STE: time set, reset button disabled bit clear */
        p->oreg[1] = bcd((tmv.tm_year + 1900) / 100);
        p->oreg[2] = bcd((tmv.tm_year + 1900) % 100);
        p->oreg[3] = (uint8_t)((tmv.tm_wday << 4) | (tmv.tm_mon + 1));
        p->oreg[4] = bcd(tmv.tm_mday);
        p->oreg[5] = bcd(tmv.tm_hour);
        p->oreg[6] = bcd(tmv.tm_min);
        p->oreg[7] = bcd(tmv.tm_sec);
        p->oreg[8] = 0;                          /* cartridge code */
        p->oreg[9] = area_code(m);
        p->oreg[10] = 0x3E;                      /* system status: DOTSEL etc. */
        p->oreg[11] = 0;
        for (int i = 12; i < 16; i++) p->oreg[i] = 0; /* SMEM */
        /* SR bit 7 = 0: this is status data; bit 6 = 1; NPE (bit 5): peripheral data follows on "continue" */
        p->sr = 0x40 | (want_pads ? 0x20 : 0);
        LOGT(SUB_SMPC, "INTBACK status%s", want_pads ? " + peripherals pending" : "");
        scu_raise(m, 7);
    } else if (want_pads) {
        deliver_pads(m, true);
    } else {
        scu_raise(m, 7);
    }
}

static void command(saturn *m, uint8_t cmd)
{
    smpc_state *p = &m->smpc;
    if (cmd < 32)
        p->commands[cmd]++;
    switch (cmd) {
    case CMD_SSHON:
        m->slave_on = true;
        sh2_reset_state(&m->cpu[1]);
        m->cpu[1].vbr = 0x06000000u;
        m->cpu[1].sr = 0x0F0;
        m->cpu[1].r[15] = 0x06001000u;
        /* The slave BIOS waits for the master to give it work; with no BIOS we park it on a
         * trap that the BIOS replacement turns into "wait for an entry point". */
        m->cpu[1].pc = BIOS_TRAP_SLOT + 0x3F0;
        {   /* The slave BIOS jumps to the routine the game registered with SETSINT(0x94, entry). */
            uint32_t entry = sh2_rd32(&m->cpu[0], 0x06000250u);
            if (entry >= 0x06000000u && entry < 0x06100000u) {
                m->cpu[1].pc = entry;
                m->cpu[1].pr = BIOS_TRAP_SLOT + 0x3F0;
            }
        }
        LOGI(SUB_SMPC, "SSHON: slave SH-2 started (pc %08X)", (unsigned)m->cpu[0].pc);
        break;
    case CMD_SSHOFF:
        m->slave_on = false;
        LOGI(SUB_SMPC, "SSHOFF");
        break;
    case CMD_INTBACK:
        intback(m);
        break;
    case CMD_CKCHG352:
    case CMD_CKCHG320:
        saturn_set_clock(m, cmd == CMD_CKCHG352);
        LOGI(SUB_SMPC, "clock change to %s mode", cmd == CMD_CKCHG352 ? "352" : "320");
        break;
    case CMD_SNDON: case CMD_SNDOFF:
        scsp_sound_cpu(m, cmd == CMD_SNDON);
        break;
    case CMD_CDON: case CMD_CDOFF:
    case CMD_NETLINKON: case CMD_NETLINKOFF: /* NetLink modem power: there is no modem */
    case CMD_SETTIME: case CMD_SETSMEM: case CMD_RESENAB: case CMD_RESDISA: case CMD_MSHON:
        LOGD(SUB_SMPC, "command %02X", cmd);
        break;
    default:
        LOGW(SUB_SMPC, "unhandled command %02X", cmd);
        break;
    }
    if (!(cmd == CMD_INTBACK && (p->sr & 0x80)))   /* peripheral data uses all 32 OREGs */
        p->oreg[31] = cmd;
    p->sf = 0;
}

uint8_t smpc_read(saturn *m, uint32_t off)
{
    smpc_state *p = &m->smpc;
    off &= 0x7F;
    if (off >= 0x21 && off <= 0x5F && (off & 1)) return p->oreg[(off - 0x21) / 2];
    switch (off) {
    case 0x61: LOGT(SUB_SMPC, "read SR = %02X", (unsigned)p->sr); return p->sr;
    case 0x63: return p->sf;
    case 0x75: return p->pdr[0];
    case 0x77: return p->pdr[1];
    default: return 0;
    }
}

void smpc_write(saturn *m, uint32_t off, uint8_t v)
{
    smpc_state *p = &m->smpc;
    off &= 0x7F;
    LOGT(SUB_SMPC, "write %02X = %02X (frame %llu line %d)", (unsigned)off, (unsigned)v, (unsigned long long)m->frame, m->line);
    if (off >= 0x01 && off <= 0x0D && (off & 1)) {
        uint8_t was = p->ireg[0];
        p->ireg[(off - 1) / 2] = v;
        /* INTBACK "continue": IREG0 bit 7 *toggles* while data is pending (the USA kernel writes 0x80,
         * then 0x00 for the next block). After the status block the peripheral report starts; after
         * a peripheral block its next 32 bytes follow. */
        if (off == 0x01 && ((v ^ was) & 0x80) && (p->sr & 0x20)) {
            deliver_pads(m, !(p->sr & 0x80));
        } else if (off == 0x01 && (v & 0x40)) {
            p->sr &= (uint8_t)~0x20; /* break */
        }
        return;
    }
    switch (off) {
    case 0x1F: command(m, v); break;
    case 0x63: p->sf = v; break;
    case 0x75: p->pdr[0] = v; break;
    case 0x77: p->pdr[1] = v; break;
    case 0x79: p->ddr[0] = v; break;
    case 0x7B: p->ddr[1] = v; break;
    case 0x7D: p->iosel = v; break;
    case 0x7F: p->exle = v; break;
    default: break;
    }
}
