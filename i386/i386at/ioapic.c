/*
 * Copyright (C) 2019 Free Software Foundation, Inc.
 *
 * This file is part of GNU Mach.
 *
 * GNU Mach is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * GNU Mach is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111, USA.
 */

#include <sys/types.h>
#include <i386/ipl.h>
#include <machine/irq.h>
#include <i386/fpu.h>
#include <i386/hardclock.h>
#include <i386at/kd.h>
#include <i386at/idt.h>
#include <i386/pio.h>
#include <i386/pit.h>
#include <i386/pic.h> /* only for macros */
#include <mach/machine.h>
#include <kern/printf.h>

static int has_irq_specific_eoi = 1; /* FIXME: Assume all machines have this */
int duplicate_pin;

uint32_t lapic_timer_val = 0;
uint32_t calibrated_ticks = 0;

spl_t curr_ipl;

int iunit[NINTR] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
                    16, 17, 18, 19, 20, 21, 22, 23};

interrupt_handler_fn ivect[NINTR] = {
    /* 00 */	intnull,	/* install timer later */
    /* 01 */	kdintr,		/* kdintr, ... */
    /* 02 */	intnull,
    /* 03 */	intnull,	/* lnpoll, comintr, ... */

    /* 04 */	intnull,	/* comintr, ... */
    /* 05 */	intnull,	/* comintr, wtintr, ... */
    /* 06 */	intnull,	/* fdintr, ... */
    /* 07 */	intnull,	/* qdintr, ... */

    /* 08 */	intnull,
    /* 09 */	intnull,	/* ether */
    /* 10 */	intnull,
    /* 11 */	intnull,

    /* 12 */	intnull,
    /* 13 */	fpintr,		/* always */
    /* 14 */	intnull,	/* hdintr, ... */
    /* 15 */	intnull,	/* ??? */

    /* 16 */	intnull,	/* PIRQA */
    /* 17 */	intnull,	/* PIRQB */
    /* 18 */	intnull,	/* PIRQC */
    /* 19 */	intnull,	/* PIRQD */
    /* 20 */	intnull,	/* PIRQE */
    /* 21 */	intnull,	/* PIRQF */
    /* 22 */	intnull,	/* PIRQG */
    /* 23 */	intnull,	/* PIRQH */
};

void
picdisable(void)
{
    asm("cli");
    curr_ipl = SPLHI;

    /*
    ** Disable PIC
    */
    outb ( PIC_SLAVE_OCW, PICS_MASK );
    outb ( PIC_MASTER_OCW, PICM_MASK );

    /*
    ** Route interrupts through IOAPIC
    */
    outb ( IMCR_SELECT, MODE_IMCR );
    outb ( IMCR_DATA, IMCR_USE_APIC );
}

void
intnull(int unit_dev)
{
    printf("intnull(%d)\n", unit_dev);
}

static uint32_t
ioapic_read(uint8_t id, uint8_t reg)
{
    volatile ApicIoUnit *ioapic = apic_get_ioapic(id)->ioapic;
    ioapic->select.r = reg;
    return ioapic->window.r;
}

static void
ioapic_write(uint8_t id, uint8_t reg, uint32_t value)
{
    volatile ApicIoUnit *ioapic = apic_get_ioapic(id)->ioapic;
    ioapic->select.r = reg;
    ioapic->window.r = value;
}

static void
ioapic_read_entry(int apic, int pin, struct ioapic_route_entry *e)
{
    union ioapic_route_entry_union entry;

    entry.lo = ioapic_read(apic, APIC_IO_REDIR_LOW(pin));
    entry.hi = ioapic_read(apic, APIC_IO_REDIR_HIGH(pin));

    *e = entry.both;
}

/* Write the high word first because mask bit is in low word */
static void
ioapic_write_entry(int apic, int pin, struct ioapic_route_entry e)
{
    union ioapic_route_entry_union entry = {{0, 0}};

    entry.both = e;
    ioapic_write(apic, APIC_IO_REDIR_HIGH(pin), entry.hi);
    ioapic_write(apic, APIC_IO_REDIR_LOW(pin), entry.lo);
}

