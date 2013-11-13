/*
 * ARM Android ("lionhead") emulator
 *
 * modified from vexpress.c
 *
 * Copyright (c) 2010 - 2011 B Labs Ltd.
 * Copyright (c) 2011 Linaro Limited
 * Written by Bahadir Balban, Amit Mahajan, Peter Maydell
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, see <http://www.gnu.org/licenses/>.
 *
 *  Contributions after 2012-01-13 are licensed under the terms of the
 *  GNU GPL, version 2 or (at your option) any later version.
 */

#include "hw/sysbus.h"
#include "hw/arm/arm.h"
#include "hw/arm/primecell.h"
#include "hw/devices.h"
#include "sysemu/sysemu.h"
#include "hw/boards.h"
#include "exec/address-spaces.h"
#include "sysemu/blockdev.h"
#include "hw/block/flash.h"
#include "sysemu/device_tree.h"
#include <libfdt.h>

#define LIONHEAD_BOARD_ID 0x5A1

/* Number of virtio transports to create (0..8; limited by
 * number of available IRQ lines).
 */
#define NUM_VIRTIO_TRANSPORTS 4

/* Address maps for peripherals:
 * the Versatile Express motherboard has two possible maps,
 * the "legacy" one (used for A9) and the "Cortex-A Series"
 * map (used for newer cores).
 * Individual daughterboards can also have different maps for
 * their peripherals.
 */

enum {
    VE_SYSREGS,
    VE_SP810,
    VE_SERIALPCI,
    GOLDFISH_AUDIO,
    GOLDFISH_BATTERY,
    VE_KMI0,
    VE_KMI1,
    VE_UART0,
    VE_UART1,
    VE_UART2,
    VE_UART3,
    VE_WDT,
    VE_TIMER01,
    VE_TIMER23,
    VE_RTC,
    VE_COMPACTFLASH,
    GOLDFISH_FB,
    VE_NORFLASH0,
    VE_NORFLASH1,
    VE_NORFLASHALIAS,
    VE_SRAM,
    VE_ETHERNET,
    VE_USB,
    VE_DAPROM,
    VE_VIRTIO,
};

static hwaddr motherboard_aseries_map[] = {
    [VE_NORFLASHALIAS] = 0,
    /* CS0: 0x08000000 .. 0x0c000000 */
    [VE_NORFLASH0] = 0x08000000,
    /* CS4: 0x0c000000 .. 0x10000000 */
    [VE_NORFLASH1] = 0x0c000000,
    /* CS5: 0x10000000 .. 0x14000000 */
    /* CS1: 0x14000000 .. 0x18000000 */
    [VE_SRAM] = 0x14000000,
    /* CS2: 0x18000000 .. 0x1c000000 */
    [VE_ETHERNET] = 0x1a000000,
    [VE_USB] = 0x1b000000,
    /* CS3: 0x1c000000 .. 0x20000000 */
    [VE_DAPROM] = 0x1c000000,
    [VE_SYSREGS] = 0x1c010000,
    [VE_SP810] = 0x1c020000,
    [VE_SERIALPCI] = 0x1c030000,
    [GOLDFISH_AUDIO] = 0x1c040000,
    [GOLDFISH_BATTERY] = 0x1c050000,
    [VE_KMI0] = 0x1c060000,
    [VE_KMI1] = 0x1c070000,
    [VE_UART0] = 0x1c090000,
    [VE_UART1] = 0x1c0a0000,
    [VE_UART2] = 0x1c0b0000,
    [VE_UART3] = 0x1c0c0000,
    [VE_WDT] = 0x1c0f0000,
    [VE_TIMER01] = 0x1c110000,
    [VE_TIMER23] = 0x1c120000,
    [VE_VIRTIO] = 0x1c130000,
    [VE_RTC] = 0x1c170000,
    [VE_COMPACTFLASH] = 0x1c1a0000,
    [GOLDFISH_FB] = 0x1c1f0000,
};

/* Structure defining the peculiarities of a specific daughterboard */

typedef struct VEDBoardInfo VEDBoardInfo;

typedef void DBoardInitFn(const VEDBoardInfo *daughterboard,
                          ram_addr_t ram_size,
                          const char *cpu_model,
                          qemu_irq *pic);

struct VEDBoardInfo {
    struct arm_boot_info bootinfo;
    const hwaddr *motherboard_map;
    hwaddr loader_start;
    const hwaddr gic_cpu_if_addr;
    uint32_t proc_id;
    uint32_t num_voltage_sensors;
    const uint32_t *voltages;
    uint32_t num_clocks;
    const uint32_t *clocks;
    DBoardInitFn *init;
};

