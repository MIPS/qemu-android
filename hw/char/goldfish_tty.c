/* Copyright (C) 2007-2008 The Android Open Source Project
**
** This software is licensed under the terms of the GNU General Public
** License version 2, as published by the Free Software Foundation, and
** may be copied, distributed, and modified under those terms.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
*/
#include "exec/ram_addr.h"
#include "migration/qemu-file.h"
#include "sysemu/char.h"
#include "hw/misc/vmem.h"
#include "hw/hw.h"
#include "exec/address-spaces.h"
#include "hw/sysbus.h"
#include "sysemu/sysemu.h"

enum {
    TTY_PUT_CHAR       = 0x00,
    TTY_BYTES_READY    = 0x04,
    TTY_CMD            = 0x08,

    TTY_DATA_PTR       = 0x10,
    TTY_DATA_LEN       = 0x14,
    TTY_DATA_PTR_HIGH  = 0x18,

    TTY_CMD_INT_DISABLE    = 0,
    TTY_CMD_INT_ENABLE     = 1,
    TTY_CMD_WRITE_BUFFER   = 2,
    TTY_CMD_READ_BUFFER    = 3,
};

struct tty_state {
    SysBusDevice parent;

    MemoryRegion iomem;
    qemu_irq irq;

    CharDriverState *cs;
    uint64_t ptr;
    uint32_t ptr_len;
    uint32_t ready;
    uint8_t data[128];
    uint32_t data_count;
};

#define  GOLDFISH_TTY_SAVE_VERSION  2

#define TYPE_GOLDFISH_TTY "goldfish_tty"
#define GOLDFISH_TTY(obj) OBJECT_CHECK(struct tty_state, (obj), TYPE_GOLDFISH_TTY)

/* Number of instantiated TTYs */
static int  instance_id = 0;

static void goldfish_tty_save(QEMUFile*  f, void*  opaque)
{
    struct tty_state*  s = opaque;

    qemu_put_be64( f, s->ptr );
    qemu_put_be32( f, s->ptr_len );
    qemu_put_byte( f, s->ready );
    qemu_put_byte( f, s->data_count );
    qemu_put_buffer( f, s->data, s->data_count );
}

static int goldfish_tty_load(QEMUFile*  f, void*  opaque, int  version_id)
{
    struct tty_state*  s = opaque;

    if ((version_id != GOLDFISH_TTY_SAVE_VERSION) &&
        (version_id != (GOLDFISH_TTY_SAVE_VERSION - 1))) {
        return -1;
    }
    if (version_id == (GOLDFISH_TTY_SAVE_VERSION - 1)) {
        s->ptr    = (uint64_t)qemu_get_be32(f);
    } else {
        s->ptr    = qemu_get_be64(f);
    }
    s->ptr_len    = qemu_get_be32(f);
    s->ready      = qemu_get_byte(f);
    s->data_count = qemu_get_byte(f);

    if (qemu_get_buffer(f, s->data, s->data_count) < 0)
        return -1;

    qemu_set_irq(s->irq, s->ready && s->data_count > 0);
    return 0;
}

static uint64_t goldfish_tty_read(void *opaque, hwaddr offset, unsigned size)
{
    struct tty_state *s = (struct tty_state *)opaque;

    switch (offset) {
        case TTY_BYTES_READY:
            return s->data_count;
    default:
        cpu_abort(current_cpu,
                  "goldfish_tty_read: Bad offset %" HWADDR_PRIx "\n",
                  offset);
        return 0;
    }
}

