/*
 * Sega Dreamcast LAN Adapter (HIT-0300) - Fujitsu MB86967
 *
 * A 10 Mbit Ethernet controller on the Dreamcast G2 expansion bus.  The
 * register and buffer-port model here follows Linux's
 * drivers/net/ethernet/fujitsu/lan_adapter.c exactly.
 *
 * Copyright (c) 2026 Florian Fuchs
 *
 * SPDX-License-Identifier: MIT
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/core/sysbus.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/irq.h"
#include "migration/vmstate.h"
#include "net/net.h"
#include "net/eth.h"
#include "hw/sh4/sh.h"
#include "qemu/module.h"
#include "qom/object.h"

/* Maximum Ethernet frame we accept (header + payload, no FCS). */
#define LA_FRAME_LEN  (ETH_HLEN + ETH_MTU)

#define TYPE_DC_LANADAPTER "dc-lanadapter"
OBJECT_DECLARE_SIMPLE_TYPE(DCLanState, DC_LANADAPTER)

/* Register file: reg N is at byte offset N*4; the G2 reset is at 0x80. */
#define LA_REG_STRIDE   4
#define LA_RESET_OFF    0x80
#define LA_MMIO_SIZE    0x100

/* Data Link Control Registers (0-7, always accessible). */
#define DLCR0  0    /* transmit status        */
#define DLCR1  1    /* receive status         */
#define DLCR2  2    /* transmit int mask      */
#define DLCR3  3    /* receive int mask       */
#define DLCR4  4    /* transmit mode          */
#define DLCR5  5    /* receive mode           */
#define DLCR6  6    /* config 0               */
#define DLCR7  7    /* config 1 / bank select */
/* Bank-switched upper registers 8-15, plus EEPROM regs 16-17. */
#define BMPR8  8    /* buffer memory data port (bank 2)  */
#define BMPR10 10   /* transmit packet count / start     */
#define BMPR16 16   /* EEPROM control                    */
#define BMPR17 17   /* EEPROM data                       */

/* Register bits (subset used by the driver). */
#define D1_PKT_RDY   0x80   /* DLCR1: received packet ready       */
#define D3_PKT_RDY   0x80   /* DLCR3: enable rx packet-ready int  */
#define D5_BUF_EMP   0x40   /* DLCR5: receive buffer empty        */
#define D5_AM_PROM   0x03   /* DLCR5: promiscuous                 */
#define D6_ENA_DLC   0x80   /* DLCR6: hold DLC in reset           */
#define D7_RBMASK    0x0c   /* DLCR7: register bank select        */
#define B10_TX       0x80   /* BMPR10: start transmission         */
#define B16_SELECT   0x20   /* BMPR16: EEPROM chip select         */
#define B16_CLOCK    0x40   /* BMPR16: EEPROM shift clock         */
#define B17_DATA     0x80   /* BMPR17: EEPROM data bit            */

#define CONTROLLER_ID  0x02 /* MB86967 id, reported in DLCR7[7:6] */

#define LA_RX_BUFSIZE  (64 * 1024)
#define LA_TX_BUFSIZE  2048

struct DCLanState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;
    NICState *nic;
    NICConf conf;

    uint8_t dlcr[8];        /* DLCR0-7            */
    uint8_t node_id[6];     /* bank 0             */
    uint8_t mar[8];         /* bank 1 (multicast) */
    uint8_t bmpr10, bmpr11; /* bank 2 misc        */

    /* Serial EEPROM (holds the MAC), driven via BMPR16/17. */
    uint8_t  ee_datain;     /* last data bit written    */
    uint8_t  ee_clk;        /* last clock level         */
    uint8_t  ee_cs;         /* last chip-select level   */
    uint8_t  ee_dout;       /* current data-out bit     */
    int      ee_count;      /* clocks since CS asserted */
    uint16_t ee_cmd;        /* accumulated command bits */
    uint16_t ee_word;       /* 16-bit word being shifted out */

    /* Transmit assembly buffer (length header + frame). */
    uint8_t  tx_buf[LA_TX_BUFSIZE];
    uint32_t tx_len;

    /* Receive FIFO: framed [status, 0, len_lo, len_hi, data...]. */
    uint8_t  rx_buf[LA_RX_BUFSIZE];
    uint32_t rx_wlen;
    uint32_t rx_rpos;
};

static int la_bank(DCLanState *s)
{
    return (s->dlcr[DLCR7] & D7_RBMASK) >> 2;
}

static bool la_dlc_running(DCLanState *s)
{
    return !(s->dlcr[DLCR6] & D6_ENA_DLC);
}

static bool la_rx_empty(DCLanState *s)
{
    return s->rx_rpos >= s->rx_wlen;
}

static void la_update_irq(DCLanState *s)
{
    bool rx = (s->dlcr[DLCR1] & D1_PKT_RDY) && (s->dlcr[DLCR3] & D3_PKT_RDY);

    /* Holly latches the event on a rising edge; pulse when asserting. */
    if (rx) {
        qemu_set_irq(s->irq, 1);
    }
}

