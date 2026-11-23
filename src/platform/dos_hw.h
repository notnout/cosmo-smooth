#ifndef COSMO_DOS_HW_H
#define COSMO_DOS_HW_H

#include <stdint.h>

#define DOS_PIT_HZ 1193182ULL

typedef void (*dos_irq_callback)(void);

uint8_t  hw_inb(uint16_t port);
void     hw_outb(uint16_t port, uint8_t value);

void     hw_cli(void);
void     hw_sti(void);
uint32_t hw_save_flags(void);
void     hw_restore_flags(uint32_t flags);

void     hw_vga_set_mode(uint8_t mode);
void     hw_vga_set_dac(uint8_t index, uint8_t r6, uint8_t g6, uint8_t b6);
int      hw_vga_in_retrace(void);
uint8_t *hw_vga_framebuffer(void);

void     hw_install_irqs(dos_irq_callback timer_cb, dos_irq_callback kbd_cb);
void     hw_restore_irqs(void);
void     hw_request_chain_int8(void);
void     hw_request_chain_int9(void);
int      hw_in_isr(void);

void     hw_set_pit0_divisor(uint16_t divisor);
uint64_t hw_read_pit_ticks(void);
void     hw_delay_us(unsigned us);

int      hw_getch(void);

#endif /* COSMO_DOS_HW_H */