static void a15_daughterboard_init(const VEDBoardInfo *daughterboard,
                                   ram_addr_t ram_size,
                                   const char *cpu_model,
                                   qemu_irq *pic)
{
    int n;
    MemoryRegion *sysmem = get_system_memory();
    MemoryRegion *ram = g_new(MemoryRegion, 1);
    MemoryRegion *sram = g_new(MemoryRegion, 1);
    qemu_irq cpu_irq[4];
    DeviceState *dev;
    SysBusDevice *busdev;

    if (!cpu_model) {
        cpu_model = "cortex-a15";
    }

    for (n = 0; n < smp_cpus; n++) {
        ARMCPU *cpu;

        cpu = cpu_arm_init(cpu_model);
        if (!cpu) {
            fprintf(stderr, "Unable to find CPU definition\n");
            exit(1);
        }
        cpu_irq[n] = qdev_get_gpio_in(DEVICE(cpu), ARM_CPU_IRQ);
    }

    {
        /* We have to use a separate 64 bit variable here to avoid the gcc
         * "comparison is always false due to limited range of data type"
         * warning if we are on a host where ram_addr_t is 32 bits.
         */
        uint64_t rsz = ram_size;
        if (rsz > (30ULL * 1024 * 1024 * 1024)) {
            fprintf(stderr, "vexpress-a15: cannot model more than 30GB RAM\n");
            exit(1);
        }
    }

    memory_region_init_ram(ram, NULL, "vexpress.highmem", ram_size);
    vmstate_register_ram_global(ram);
    /* RAM is from 0x80000000 upwards; there is no low-memory alias for it. */
    memory_region_add_subregion(sysmem, 0x80000000, ram);

    /* 0x2c000000 A15MPCore private memory region (GIC) */
    dev = qdev_create(NULL, "a15mpcore_priv");
    qdev_prop_set_uint32(dev, "num-cpu", smp_cpus);
    qdev_init_nofail(dev);
    busdev = SYS_BUS_DEVICE(dev);
    sysbus_mmio_map(busdev, 0, 0x2c000000);
    for (n = 0; n < smp_cpus; n++) {
        sysbus_connect_irq(busdev, n, cpu_irq[n]);
    }
    /* Interrupts [42:0] are from the motherboard;
     * [47:43] are reserved; [63:48] are daughterboard
     * peripherals. Note that some documentation numbers
     * external interrupts starting from 32 (because there
     * are internal interrupts 0..31).
     */
    for (n = 0; n < 64; n++) {
        pic[n] = qdev_get_gpio_in(dev, n);
    }

    /* A15 daughterboard peripherals: */

    /* 0x20000000: CoreSight interfaces: not modelled */
    /* 0x2a000000: PL301 AXI interconnect: not modelled */
    /* 0x2a420000: SCC: not modelled */
    /* 0x2a430000: system counter: not modelled */
    /* 0x2b000000: HDLCD controller: not modelled */
    /* 0x2b060000: SP805 watchdog: not modelled */
    /* 0x2b0a0000: PL341 dynamic memory controller: not modelled */
    /* 0x2e000000: system SRAM */
    memory_region_init_ram(sram, NULL, "vexpress.a15sram", 0x10000);
    vmstate_register_ram_global(sram);
    memory_region_add_subregion(sysmem, 0x2e000000, sram);

    /* 0x7ffb0000: DMA330 DMA controller: not modelled */
    /* 0x7ffd0000: PL354 static memory controller: not modelled */
}

static const uint32_t a15_voltages[] = {
    900000, /* Vcore: 0.9V : CPU core voltage */
};

static const uint32_t a15_clocks[] = {
    60000000, /* OSCCLK0: 60MHz : CPU_CLK reference */
    0, /* OSCCLK1: reserved */
    0, /* OSCCLK2: reserved */
    0, /* OSCCLK3: reserved */
    40000000, /* OSCCLK4: 40MHz : external AXI master clock */
    23750000, /* OSCCLK5: 23.75MHz : HDLCD PLL reference */
    50000000, /* OSCCLK6: 50MHz : static memory controller clock */
    60000000, /* OSCCLK7: 60MHz : SYSCLK reference */
    40000000, /* OSCCLK8: 40MHz : DDR2 PLL reference */
};

