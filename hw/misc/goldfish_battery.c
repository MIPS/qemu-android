/* Copyright (C) 2007-2013 The Android Open Source Project
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
#include "hw/hw.h"
#include "hw/sysbus.h"
#include "qemu/error-report.h"

enum {
	/* status register */
	BATTERY_INT_STATUS	    = 0x00,
	/* set this to enable IRQ */
	BATTERY_INT_ENABLE	    = 0x04,

	BATTERY_AC_ONLINE       = 0x08,
	BATTERY_STATUS          = 0x0C,
	BATTERY_HEALTH          = 0x10,
	BATTERY_PRESENT         = 0x14,
	BATTERY_CAPACITY        = 0x18,

	BATTERY_STATUS_CHANGED	= 1U << 0,
	AC_STATUS_CHANGED   	= 1U << 1,
	BATTERY_INT_MASK        = BATTERY_STATUS_CHANGED | AC_STATUS_CHANGED,
};

const uint32_t POWER_SUPPLY_STATUS_CHARGING = 1;
const uint32_t POWER_SUPPLY_HEALTH_GOOD = 1;

#define TYPE_GOLDFISH_BATTERY "goldfish_battery"
#define GOLDFISH_BATTERY(obj) OBJECT_CHECK(struct goldfish_battery_state, (obj), TYPE_GOLDFISH_BATTERY)

struct goldfish_battery_state {
    SysBusDevice parent;

    MemoryRegion iomem;
    qemu_irq irq;

    // IRQs
    uint32_t int_status;
    // irq enable mask for int_status
    uint32_t int_enable;

    uint32_t ac_online;
    uint32_t status;
    uint32_t health;
    uint32_t present;
    uint32_t capacity;
};

/* update this each time you update the battery_state struct */
#define  BATTERY_STATE_SAVE_VERSION  1

static const VMStateDescription goldfish_battery_vmsd = {
    .name = "goldfish_battery",
    .version_id = BATTERY_STATE_SAVE_VERSION,
    .minimum_version_id = BATTERY_STATE_SAVE_VERSION,
    .minimum_version_id_old = BATTERY_STATE_SAVE_VERSION,
    .fields = (VMStateField[]) {
        VMSTATE_UINT32(int_status, struct goldfish_battery_state),
        VMSTATE_UINT32(int_enable, struct goldfish_battery_state),
        VMSTATE_UINT32(ac_online, struct goldfish_battery_state),
        VMSTATE_UINT32(status, struct goldfish_battery_state),
        VMSTATE_UINT32(health, struct goldfish_battery_state),
        VMSTATE_UINT32(present, struct goldfish_battery_state),
        VMSTATE_UINT32(capacity, struct goldfish_battery_state),
        VMSTATE_END_OF_LIST()
    }
};

static uint64_t goldfish_battery_read(void *opaque, hwaddr offset, unsigned size)
{
    uint64_t ret;
    struct goldfish_battery_state *s = opaque;

    switch(offset) {
        case BATTERY_INT_STATUS:
            // return current buffer status flags
            ret = s->int_status & s->int_enable;
            if (ret) {
                qemu_irq_lower(s->irq);
                s->int_status = 0;
            }
            return ret;

		case BATTERY_INT_ENABLE:
		    return s->int_enable;
		case BATTERY_AC_ONLINE:
		    return s->ac_online;
		case BATTERY_STATUS:
		    return s->status;
		case BATTERY_HEALTH:
		    return s->health;
		case BATTERY_PRESENT:
		    return s->present;
		case BATTERY_CAPACITY:
		    return s->capacity;

        default:
            error_report ("goldfish_battery_read: Bad offset " TARGET_FMT_plx,
                    offset);
            return 0;
    }
}

static void goldfish_battery_write(void *opaque, hwaddr offset, uint64_t val,
        unsigned size)
{
    struct goldfish_battery_state *s = opaque;

    switch(offset) {
        case BATTERY_INT_ENABLE:
            /* enable interrupts */
            s->int_enable = val;
            break;

        default:
            error_report ("goldfish_audio_write: Bad offset " TARGET_FMT_plx,
                    offset);
    }
}

static const MemoryRegionOps goldfish_battery_iomem_ops = {
    .read = goldfish_battery_read,
    .write = goldfish_battery_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void goldfish_battery_realize(DeviceState *dev, Error **errp)
{
    SysBusDevice *sbdev = SYS_BUS_DEVICE(dev);
    struct goldfish_battery_state *s = GOLDFISH_BATTERY(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &goldfish_battery_iomem_ops, s,
            "goldfish_battery", 0x1000);
    sysbus_init_mmio(sbdev, &s->iomem);
    sysbus_init_irq(sbdev, &s->irq);

    // default values for the battery
    s->ac_online = 1;
    s->status = POWER_SUPPLY_STATUS_CHARGING;
    s->health = POWER_SUPPLY_HEALTH_GOOD;
    s->present = 1;     // battery is present
    s->capacity = 50;   // 50% charged
}

static void goldfish_battery_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = goldfish_battery_realize;
    dc->vmsd = &goldfish_battery_vmsd;
    dc->desc = "goldfish battery";
}

static const TypeInfo goldfish_audio_info = {
    .name          = TYPE_GOLDFISH_BATTERY,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(struct goldfish_battery_state),
    .class_init    = goldfish_battery_class_init,
};

static void goldfish_audio_register(void)
{
    type_register_static(&goldfish_audio_info);
}

type_init(goldfish_audio_register);