static void goldfish_tty_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    struct tty_state *s = (struct tty_state *)opaque;

    switch(offset) {
        case TTY_PUT_CHAR: {
            uint8_t ch = value;
            if(s->cs)
                qemu_chr_fe_write(s->cs, &ch, 1);
        } break;

        case TTY_CMD:
            switch(value) {
                case TTY_CMD_INT_DISABLE:
                    if(s->ready) {
                        if(s->data_count > 0)
                            qemu_set_irq(s->irq, 0);
                        s->ready = 0;
                    }
                    break;

                case TTY_CMD_INT_ENABLE:
                    if(!s->ready) {
                        if(s->data_count > 0)
                            qemu_set_irq(s->irq, 1);
                        s->ready = 1;
                    }
                    break;

                case TTY_CMD_WRITE_BUFFER:
                    if(s->cs) {
                        int len;
                        target_ulong  buf;

                        buf = s->ptr;
                        len = s->ptr_len;

                        while (len) {
                            char   temp[64];
                            int    to_write = sizeof(temp);
                            if (to_write > len)
                                to_write = len;

                            safe_memory_rw_debug(current_cpu, buf, (uint8_t*)temp, to_write, 0);
                            qemu_chr_fe_write(s->cs, (const uint8_t*)temp, to_write);
                            buf += to_write;
                            len -= to_write;
                        }
                    }
                    break;

                case TTY_CMD_READ_BUFFER:
                    if(s->ptr_len > s->data_count)
                        cpu_abort(current_cpu, "goldfish_tty_write: reading more data than available %d %d\n", s->ptr_len, s->data_count);
                    safe_memory_rw_debug(current_cpu, s->ptr, s->data, s->ptr_len,1);
                    if(s->data_count > s->ptr_len)
                        memmove(s->data, s->data + s->ptr_len, s->data_count - s->ptr_len);
                    s->data_count -= s->ptr_len;
                    if(s->data_count == 0 && s->ready)
                        qemu_set_irq(s->irq, 0);
                    break;

                default:
                    cpu_abort(current_cpu, "goldfish_tty_write: Bad command %" PRIx64 "\n", value);
            };
            break;

        case TTY_DATA_PTR:
            s->ptr = deposit64(s->ptr, 0, 32, value);
            break;

        case TTY_DATA_PTR_HIGH:
            goldfish_64bit_guest = 1;
            s->ptr = deposit64(s->ptr, 32, 32, value);
            break;

        case TTY_DATA_LEN:
            s->ptr_len = value;
            break;

        default:
            cpu_abort(current_cpu,
                      "goldfish_tty_write: Bad offset %" HWADDR_PRIx "\n",
                      offset);
    }
}

static int tty_can_receive(void *opaque)
{
    struct tty_state *s = opaque;

    return (sizeof(s->data) - s->data_count);
}

static void tty_receive(void *opaque, const uint8_t *buf, int size)
{
    struct tty_state *s = opaque;

    memcpy(s->data + s->data_count, buf, size);
    s->data_count += size;
    if(s->data_count > 0 && s->ready)
        qemu_set_irq(s->irq, 1);
}

static const MemoryRegionOps mips_qemu_ops = {
    .read = goldfish_tty_read,
    .write = goldfish_tty_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static void goldfish_tty_realize(DeviceState *dev, Error **errp)
{
    SysBusDevice *sbdev = SYS_BUS_DEVICE(dev);
    struct tty_state *s = GOLDFISH_TTY(dev);
    int i;

    if ((instance_id + 1) == MAX_SERIAL_PORTS) {
        cpu_abort(current_cpu, "goldfish_tty: MAX_SERIAL_PORTS(%d) reached\n", MAX_SERIAL_PORTS);
    }

    memory_region_init_io(&s->iomem, OBJECT(s), &mips_qemu_ops, s,
            "goldfish_tty", 0x1000);
    sysbus_init_mmio(sbdev, &s->iomem);
    sysbus_init_irq(sbdev, &s->irq);

    for(i = 0; i < MAX_SERIAL_PORTS; i++) {
        if(serial_hds[i]) {
            s->cs = serial_hds[i];
            qemu_chr_add_handlers(serial_hds[i], tty_can_receive, tty_receive, NULL, s);
            break;
        }
    }

    register_savevm(NULL,
                    "goldfish_tty",
                    instance_id++,
                    GOLDFISH_TTY_SAVE_VERSION,
                    goldfish_tty_save,
                    goldfish_tty_load,
                    s);
}

static void goldfish_tty_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = goldfish_tty_realize;
    dc->desc = "goldfish tty";
}

static const TypeInfo goldfish_tty_info = {
    .name          = TYPE_GOLDFISH_TTY,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(struct tty_state),
    .class_init    = goldfish_tty_class_init,
};

static void goldfish_tty_register(void)
{
    type_register_static(&goldfish_tty_info);
}

type_init(goldfish_tty_register);