static VEDBoardInfo a15_daughterboard = {
    .motherboard_map = motherboard_aseries_map,
    .loader_start = 0x80000000,
    .gic_cpu_if_addr = 0x2c002000,
    .proc_id = 0x14000237,
    .num_voltage_sensors = ARRAY_SIZE(a15_voltages),
    .voltages = a15_voltages,
    .num_clocks = ARRAY_SIZE(a15_clocks),
    .clocks = a15_clocks,
    .init = a15_daughterboard_init,
};

static int add_virtio_mmio_node(void *fdt, uint32_t acells, uint32_t scells,
                                hwaddr addr, hwaddr size, uint32_t intc,
                                int irq)
{
    /* Add a virtio_mmio node to the device tree blob:
     *   virtio_mmio@ADDRESS {
     *       compatible = "virtio,mmio";
     *       reg = <ADDRESS, SIZE>;
     *       interrupt-parent = <&intc>;
     *       interrupts = <0, irq, 1>;
     *   }
     * (Note that the format of the interrupts property is dependent on the
     * interrupt controller that interrupt-parent points to; these are for
     * the ARM GIC and indicate an SPI interrupt, rising-edge-triggered.)
     */
    int rc;
    char *nodename = g_strdup_printf("/virtio_mmio@%" PRIx64, addr);

    rc = qemu_fdt_add_subnode(fdt, nodename);
    rc |= qemu_fdt_setprop_string(fdt, nodename,
                                  "compatible", "virtio,mmio");
    rc |= qemu_fdt_setprop_sized_cells(fdt, nodename, "reg",
                                       acells, addr, scells, size);
    qemu_fdt_setprop_cells(fdt, nodename, "interrupt-parent", intc);
    qemu_fdt_setprop_cells(fdt, nodename, "interrupts", 0, irq, 1);
    g_free(nodename);
    if (rc) {
        return -1;
    }
    return 0;
}

static uint32_t find_int_controller(void *fdt)
{
    /* Find the FDT node corresponding to the interrupt controller
     * for virtio-mmio devices. We do this by scanning the fdt for
     * a node with the right compatibility, since we know there is
     * only one GIC on a vexpress board.
     * We return the phandle of the node, or 0 if none was found.
     */
    const char *compat = "arm,cortex-a9-gic";
    int offset;

    offset = fdt_node_offset_by_compatible(fdt, -1, compat);
    if (offset >= 0) {
        return fdt_get_phandle(fdt, offset);
    }
    return 0;
}

static void vexpress_modify_dtb(const struct arm_boot_info *info, void *fdt)
{
    uint32_t acells, scells, intc;
    const VEDBoardInfo *daughterboard = (const VEDBoardInfo *)info;

    acells = qemu_fdt_getprop_cell(fdt, "/", "#address-cells");
    scells = qemu_fdt_getprop_cell(fdt, "/", "#size-cells");
    intc = find_int_controller(fdt);
    if (!intc) {
        /* Not fatal, we just won't provide virtio. This will
         * happen with older device tree blobs.
         */
        fprintf(stderr, "QEMU: warning: couldn't find interrupt controller in "
                "dtb; will not include virtio-mmio devices in the dtb.\n");
    } else {
        int i;
        const hwaddr *map = daughterboard->motherboard_map;

        /* We iterate backwards here because adding nodes
         * to the dtb puts them in last-first.
         */
        for (i = NUM_VIRTIO_TRANSPORTS - 1; i >= 0; i--) {
            add_virtio_mmio_node(fdt, acells, scells,
                                 map[VE_VIRTIO] + 0x200 * i,
                                 0x200, intc, 40 + i);
        }
    }
}

