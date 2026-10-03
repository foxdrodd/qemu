/*
 * Sega Dreamcast modem - Rockwell/Conexant modem data pump (MDP) on the G2 bus
 *
 * The Dreamcast modem is a bare data pump: 32 byte-wide interface registers,
 * a DSP RAM reached through an indirect window, and 128-byte TX/RX FIFOs.
 * There is no AT command processor; the guest takes the line off-hook, dials
 * and starts the handshake itself.  The register behaviour modelled here is
 * what the KallistiOS driver (kernel/arch/dreamcast/hardware/modem) relies on;
 * it has not been checked against a datasheet.
 *
 * No line signal is modelled.  The "telephone line" is a chardev: once the
 * guest has dialled, the far end answers if the chardev is open, a timed fake
 * handshake runs, and then bytes flow between the FIFOs and the chardev at the
 * negotiated rate.
 *
 * Copyright (c) 2026 Florian Fuchs
 *
 * SPDX-License-Identifier: MIT
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/core/sysbus.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/core/irq.h"
#include "migration/vmstate.h"
#include "chardev/char-fe.h"
#include "hw/sh4/sh.h"
#include "qemu/timer.h"
#include "qemu/module.h"
#include "qom/object.h"
#include "trace.h"

#define TYPE_DC_MODEM "dc-modem"
OBJECT_DECLARE_SIMPLE_TYPE(DCModemState, DC_MODEM)

/* Register file: reg N is at byte offset N*4; the G2 reset is at 0x80. */
#define MDM_REG_STRIDE  4
#define MDM_NREGS       0x20
#define MDM_RESET_OFF   0x80
#define MDM_MMIO_SIZE   0x100

#define R_RBUFFER   0x00
#define R_FIFOSTAT  0x01
#define R_LINE      0x07
#define R_CTRL9     0x09
#define R_STATA     0x0a
#define R_STATB     0x0b
#define R_STATC     0x0c
#define R_STATD     0x0d
#define R_SPEED     0x0e
#define R_STATF     0x0f
#define R_TBUFFER   0x10
#define R_CONF      0x12
#define R_ABCODE    0x14
#define R_CTRL15    0x15
#define R_SECRXB    0x16
#define R_MEDAL     0x18
#define R_MEDAM     0x19
#define R_CTRL1A    0x1a
#define R_MEADDL    0x1c
#define R_MEADDH    0x1d
#define R_IRQ1E     0x1e
#define R_IRQ1F     0x1f

#define FS_RXHF     0x02    /* 01h: RX FIFO half full              */
#define FS_TXHF     0x04    /* 01h: TX FIFO half full              */
#define LINE_RA     0x02    /* 07h: relay A, line off-hook         */
#define C9_DTR      0x01    /* 09h: data terminal ready            */
#define SB_ATV25    0x10    /* 0Bh: answer tone detected           */
#define SB_TONES    0xe0    /* 0Bh: TONEA/B/C, all set = dial tone */
#define SC_RXFNE    0x02    /* 0Ch: RX FIFO not empty              */
#define SD_TXFNF    0x02    /* 0Dh: TX FIFO not full               */
#define SPEED_MASK  0x1f    /* 0Eh: negotiated speed index         */
#define SF_DSR      0x10    /* 0Fh: data set ready                 */
#define SF_CTS      0x20    /* 0Fh: clear to send                  */
#define SF_RLSD     0x80    /* 0Fh: carrier (handshake complete)   */
#define C15_RREN    0x04    /* 15h: rate renegotiation / cleardown */
#define C1A_SFRES   0x80    /* 1Ah: soft reset on next NEWC        */
#define MA_ADDRH    0x0f    /* 1Dh: DSP RAM address bits 8-11      */
#define MA_MEMW     0x20    /* 1Dh: DSP RAM access is a write      */
#define MA_ADDR12   0x40    /* 1Dh: DSP RAM address bit 12         */
#define MA_MEACC    0x80    /* 1Dh: DSP RAM access strobe          */
#define I1E_RDBF    0x01    /* 1Eh: receive data buffer full       */
#define I1E_RDBIE   0x04
#define I1E_TDBE    0x08    /* 1Eh: transmit data buffer empty     */
#define I1E_TDBIE   0x20
#define I1E_RDBIA   0x40
#define I1E_TDBIA   0x80
#define I1F_NEWC    0x01    /* 1Fh: new configuration              */
#define I1F_NEWS    0x08    /* 1Fh: new status                     */
#define I1F_NSIE    0x10
#define I1F_NSIA    0x80