/* --- serial EEPROM: return the MAC as three 16-bit big-endian words --- */
static void la_eeprom_clock(DCLanState *s, uint8_t bmpr16)
{
    uint8_t cs = bmpr16 & B16_SELECT;
    uint8_t clk = bmpr16 & B16_CLOCK;

    if (!cs) {                      /* deselected: reset the state machine */
        s->ee_count = 0;
        s->ee_cmd = 0;
        s->ee_dout = 0;
    } else if (clk && !s->ee_clk) { /* rising clock edge */
        s->ee_count++;
        if (s->ee_count >= 2 && s->ee_count <= 9) {
            /* 8 command bits after the start bit (MSB first). */
            s->ee_cmd = (s->ee_cmd << 1) | (s->ee_datain ? 1 : 0);
            if (s->ee_count == 9) {
                int addr = s->ee_cmd & 0x3f;    /* READ opcode + 6-bit addr */
                const uint8_t *m = s->conf.macaddr.a;
                s->ee_word = (m[2 * addr] << 8) | m[2 * addr + 1];
            }
        } else if (s->ee_count >= 10 && s->ee_count <= 25) {
            /* Shift out 16 data bits, MSB first, no leading dummy. */
            s->ee_dout = (s->ee_word >> (25 - s->ee_count)) & 1;
        }
    }
    s->ee_cs = cs;
    s->ee_clk = clk;
}

static void la_transmit(DCLanState *s)
{
    uint32_t len;

    if (s->tx_len < 2) {
        s->tx_len = 0;
        return;
    }
    len = s->tx_buf[0] | (s->tx_buf[1] << 8);
    if (len > 0 && len + 2 <= s->tx_len) {
        qemu_send_packet(qemu_get_queue(s->nic), s->tx_buf + 2, len);
    }
    s->tx_len = 0;
}

static uint64_t la_read(void *opaque, hwaddr addr, unsigned int size)
{
    DCLanState *s = opaque;
    int reg;

    if (addr == LA_RESET_OFF) {
        return 0;
    }
    reg = addr >> 2;

    switch (reg) {
    case DLCR0:
        return 0;                       /* no transmit interrupt modelled */
    case DLCR1:
    case DLCR2:
    case DLCR3:
    case DLCR4:
    case DLCR6:
        return s->dlcr[reg];
    case DLCR5:
        return (s->dlcr[DLCR5] & ~D5_BUF_EMP) |
               (la_rx_empty(s) ? D5_BUF_EMP : 0);
    case DLCR7:
        /* Controller id is hard-wired in the top two bits. */
        return (s->dlcr[DLCR7] & 0x3f) | (CONTROLLER_ID << 6);
    case BMPR16:
        return 0;
    case BMPR17:
        return s->ee_dout ? B17_DATA : 0;
    default:
        break;
    }

    /* Bank-switched region 8-15. */
    if (reg >= 8 && reg <= 15) {
        switch (la_bank(s)) {
        case 0:
            return reg <= 13 ? s->node_id[reg - 8] : 0;
        case 1:
            return s->mar[reg - 8];
        case 2:
            if (reg == BMPR8) {         /* drain the receive FIFO */
                uint8_t b = 0;
                if (!la_rx_empty(s)) {
                    b = s->rx_buf[s->rx_rpos++];
                    if (la_rx_empty(s)) {
                        s->rx_rpos = s->rx_wlen = 0;
                    }
                }
                return b;
            }
            if (reg == BMPR10) {
                return 0;               /* transmit buffer always free */
            }
            return reg == 11 ? s->bmpr11 : 0;
        }
    }
    return 0;
}

static void la_write(void *opaque, hwaddr addr, uint64_t val,
                     unsigned int size)
{
    DCLanState *s = opaque;
    int reg;
    uint8_t v = val & 0xff;

    if (addr == LA_RESET_OFF) {         /* G2 slot reset */
        s->ee_count = 0;
        s->tx_len = 0;
        s->rx_rpos = s->rx_wlen = 0;
        return;
    }
    reg = addr >> 2;

    switch (reg) {
    case DLCR0:
        return;                         /* write-1-to-clear tx status */
    case DLCR1:
        s->dlcr[DLCR1] &= ~v;           /* ack received-packet status */
        return;
    case DLCR2:
    case DLCR3:
    case DLCR4:
    case DLCR5:
    case DLCR6:
    case DLCR7:
        s->dlcr[reg] = v;
        return;
    case BMPR16:
        la_eeprom_clock(s, v);
        return;
    case BMPR17:
        s->ee_datain = v & B17_DATA;
        return;
    default:
        break;
    }

    if (reg >= 8 && reg <= 15) {
        switch (la_bank(s)) {
        case 0:
            if (reg <= 13) {
                s->node_id[reg - 8] = v;
            }
            return;
        case 1:
            s->mar[reg - 8] = v;
            return;
        case 2:
            if (reg == BMPR8) {         /* append to the transmit frame */
                if (s->tx_len < LA_TX_BUFSIZE) {
                    s->tx_buf[s->tx_len++] = v;
                }
            } else if (reg == BMPR10) {
                s->bmpr10 = v;
                if (v & B10_TX) {
                    la_transmit(s);
                }
            } else if (reg == 11) {
                s->bmpr11 = v;
            }
            return;
        }
    }
}