static void lionhead_common_init(VEDBoardInfo *daughterboard,
                                 QEMUMachineInitArgs *args)
{
    DeviceState *sysctl;
    qemu_irq pic[64];
    uint32_t sys_id;
    ram_addr_t sram_size;
    MemoryRegion *sysmem = get_system_memory();
    MemoryRegion *sram = g_new(MemoryRegion, 1);
    const hwaddr *map = daughterboard->motherboard_map;
    int i;

    daughterboard->init(daughterboard, args->ram_size, args->cpu_model, pic);

    /* Motherboard peripherals: the wiring is the same but the
     * addresses vary between the legacy and A-Series memory maps.
     */

    sys_id = 0x1190f500;

    sysctl = qdev_create(NULL, "realview_sysctl");
    qdev_prop_set_uint32(sysctl, "sys_id", sys_id);
    qdev_prop_set_uint32(sysctl, "proc_id", daughterboard->proc_id);
    qdev_prop_set_uint32(sysctl, "len-db-voltage",
                         daughterboard->num_voltage_sensors);
    for (i = 0; i < daughterboard->num_voltage_sensors; i++) {
        char *propname = g_strdup_printf("db-voltage[%d]", i);
        qdev_prop_set_uint32(sysctl, propname, daughterboard->voltages[i]);
        g_free(propname);
    }
    qdev_prop_set_uint32(sysctl, "len-db-clock",
                         daughterboard->num_clocks);
    for (i = 0; i < daughterboard->num_clocks; i++) {
        char *propname = g_strdup_printf("db-clock[%d]", i);
        qdev_prop_set_uint32(sysctl, propname, daughterboard->clocks[i]);
        g_free(propname);
    }
    qdev_init_nofail(sysctl);
    sysbus_mmio_map(SYS_BUS_DEVICE(sysctl), 0, map[VE_SYSREGS]);

    /* VE_SP810: not modelled */
    /* VE_SERIALPCI: not modelled */

    sysbus_create_simple("goldfish_audio", map[GOLDFISH_AUDIO], pic[11]);
    sysbus_create_simple("goldfish_battery", map[GOLDFISH_BATTERY], pic[9]);

    sysbus_create_simple("pl050_keyboard", map[VE_KMI0], pic[12]);
    sysbus_create_simple("pl050_mouse", map[VE_KMI1], pic[13]);

    sysbus_create_simple("pl011", map[VE_UART0], pic[5]);
    sysbus_create_simple("pl011", map[VE_UART1], pic[6]);
    sysbus_create_simple("pl011", map[VE_UART2], pic[7]);
    sysbus_create_simple("pl011", map[VE_UART3], pic[8]);

    sysbus_create_simple("sp804", map[VE_TIMER01], pic[2]);
    sysbus_create_simple("sp804", map[VE_TIMER23], pic[3]);

    /* VE_SERIALDVI: not modelled */

    sysbus_create_simple("pl031", map[VE_RTC], pic[4]); /* RTC */

    /* VE_COMPACTFLASH: not modelled */

    sysbus_create_simple("goldfish_fb", map[GOLDFISH_FB], pic[14]);

    sram_size = 0x2000000;
    memory_region_init_ram(sram, NULL, "vexpress.sram", sram_size);
    vmstate_register_ram_global(sram);
    memory_region_add_subregion(sysmem, map[VE_SRAM], sram);

    /* VE_USB: not modelled */

    /* VE_DAPROM: not modelled */

    /* Create mmio transports, so the user can create virtio backends
     * (which will be automatically plugged in to the transports). If
     * no backend is created the transport will just sit harmlessly idle.
     */
    for (i = 0; i < NUM_VIRTIO_TRANSPORTS; i++) {
        sysbus_create_simple("virtio-mmio", map[VE_VIRTIO] + 0x200 * i,
                             pic[40 + i]);
    }

    daughterboard->bootinfo.ram_size = args->ram_size;
    daughterboard->bootinfo.kernel_filename = args->kernel_filename;
    daughterboard->bootinfo.kernel_cmdline = args->kernel_cmdline;
    daughterboard->bootinfo.initrd_filename = args->initrd_filename;
    daughterboard->bootinfo.nb_cpus = smp_cpus;
    daughterboard->bootinfo.board_id = LIONHEAD_BOARD_ID;
    daughterboard->bootinfo.loader_start = daughterboard->loader_start;
    daughterboard->bootinfo.smp_loader_start = map[VE_SRAM];
    daughterboard->bootinfo.smp_bootreg_addr = map[VE_SYSREGS] + 0x30;
    daughterboard->bootinfo.gic_cpu_if_addr = daughterboard->gic_cpu_if_addr;
    daughterboard->bootinfo.modify_dtb = vexpress_modify_dtb;
    arm_load_kernel(ARM_CPU(first_cpu), &daughterboard->bootinfo);
}

static void lionhead_a15_init(QEMUMachineInitArgs *args)
{
    lionhead_common_init(&a15_daughterboard, args);
}

static QEMUMachine lionhead_a15_machine = {
    .name = "lionhead-a15",
    .desc = "ARM Android Emulator for Cortex-A15",
    .init = lionhead_a15_init,
    .block_default_type = IF_SCSI,
    .max_cpus = 4,
};

static void vexpress_machine_init(void)
{
    qemu_register_machine(&lionhead_a15_machine);
}

machine_init(vexpress_machine_init);