#define CONF_DIAL       0x81    /* DTMF dialling mode        */
#define CONF_V34_CLEAR  0xc0    /* V.34 cleardown            */
#define CONF_V34_33600  0xce
#define SPEED_33600     16      /* index into the bps table  */
#define LINE_BPS        33600
#define ABCODE_NO_PHASE 0x24    /* timed out waiting for the far end */

/* DSP RAM cells the guest uses to gate "new status" interrupts. */
#define DSP_RAM_SIZE    0x2000
#define DSP_MEMACC_CFG  0x089   /* bit 6: no NEWS on DSP RAM access */

#define MDM_FIFO_SIZE   128

/* Timings, all on the virtual clock. */
#define MDM_RESET_MS    20      /* self test until NEWC clears          */
#define MDM_DIGIT_MS    100     /* one DTMF digit                       */
#define MDM_ANSWER_MS   1500    /* last digit until the far end answers */
#define MDM_TRAIN_MS    500     /* one handshake phase                  */
#define MDM_TRAIN_STEPS 6
#define MDM_CLEAR_MS    200     /* cleardown until carrier drops        */
#define MDM_PACE_MS     10      /* data pump tick                       */

enum {
    MDM_HELD,           /* G2 reset asserted                    */
    MDM_SELFTEST,       /* self-test words in the registers     */
    MDM_IDLE,           /* on hook                              */
    MDM_OFFHOOK,        /* off hook, dialling                   */
    MDM_ANSWERED,       /* far end answered, waiting for DTR    */
    MDM_TRAINING,
    MDM_CONNECTED,
    MDM_DROPPED,        /* call over, still off hook            */
};

struct DCModemState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;
    CharFrontend chr;

    uint8_t  reg[MDM_NREGS];
    uint16_t ram[DSP_RAM_SIZE];
    uint8_t  state;
    uint8_t  selftest_reads;    /* reads of TBUFFER during the self test */
    uint8_t  train_step;
    bool     irq_level;
    bool     chr_open;

    uint8_t  rx[MDM_FIFO_SIZE];
    uint8_t  tx[MDM_FIFO_SIZE];
    uint32_t rx_head, rx_len;
    uint32_t tx_head, tx_len;
    uint32_t rx_budget;         /* bytes the line may still deliver this tick */

    QEMUTimer *reset_timer;
    QEMUTimer *digit_timer;
    QEMUTimer *call_timer;      /* answer, training and cleardown delays */
    QEMUTimer *pace_timer;
};

/*
 * Self-test results, low register first.  The controller words sit in
 * 1Dh/1Ch down to 11h/00h, the DSP words in 1Bh/1Ah down to 11h/00h.
 */
static const struct { uint8_t hi, lo; uint16_t val; } mdm_ctrl_sig[] = {
    { 0x1d, 0x1c, 0xea3c }, { 0x1b, 0x1a, 0x5536 }, { 0x19, 0x18, 0x5f4c },
    { 0x17, 0x16, 0x3835 }, { 0x15, 0x14, 0x0801 }, { 0x13, 0x12, 0x3730 },
    { 0x11, 0x00, 0x4241 },
}, mdm_dsp_sig[] = {
    { 0x1b, 0x1a, 0xf083 }, { 0x19, 0x18, 0x46ee }, { 0x17, 0x16, 0x00fa },
    { 0x15, 0x14, 0x0a09 }, { 0x13, 0x12, 0x3730 }, { 0x11, 0x00, 0x2041 },
};

static void mdm_arm(QEMUTimer *t, int ms)
{
    timer_mod(t, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + ms);
}