/* When toggling the interrupt via mask, write low word only */
static void
ioapic_toggle_entry(int apic, int pin, int mask)
{
    union ioapic_route_entry_union entry;

    ioapic_read_entry(apic, pin, &entry.both);
    entry.both.mask = mask & 0x1;
    ioapic_write(apic, APIC_IO_REDIR_LOW(pin), entry.lo);
}

static uint32_t
pit_measure_apic_hz(void)
{
    uint32_t start = 0xffffffff;

    /* Prepare accurate delay for 1/hz seconds */
    pit_prepare_sleep(hz);

    /* Set APIC timer */
    lapic->init_count.r = start;

    /* zZz */
    pit_sleep();

    /* Stop APIC timer */
    lapic->lvt_timer.r |= LAPIC_DISABLE;

    return start - lapic->cur_count.r;
}

void lapic_update_timer(void)
{
    /* Timer decrements until zero and then calls this on every interrupt */
    lapic_timer_val += calibrated_ticks;
}

void
lapic_enable_timer(void)
{
    /* Set up counter */
    lapic->init_count.r = calibrated_ticks;
    lapic->divider_config.r = LAPIC_TIMER_DIVIDE_16;

    /* Set the timer to interrupt periodically on remapped timer GSI */
    lapic->lvt_timer.r = IOAPIC_INT_BASE | LAPIC_TIMER_PERIODIC;

    /* Some buggy hardware requires this set again */
    lapic->divider_config.r = LAPIC_TIMER_DIVIDE_16;

    /* Enable interrupts for the first time */
    printf("LAPIC timer configured on cpu%d\n", cpu_number());
}

void
ioapic_toggle(int pin, int mask)
{
    int apic = 0;
    ioapic_toggle_entry(apic, pin, mask);
}

void
ioapic_irq_eoi(int pin)
{
    int apic = 0;
    union ioapic_route_entry_union oldentry, entry;

    if (pin == 0)
        goto skip_specific_eoi;

    if (!has_irq_specific_eoi) {
        /* Workaround for old IOAPICs with no specific EOI */

        /* Mask the pin and change to edge triggered */
        ioapic_read_entry(apic, pin, &entry.both);
        oldentry = entry;
        entry.both.mask = IOAPIC_MASK_DISABLED;
        entry.both.trigger = IOAPIC_EDGE_TRIGGERED;
        ioapic_write_entry(apic, pin, entry.both);

        /* Restore level entry */
        ioapic_write_entry(apic, pin, oldentry.both);
    } else {
        volatile ApicIoUnit *ioapic = apic_get_ioapic(apic)->ioapic;

        ioapic_read_entry(apic, pin, &entry.both);
        ioapic->eoi.r = entry.both.vector;
    }

skip_specific_eoi:
    lapic_eoi ();
}

static unsigned int
override_irq(IrqOverrideData *override, union ioapic_route_entry_union *entry)
{
    if (override->flags & APIC_IRQ_OVERRIDE_TRIGGER_MASK) {
        entry->both.trigger = (override->flags & APIC_IRQ_OVERRIDE_LEVEL_TRIGGERED) ?
                              IOAPIC_LEVEL_TRIGGERED : IOAPIC_EDGE_TRIGGERED;
    } else {
        if (override->bus == 0) {
            /* ISA is edge-triggered by default */
            entry->both.trigger = IOAPIC_EDGE_TRIGGERED;
        } else {
            entry->both.trigger = IOAPIC_LEVEL_TRIGGERED;
        }
    }

    if (override->flags & APIC_IRQ_OVERRIDE_POLARITY_MASK) {
        entry->both.polarity = (override->flags & APIC_IRQ_OVERRIDE_ACTIVE_LOW) ?
                               IOAPIC_ACTIVE_LOW : IOAPIC_ACTIVE_HIGH;
    } else {
        if (override->bus == 0) {
            /* EISA is active-low for level-triggered interrupts */
            if (entry->both.trigger == IOAPIC_LEVEL_TRIGGERED) {
                entry->both.polarity = IOAPIC_ACTIVE_LOW;
            } else {
                entry->both.polarity = IOAPIC_ACTIVE_HIGH;
            }
        }
    }
    printf("IRQ override: pin=%d gsi=%d trigger=%s polarity=%s\n",
           override->irq, override->gsi,
           entry->both.trigger == IOAPIC_LEVEL_TRIGGERED ? "LEVEL" : "EDGE",
           entry->both.polarity == IOAPIC_ACTIVE_LOW ? "LOW" : "HIGH");

    return override->gsi;
}

