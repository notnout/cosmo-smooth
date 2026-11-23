/*
 * dos_hw.c -- Real 32-bit DOS hardware access for Open Watcom / CauseWay.
 *
 * Kept separate from cosmo/dos_compat.h so Watcom's <dos.h>/<conio.h>/<i86.h>
 * never clash with the Borland compatibility names used by Cosmore.
 */

#include <conio.h>
#include <dos.h>
#include <i86.h>
#include <stdint.h>
#include <stddef.h>

#include "dos_hw.h"

typedef void (__interrupt __far *watcom_isr_t)(void);

static watcom_isr_t orig_int8;
static watcom_isr_t orig_int9;
static int irqs_installed;

static dos_irq_callback user_int8_cb;
static dos_irq_callback user_int9_cb;

static volatile int chain_int8_flag;
static volatile int chain_int9_flag;
static volatile int isr_depth;

static volatile uint32_t pit0_period = 65536u;
static volatile uint64_t pit_ticks_total;
static volatile uint64_t last_monotonic_ticks;

/* Inline helpers for saving/restoring EFLAGS (specifically IF). */
uint32_t watcom_pushf_read(void);
#pragma aux watcom_pushf_read = \
    "pushfd" \
    "pop eax" \
    value [eax];

void watcom_popf_write(uint32_t flags);
#pragma aux watcom_popf_write = \
    "push eax" \
    "popfd" \
    parm [eax];

uint8_t hw_inb(uint16_t port)
{
    return (uint8_t)inp(port);
}

void hw_outb(uint16_t port, uint8_t value)
{
    outp(port, value);
}

void hw_cli(void)
{
    _disable();
}

void hw_sti(void)
{
    _enable();
}

uint32_t hw_save_flags(void)
{
    return watcom_pushf_read();
}

void hw_restore_flags(uint32_t flags)
{
    watcom_popf_write(flags);
}

void hw_vga_set_mode(uint8_t mode)
{
    union REGS r;
    r.w.ax = mode;
    int386(0x10, &r, &r);
}

void hw_vga_set_dac(uint8_t index, uint8_t r6, uint8_t g6, uint8_t b6)
{
    outp(0x03C8, index);
    outp(0x03C9, r6);
    outp(0x03C9, g6);
    outp(0x03C9, b6);
}

int hw_vga_in_retrace(void)
{
    return (inp(0x03DA) & 0x08) != 0;
}

uint8_t *hw_vga_framebuffer(void)
{
    return (uint8_t *)0xA0000UL;
}

static void __interrupt __far hw_int8_isr(void)
{
    isr_depth++;
    pit_ticks_total += pit0_period;
    chain_int8_flag = 0;

    if (user_int8_cb) {
        user_int8_cb();
        if (chain_int8_flag) {
            isr_depth--;
            _chain_intr(orig_int8);
        }
        isr_depth--;
        return;
    }

    isr_depth--;
    _chain_intr(orig_int8);
}

static void __interrupt __far hw_int9_isr(void)
{
    isr_depth++;
    chain_int9_flag = 0;

    if (user_int9_cb) {
        user_int9_cb();
        if (chain_int9_flag) {
            isr_depth--;
            _chain_intr(orig_int9);
        }
        isr_depth--;
        return;
    }

    isr_depth--;
    _chain_intr(orig_int9);
}

void hw_install_irqs(dos_irq_callback timer_cb, dos_irq_callback kbd_cb)
{
    if (irqs_installed) return;
    _disable();
    user_int8_cb = timer_cb;
    user_int9_cb = kbd_cb;
    orig_int8 = _dos_getvect(8);
    orig_int9 = _dos_getvect(9);
    _dos_setvect(8, hw_int8_isr);
    _dos_setvect(9, hw_int9_isr);
    irqs_installed = 1;
    _enable();
}

void hw_restore_irqs(void)
{
    if (!irqs_installed) return;
    _disable();
    /* Restore PIT channel 0 to standard 18.2 Hz */
    outp(0x0043, 0x36);
    outp(0x0040, 0x00);
    outp(0x0040, 0x00);
    pit0_period = 65536u;
    /* Silence PC speaker */
    outp(0x0061, inp(0x0061) & ~0x03);
    _dos_setvect(8, orig_int8);
    _dos_setvect(9, orig_int9);
    user_int8_cb = NULL;
    user_int9_cb = NULL;
    irqs_installed = 0;
    _enable();
}

void hw_request_chain_int8(void)
{
    chain_int8_flag = 1;
}

void hw_request_chain_int9(void)
{
    chain_int9_flag = 1;
}

int hw_in_isr(void)
{
    return isr_depth != 0;
}

void hw_set_pit0_divisor(uint16_t divisor)
{
    pit0_period = divisor ? (uint32_t)divisor : 65536u;
}

uint64_t hw_read_pit_ticks(void)
{
    uint32_t flags = watcom_pushf_read();
    uint64_t base;
    uint32_t period;
    uint8_t lo, hi;
    uint32_t remaining, elapsed;
    uint64_t sample;

    _disable();
    base = pit_ticks_total;
    period = pit0_period;
    /* Latch PIT channel 0 current count */
    outp(0x0043, 0x00);
    lo = (uint8_t)inp(0x0040);
    hi = (uint8_t)inp(0x0040);
    watcom_popf_write(flags);

    remaining = ((uint32_t)hi << 8) | lo;
    if (remaining > period) remaining = period;
    elapsed = period - remaining;
    sample = base + elapsed;

    /* Guard against a pending-IRQ boundary where the counter just wrapped
     * before hw_int8_isr ran: keep the clock strictly monotonic. */
    if (sample < last_monotonic_ticks) {
        sample = last_monotonic_ticks;
    } else {
        last_monotonic_ticks = sample;
    }
    return sample;
}

void hw_delay_us(unsigned us)
{
    uint64_t target;
    uint64_t ticks = ((uint64_t)us * DOS_PIT_HZ + 999999ULL) / 1000000ULL;
    if (ticks < 2) ticks = 2;
    target = hw_read_pit_ticks() + ticks;
    while (hw_read_pit_ticks() < target) {
        /* Wait on PIT hardware counter */
    }
}

int hw_getch(void)
{
    return getch();
}