static void mdm_update_irq(DCModemState *s)
{
    uint8_t e = s->reg[R_IRQ1E] & ~(I1E_RDBIA | I1E_TDBIA);
    uint8_t f = s->reg[R_IRQ1F] & ~I1F_NSIA;
    bool level;

    if ((e & I1E_RDBF) && (e & I1E_RDBIE)) {
        e |= I1E_RDBIA;
    }
    if ((e & I1E_TDBE) && (e & I1E_TDBIE)) {
        e |= I1E_TDBIA;
    }
    if ((f & I1F_NEWS) && (f & I1F_NSIE)) {
        f |= I1F_NSIA;
    }
    s->reg[R_IRQ1E] = e;
    s->reg[R_IRQ1F] = f;

    level = (e & (I1E_RDBIA | I1E_TDBIA)) || (f & I1F_NSIA);
    /* Holly latches the event on a rising edge; pulse when asserting. */
    if (level && !s->irq_level) {
        qemu_set_irq(s->irq, 1);
    }
    s->irq_level = level;
}

/* DSP RAM address of the interrupt mask for a status register, or 0. */
static uint16_t mdm_status_mask(DCModemState *s, int reg)
{
    switch (reg) {
    case R_STATF:    return s->ram[0x241];
    case R_SPEED:    return s->ram[0x242];
    case R_STATD:    return s->ram[0x243];
    case R_STATC:    return s->ram[0x244];
    case R_STATB:    return s->ram[0x245];
    case R_STATA:    return s->ram[0x246];
    case R_FIFOSTAT: return s->ram[0x247];
    case R_SECRXB:   return s->ram[0x370];
    case R_ABCODE:   return s->ram[0x38a];
    default:         return 0;
    }
}

/* Change status bits; an unmasked change raises "new status". */
static void mdm_status(DCModemState *s, int reg, uint8_t bits, uint8_t val)
{
    uint8_t new = (s->reg[reg] & ~bits) | (val & bits);

    if ((s->reg[reg] ^ new) & mdm_status_mask(s, reg)) {
        s->reg[R_IRQ1F] |= I1F_NEWS;
    }
    s->reg[reg] = new;
}

static void mdm_fifo_status(DCModemState *s)
{
    bool tx_room = s->tx_len < MDM_FIFO_SIZE;

    mdm_status(s, R_STATC, SC_RXFNE, s->rx_len ? SC_RXFNE : 0);
    mdm_status(s, R_STATD, SD_TXFNF, tx_room ? SD_TXFNF : 0);
    mdm_status(s, R_FIFOSTAT, FS_RXHF | FS_TXHF,
               (s->rx_len >= MDM_FIFO_SIZE / 2 ? FS_RXHF : 0) |
               (s->tx_len >= MDM_FIFO_SIZE / 2 ? FS_TXHF : 0));
    s->reg[R_IRQ1E] = (s->reg[R_IRQ1E] & ~(I1E_RDBF | I1E_TDBE)) |
                      (s->rx_len ? I1E_RDBF : 0) | (tx_room ? I1E_TDBE : 0);
}

static void mdm_set_state(DCModemState *s, int state)
{
    trace_dc_modem_state(s->state, state);
    s->state = state;
}

/* The call is over: carrier and handshake status go away. */
static void mdm_drop_call(DCModemState *s, int state)
{
    bool was_up = s->state == MDM_TRAINING || s->state == MDM_CONNECTED;

    timer_del(s->digit_timer);
    timer_del(s->call_timer);
    timer_del(s->pace_timer);
    s->rx_head = s->rx_len = s->tx_head = s->tx_len = 0;
    mdm_status(s, R_STATF, SF_RLSD | SF_CTS | SF_DSR, 0);
    mdm_status(s, R_STATB, SB_ATV25 | SB_TONES, 0);
    mdm_status(s, R_SECRXB, 0xff, 0);
    mdm_fifo_status(s);
    mdm_set_state(s, state);
    if (was_up && s->chr_open) {
        qemu_chr_fe_disconnect(&s->chr);    /* hang up on the far end */
    }
}