void
ioapic_configure(void)
{
    /* Assume first IO APIC maps to GSI base 0 */
    int gsi, apic = 0, bsp = 0, pin;
    IrqOverrideData *irq_over;
    int timer_gsi;

    /* Disable IOAPIC interrupts and set spurious interrupt */
    lapic->spurious_vector.r = IOAPIC_SPURIOUS_BASE;

    union ioapic_route_entry_union entry = {{0, 0}};

    entry.both.delvmode = IOAPIC_FIXED;
    entry.both.destmode = IOAPIC_PHYSICAL;
    entry.both.mask = IOAPIC_MASK_DISABLED;
    entry.both.dest = apic_get_cpu_apic_id(bsp);

    for (pin = 0; pin < 16; pin++) {
        gsi = pin;

        /* ISA legacy IRQs */
        entry.both.trigger = IOAPIC_EDGE_TRIGGERED;
        entry.both.polarity = IOAPIC_ACTIVE_HIGH;

        if ((irq_over = acpi_get_irq_override(pin))) {
            gsi = override_irq(irq_over, &entry);
        }
        entry.both.vector = IOAPIC_INT_BASE + gsi;
        ioapic_write_entry(apic, pin, entry.both);

        /* Timer workaround for x86 */
        if (pin == 0) {
            /* Save timer info */
            timer_gsi = gsi;
        } else {
            /* Disable duplicated timer gsi */
            if (gsi == timer_gsi) {
                duplicate_pin = pin;
                /* Remap this interrupt pin to GSI base
                 * so we don't duplicate vectors */
                entry.both.vector = IOAPIC_INT_BASE;
                ioapic_write_entry(apic, duplicate_pin, entry.both);
                /* Mask the ioapic pin with deduplicated vector as
		 * we will never use it, since timer is on another gsi */
                mask_irq(duplicate_pin);
            }
        }
    }

    for (pin = 16; pin < 24; pin++) {
        gsi = pin;

        /* PCI IRQs PIRQ A-H */
        entry.both.trigger = IOAPIC_LEVEL_TRIGGERED;
        entry.both.polarity = IOAPIC_ACTIVE_LOW;

        if ((irq_over = acpi_get_irq_override(pin))) {
            gsi = override_irq(irq_over, &entry);
        }
        entry.both.vector = IOAPIC_INT_BASE + gsi;
        ioapic_write_entry(apic, pin, entry.both);
    }

    /* Start the IO APIC receiving interrupts */
    lapic_enable();

    /* Set one-shot timer */
    lapic->divider_config.r = LAPIC_TIMER_DIVIDE_16;
    lapic->lvt_timer.r = IOAPIC_INT_BASE;

    /* Measure number of APIC timer ticks in 1/hz seconds
     * but calibrate the timer to expire at rate of hz */
    calibrated_ticks = pit_measure_apic_hz() * hz;

    /* Set up counter later */
    lapic->lvt_timer.r = LAPIC_DISABLE;

    /* Install clock interrupt handler on pin 0 */
    ivect[0] = (interrupt_handler_fn)hardclock;

    printf("IOAPIC 0 configured\n");
}