static const MemoryRegionOps la_ops = {
    .read = la_read,
    .write = la_write,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 1,
    .impl.max_access_size = 1,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static bool la_can_receive(NetClientState *nc)
{
    DCLanState *s = qemu_get_nic_opaque(nc);

    return la_dlc_running(s) &&
           s->rx_wlen + 4 + LA_FRAME_LEN <= LA_RX_BUFSIZE;
}

static ssize_t la_receive(NetClientState *nc, const uint8_t *buf, size_t size)
{
    DCLanState *s = qemu_get_nic_opaque(nc);
    size_t len = size;

    if (!la_dlc_running(s)) {
        return size;                    /* silently drop while down */
    }
    if (len > LA_FRAME_LEN) {
        len = LA_FRAME_LEN;
    }
    if (s->rx_wlen + 4 + len > LA_RX_BUFSIZE) {
        return 0;                       /* no room: apply backpressure */
    }

    /* Frame header: 2 status bytes (0x20 = good) then the 16-bit length. */
    s->rx_buf[s->rx_wlen++] = 0x20;
    s->rx_buf[s->rx_wlen++] = 0x00;
    s->rx_buf[s->rx_wlen++] = len & 0xff;
    s->rx_buf[s->rx_wlen++] = (len >> 8) & 0xff;
    memcpy(s->rx_buf + s->rx_wlen, buf, len);
    s->rx_wlen += len;

    s->dlcr[DLCR1] |= D1_PKT_RDY;
    la_update_irq(s);
    return size;
}

static NetClientInfo net_la_info = {
    .type = NET_CLIENT_DRIVER_NIC,
    .size = sizeof(NICState),
    .can_receive = la_can_receive,
    .receive = la_receive,
};

static void la_reset_hold(Object *obj, ResetType type)
{
    DCLanState *s = DC_LANADAPTER(obj);

    memset(s->dlcr, 0, sizeof(s->dlcr));
    s->dlcr[DLCR6] = D6_ENA_DLC;        /* DLC starts held in reset */
    s->tx_len = 0;
    s->rx_rpos = s->rx_wlen = 0;
    s->ee_count = 0;
    s->ee_cmd = s->ee_word = 0;
    s->ee_dout = s->ee_datain = s->ee_clk = s->ee_cs = 0;
}

static void la_realize(DeviceState *dev, Error **errp)
{
    DCLanState *s = DC_LANADAPTER(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &la_ops, s,
                          "dc-lanadapter", LA_MMIO_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);

    qemu_macaddr_default_if_unset(&s->conf.macaddr);
    s->nic = qemu_new_nic(&net_la_info, &s->conf,
                          object_get_typename(OBJECT(dev)), dev->id,
                          &dev->mem_reentrancy_guard, s);
    qemu_format_nic_info_str(qemu_get_queue(s->nic), s->conf.macaddr.a);
}

static const VMStateDescription vmstate_la = {
    .name = "dc-lanadapter",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(dlcr, DCLanState, 8),
        VMSTATE_UINT8_ARRAY(node_id, DCLanState, 6),
        VMSTATE_UINT8_ARRAY(mar, DCLanState, 8),
        VMSTATE_UINT32(tx_len, DCLanState),
        VMSTATE_UINT32(rx_wlen, DCLanState),
        VMSTATE_UINT32(rx_rpos, DCLanState),
        VMSTATE_END_OF_LIST()
    }
};

static const Property la_properties[] = {
    DEFINE_NIC_PROPERTIES(DCLanState, conf),
};

static void la_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->realize = la_realize;
    rc->phases.hold = la_reset_hold;
    dc->vmsd = &vmstate_la;
    device_class_set_props(dc, la_properties);
}

static const TypeInfo la_info = {
    .name          = TYPE_DC_LANADAPTER,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(DCLanState),
    .class_init    = la_class_init,
};

static void la_register_types(void)
{
    type_register_static(&la_info);
}

type_init(la_register_types)

/* Board helper: create the adapter, attach a NIC backend, map and wire it. */
void dc_lanadapter_init(hwaddr base, qemu_irq irq)
{
    DeviceState *dev;
    SysBusDevice *sbd;

    dev = qdev_new(TYPE_DC_LANADAPTER);
    qemu_configure_nic_device(dev, true, NULL);
    sbd = SYS_BUS_DEVICE(dev);
    sysbus_realize_and_unref(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, base);
    sysbus_connect_irq(sbd, 0, irq);
}