static void mdm_load_sig(DCModemState *s, bool dsp)
{
    int i;

    if (dsp) {
        for (i = 0; i < ARRAY_SIZE(mdm_dsp_sig); i++) {
            s->reg[mdm_dsp_sig[i].hi] = mdm_dsp_sig[i].val >> 8;
            s->reg[mdm_dsp_sig[i].lo] = mdm_dsp_sig[i].val & 0xff;
        }
    } else {
        for (i = 0; i < ARRAY_SIZE(mdm_ctrl_sig); i++) {
            s->reg[mdm_ctrl_sig[i].hi] = mdm_ctrl_sig[i].val >> 8;
            s->reg[mdm_ctrl_sig[i].lo] = mdm_ctrl_sig[i].val & 0xff;
        }
    }
}

/* Reset the data pump and present the self-test words until it completes. */
static void mdm_start_selftest(DCModemState *s)
{
    timer_del(s->digit_timer);
    timer_del(s->call_timer);
    timer_del(s->pace_timer);
    memset(s->reg, 0, sizeof(s->reg));
    memset(s->ram, 0, sizeof(s->ram));
    s->rx_head = s->rx_len = s->tx_head = s->tx_len = 0;
    s->selftest_reads = 0;
    mdm_load_sig(s, false);
    s->reg[R_IRQ1E] = I1E_TDBE;
    s->reg[R_IRQ1F] = I1F_NEWC;
    mdm_set_state(s, MDM_SELFTEST);
    mdm_update_irq(s);
    mdm_arm(s->reset_timer, MDM_RESET_MS);
}

static void mdm_reset_done(void *opaque)
{
    DCModemState *s = opaque;

    memset(s->reg, 0, sizeof(s->reg));
    s->reg[R_IRQ1E] = I1E_TDBE;
    s->reg[R_STATD] = SD_TXFNF;
    mdm_set_state(s, MDM_IDLE);
    mdm_update_irq(s);
}

static void mdm_digit_done(void *opaque)
{
    DCModemState *s = opaque;

    s->reg[R_IRQ1E] |= I1E_TDBE;
    mdm_arm(s->call_timer, MDM_ANSWER_MS);
    mdm_update_irq(s);
}

static void mdm_connect(DCModemState *s)
{
    s->reg[R_CONF] = CONF_V34_33600;
    mdm_status(s, R_SPEED, SPEED_MASK, SPEED_33600);
    mdm_status(s, R_STATF, SF_RLSD | SF_CTS | SF_DSR,
               SF_RLSD | SF_CTS | SF_DSR);
    mdm_set_state(s, MDM_CONNECTED);
    s->rx_budget = 0;
    mdm_fifo_status(s);
    mdm_arm(s->pace_timer, MDM_PACE_MS);
}

/* Answer, handshake-phase and cleardown delays. */
static void mdm_call_tick(void *opaque)
{
    DCModemState *s = opaque;

    switch (s->state) {
    case MDM_OFFHOOK:
        /* Nobody picks up unless something is listening on the chardev. */
        if (s->chr_open) {
            mdm_status(s, R_STATB, SB_ATV25 | SB_TONES, SB_ATV25);
            mdm_set_state(s, MDM_ANSWERED);
        }
        break;
    case MDM_TRAINING:
        if (!s->chr_open) {
            mdm_status(s, R_ABCODE, 0xff, ABCODE_NO_PHASE);
            mdm_drop_call(s, MDM_DROPPED);
        } else if (++s->train_step > MDM_TRAIN_STEPS) {
            mdm_connect(s);
        } else {
            /* SECRXB: handshake phase in bits 7-5, phase state in 4-0. */
            mdm_status(s, R_SECRXB, 0xff,
                       ((s->train_step + 1) / 2) << 5 | s->train_step);
            mdm_arm(s->call_timer, MDM_TRAIN_MS);
        }
        break;
    case MDM_CONNECTED:         /* cleardown finished */
        mdm_drop_call(s, MDM_DROPPED);
        break;
    }
    mdm_update_irq(s);
}

/* Move data between the FIFOs and the line at the negotiated rate. */
static void mdm_pace_tick(void *opaque)
{
    DCModemState *s = opaque;
    uint32_t quota = LINE_BPS / 10 * MDM_PACE_MS / 1000;
    uint8_t buf[MDM_FIFO_SIZE];
    uint32_t n = MIN(quota, s->tx_len), i;

    if (s->state != MDM_CONNECTED) {
        return;
    }
    for (i = 0; i < n; i++) {
        buf[i] = s->tx[(s->tx_head + i) % MDM_FIFO_SIZE];
    }
    if (n) {
        s->tx_head = (s->tx_head + n) % MDM_FIFO_SIZE;
        s->tx_len -= n;
        qemu_chr_fe_write_all(&s->chr, buf, n);
    }
    s->rx_budget = quota;
    mdm_fifo_status(s);
    mdm_update_irq(s);
    qemu_chr_fe_accept_input(&s->chr);
    mdm_arm(s->pace_timer, MDM_PACE_MS);
}

/* Re-evaluate the call after a write to a line-control register. */
static void mdm_line_update(DCModemState *s)
{
    bool offhook = s->reg[R_LINE] & LINE_RA;
    bool dtr = s->reg[R_CTRL9] & C9_DTR;

    if (s->state < MDM_IDLE) {
        return;
    }
    if (!offhook) {
        if (s->state != MDM_IDLE) {
            mdm_drop_call(s, MDM_IDLE);
        }
        return;
    }
    switch (s->state) {
    case MDM_IDLE:
        mdm_set_state(s, MDM_OFFHOOK);
        if (s->chr_open) {      /* the far end supplies a dial tone */
            mdm_status(s, R_STATB, SB_TONES, SB_TONES);
        }
        break;
    case MDM_ANSWERED:
        if (dtr) {
            s->train_step = 0;
            mdm_set_state(s, MDM_TRAINING);
            mdm_arm(s->call_timer, MDM_TRAIN_MS);
        }
        break;
    case MDM_TRAINING:
        if (!dtr) {
            mdm_drop_call(s, MDM_DROPPED);
        }
        break;
    case MDM_CONNECTED:
        if (!dtr) {
            mdm_drop_call(s, MDM_DROPPED);
        } else if ((s->reg[R_CTRL15] & C15_RREN) &&
                   s->reg[R_CONF] == CONF_V34_CLEAR &&
                   !timer_pending(s->call_timer)) {
            mdm_arm(s->call_timer, MDM_CLEAR_MS);
        }
        break;
    }
}

/* MEACC strobe: one DSP RAM cell is read into, or written from, MEDAL/MEDAM. */
static void mdm_dsp_access(DCModemState *s)
{
    uint8_t ah = s->reg[R_MEADDH];
    uint16_t addr = s->reg[R_MEADDL] | (ah & MA_ADDRH) << 8 |
                    (ah & MA_ADDR12 ? 0x1000 : 0);

    if (ah & MA_MEMW) {
        s->ram[addr] = s->reg[R_MEDAL] | s->reg[R_MEDAM] << 8;
    } else {
        s->reg[R_MEDAL] = s->ram[addr] & 0xff;
        s->reg[R_MEDAM] = s->ram[addr] >> 8;
    }
    s->reg[R_MEADDH] &= ~MA_MEACC;
    if (!(s->ram[DSP_MEMACC_CFG] & 0x40)) {
        s->reg[R_IRQ1F] |= I1F_NEWS;
    }
}

static uint64_t mdm_read(void *opaque, hwaddr addr, unsigned int size)
{
    DCModemState *s = opaque;
    int reg = addr / MDM_REG_STRIDE;
    uint8_t val;

    if (addr == MDM_RESET_OFF || reg >= MDM_NREGS || s->state == MDM_HELD) {
        return 0;
    }
    val = s->reg[reg];

    if (s->state == MDM_SELFTEST) {
        /*
         * KallistiOS reads the controller words twice, with a read of
         * TBUFFER after each pass, and only then expects the DSP words.
         */
        if (reg == R_TBUFFER && ++s->selftest_reads == 2) {
            mdm_load_sig(s, true);
        }
    } else if (reg == R_RBUFFER && s->rx_len) {
        val = s->rx[s->rx_head];
        s->rx_head = (s->rx_head + 1) % MDM_FIFO_SIZE;
        s->rx_len--;
        mdm_fifo_status(s);
        mdm_update_irq(s);
    }
    trace_dc_modem_read(reg, val);
    return val;
}

static void mdm_write(void *opaque, hwaddr addr, uint64_t val64,
                      unsigned int size)
{
    DCModemState *s = opaque;
    int reg = addr / MDM_REG_STRIDE;
    uint8_t val = val64;

    if (addr == MDM_RESET_OFF) {        /* G2 slot reset */
        if (!(val & 1)) {
            timer_del(s->reset_timer);
            mdm_drop_call(s, MDM_HELD);
            memset(s->reg, 0, sizeof(s->reg));
            mdm_update_irq(s);
        } else if (s->state == MDM_HELD) {
            mdm_start_selftest(s);
        }
        return;
    }
    if (reg >= MDM_NREGS || s->state == MDM_HELD) {
        return;
    }
    trace_dc_modem_write(reg, val);

    if (s->state == MDM_SELFTEST) {
        /* Only a repeated reset request is taken while the test runs. */
        if (reg == R_CTRL1A) {
            s->reg[reg] |= val & C1A_SFRES;
        }
        return;
    }

    switch (reg) {
    case R_RBUFFER:
    case R_FIFOSTAT:
    case R_STATC:
    case R_STATD:
    case R_STATF:
        break;                          /* read-only status */
    case R_TBUFFER:
        s->reg[reg] = val;
        if (s->state == MDM_CONNECTED) {
            if (s->tx_len < MDM_FIFO_SIZE) {
                s->tx[(s->tx_head + s->tx_len++) % MDM_FIFO_SIZE] = val;
            }
            mdm_fifo_status(s);
        } else if (s->state == MDM_OFFHOOK && s->reg[R_CONF] == CONF_DIAL) {
            /* A DTMF digit: the tone is sent, then TDBE asks for the next. */
            trace_dc_modem_dial(val);
            s->reg[R_IRQ1E] &= ~I1E_TDBE;
            mdm_status(s, R_STATB, SB_TONES, 0);
            timer_del(s->call_timer);
            mdm_arm(s->digit_timer, MDM_DIGIT_MS);
        }
        break;
    case R_MEADDH:
        s->reg[reg] = val;
        if (val & MA_MEACC) {
            mdm_dsp_access(s);
        }
        break;
    case R_IRQ1E:
        s->reg[reg] = (s->reg[reg] & ~(I1E_RDBIE | I1E_TDBIE)) |
                      (val & (I1E_RDBIE | I1E_TDBIE));
        break;
    case R_IRQ1F:
        s->reg[reg] = val & (I1F_NEWS | I1F_NSIE);
        if (val & I1F_NEWC) {
            if (s->reg[R_CTRL1A] & C1A_SFRES) {
                mdm_start_selftest(s);
                return;
            }
            mdm_line_update(s);         /* new configuration takes effect */
        }
        break;
    case R_LINE:
    case R_CTRL9:
    case R_CTRL15:
        s->reg[reg] = val;
        mdm_line_update(s);
        break;
    default:
        s->reg[reg] = val;
        break;
    }
    mdm_update_irq(s);
}

static const MemoryRegionOps mdm_ops = {
    .read = mdm_read,
    .write = mdm_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 1,
    .impl.max_access_size = 1,
};

static int mdm_can_receive(void *opaque)
{
    DCModemState *s = opaque;

    if (s->state != MDM_CONNECTED) {
        return 0;
    }
    return MIN(MDM_FIFO_SIZE - s->rx_len, s->rx_budget);
}

static void mdm_receive(void *opaque, const uint8_t *buf, int size)
{
    DCModemState *s = opaque;
    int i;

    for (i = 0; i < size && s->rx_len < MDM_FIFO_SIZE; i++) {
        s->rx[(s->rx_head + s->rx_len++) % MDM_FIFO_SIZE] = buf[i];
    }
    s->rx_budget -= MIN(s->rx_budget, size);
    mdm_fifo_status(s);
    mdm_update_irq(s);
}

static void mdm_event(void *opaque, QEMUChrEvent event)
{
    DCModemState *s = opaque;

    switch (event) {
    case CHR_EVENT_OPENED:
        s->chr_open = true;
        break;
    case CHR_EVENT_CLOSED:
        s->chr_open = false;
        if (s->state == MDM_CONNECTED) {    /* the far end hung up */
            mdm_drop_call(s, MDM_DROPPED);
            mdm_update_irq(s);
        }
        break;
    default:
        break;
    }
}

static void mdm_reset_hold(Object *obj, ResetType type)
{
    DCModemState *s = DC_MODEM(obj);

    timer_del(s->reset_timer);
    timer_del(s->digit_timer);
    timer_del(s->call_timer);
    timer_del(s->pace_timer);
    s->irq_level = false;
    s->state = MDM_HELD;
    mdm_start_selftest(s);
}

static void mdm_realize(DeviceState *dev, Error **errp)
{
    DCModemState *s = DC_MODEM(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &mdm_ops, s,
                          "dc-modem", MDM_MMIO_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);

    s->reset_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, mdm_reset_done, s);
    s->digit_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, mdm_digit_done, s);
    s->call_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, mdm_call_tick, s);
    s->pace_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, mdm_pace_tick, s);

    s->chr_open = qemu_chr_fe_backend_open(&s->chr);
    qemu_chr_fe_set_handlers(&s->chr, mdm_can_receive, mdm_receive,
                             mdm_event, NULL, s, NULL, true);
}

static const VMStateDescription vmstate_mdm = {
    .name = "dc-modem",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(reg, DCModemState, MDM_NREGS),
        VMSTATE_UINT16_ARRAY(ram, DCModemState, DSP_RAM_SIZE),
        VMSTATE_UINT8(state, DCModemState),
        VMSTATE_UINT8(selftest_reads, DCModemState),
        VMSTATE_UINT8(train_step, DCModemState),
        VMSTATE_BOOL(irq_level, DCModemState),
        VMSTATE_UINT8_ARRAY(rx, DCModemState, MDM_FIFO_SIZE),
        VMSTATE_UINT8_ARRAY(tx, DCModemState, MDM_FIFO_SIZE),
        VMSTATE_UINT32(rx_head, DCModemState),
        VMSTATE_UINT32(rx_len, DCModemState),
        VMSTATE_UINT32(tx_head, DCModemState),
        VMSTATE_UINT32(tx_len, DCModemState),
        VMSTATE_TIMER_PTR(reset_timer, DCModemState),
        VMSTATE_TIMER_PTR(digit_timer, DCModemState),
        VMSTATE_TIMER_PTR(call_timer, DCModemState),
        VMSTATE_TIMER_PTR(pace_timer, DCModemState),
        VMSTATE_END_OF_LIST()
    }
};

static const Property mdm_properties[] = {
    DEFINE_PROP_CHR("chardev", DCModemState, chr),
};

static void mdm_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->realize = mdm_realize;
    rc->phases.hold = mdm_reset_hold;
    dc->vmsd = &vmstate_mdm;
    device_class_set_props(dc, mdm_properties);
}

static const TypeInfo mdm_info = {
    .name          = TYPE_DC_MODEM,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(DCModemState),
    .class_init    = mdm_class_init,
};

static void mdm_register_types(void)
{
    type_register_static(&mdm_info);
}

type_init(mdm_register_types)

/* Board helper: create the modem on a chardev "line", map and wire it. */
void dc_modem_init(hwaddr base, qemu_irq irq, Chardev *chr)
{
    DeviceState *dev;
    SysBusDevice *sbd;

    dev = qdev_new(TYPE_DC_MODEM);
    qdev_prop_set_chr(dev, "chardev", chr);
    sbd = SYS_BUS_DEVICE(dev);
    sysbus_realize_and_unref(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, base);
    sysbus_connect_irq(sbd, 0, irq);
}
