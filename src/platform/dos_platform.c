/*
 * dos_platform.c -- 32-bit DOS platform layer for Cosmo's Cosmic Adventure
 *                   (Open Watcom + CauseWay, targeting DOSBox Pure / RetroArch).
 */

#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "cosmo/dos_compat.h"
#include "cosmo/ega.h"
#include "cosmo/video.h"
#include "dos_hw.h"

extern void InnerMain(int argc, char *argv[]);
extern uint8_t isInGame;

static volatile int toggle_smooth_req;
static volatile int toggle_turbo_req;

/* ------------------------------------------------------------------------ */
/* Borland pseudo-registers and memory helpers                              */
/* ------------------------------------------------------------------------ */

unsigned int  _AX, _BX, _CX, _DX;
unsigned char _AH, _AL, _BH, _BL, _CH, _CL, _DH, _DL;

static unsigned char text_mode_memory[4000];

void *dos_map_segment(unsigned seg, unsigned ofs)
{
    if (seg == 0xB800) {
        if (ofs >= sizeof text_mode_memory) return NULL;
        return text_mode_memory + ofs;
    }
    return NULL;
}

int cosmo_getw(FILE *fp)
{
    int lo = fgetc(fp);
    int hi = fgetc(fp);
    if (lo == EOF || hi == EOF) return EOF;
    return (int)(((unsigned)(hi & 0xFF) << 8) | (unsigned)(lo & 0xFF));
}

int cosmo_putw(int value, FILE *fp)
{
    if (fputc(value & 0xFF, fp) == EOF) return EOF;
    if (fputc((value >> 8) & 0xFF, fp) == EOF) return EOF;
    return value;
}

void movmem(void *src, void *dest, unsigned length)
{
    if (!src || !dest) return;
    memmove(dest, src, length);
}

unsigned long coreleft(void)
{
    return 655360UL;
}

long filelength(int handle)
{
    struct stat st;
    if (fstat(handle, &st) != 0) return -1L;
    return (long)st.st_size;
}

char *strupr(char *string)
{
    char *p;
    for (p = string; *p; p++) {
        *p = (char)toupper((unsigned char)*p);
    }
    return string;
}

/* ------------------------------------------------------------------------ */
/* Interrupt vector dispatch (hardware INT 8 and INT 9 via dos_hw.c)        */
/* ------------------------------------------------------------------------ */

typedef void (*isr_fn)(void);
static isr_fn vectors[256];

static void bios_chain_int8(void)
{
    hw_request_chain_int8();
}

static void bios_chain_int9(void)
{
    hw_request_chain_int9();
}

static void bios_noop(void)
{
}

static void dispatch_int8(void)
{
    if (vectors[8]) vectors[8]();
    else hw_request_chain_int8();
}

static void dispatch_int9(void)
{
    if (vectors[9]) vectors[9]();
    else hw_request_chain_int9();
}

void (*getvect(int interruptno))(void)
{
    if (interruptno < 0 || interruptno >= 256) return bios_noop;
    return vectors[interruptno];
}

void setvect(int interruptno, void (*isr)(void))
{
    uint32_t flags;
    if (interruptno < 0 || interruptno >= 256) return;
    flags = hw_save_flags();
    hw_cli();
    vectors[interruptno] = isr ? isr : bios_noop;
    hw_restore_flags(flags);
}

void disable(void)
{
    hw_cli();
}

void enable(void)
{
    hw_sti();
}

unsigned long SaveInterruptFlag(void)
{
    return (unsigned long)hw_save_flags();
}

void RestoreInterruptFlag(unsigned long saved)
{
    hw_restore_flags((uint32_t)saved);
}

/* ------------------------------------------------------------------------ */
/* Port I/O: EGA emulated, PIT/PIC/KBD/Speaker/AdLib/Joystick real hardware */
/* ------------------------------------------------------------------------ */

static uint8_t pit0_latch_low;
static bool pit0_expecting_high;

void outportb(int portid, unsigned char value)
{
    switch (portid) {
    /* Emulated EGA registers */
    case EGA_PORT_SEQ_INDEX:
    case EGA_PORT_SEQ_DATA:
    case EGA_PORT_GC_INDEX:
    case EGA_PORT_GC_DATA:
        ega_out_byte((uint16_t)portid, value);
        break;

    /* PIT channel 0: track divisor for high-res clock AND program hardware */
    case 0x0043:
        if ((value >> 6) == 0) pit0_expecting_high = false;
        hw_outb(0x0043, value);
        break;

    case 0x0040:
        if (!pit0_expecting_high) {
            pit0_latch_low = value;
            pit0_expecting_high = true;
        } else {
            uint16_t div = (uint16_t)(((uint16_t)value << 8) | pit0_latch_low);
            hw_set_pit0_divisor(div);
            pit0_expecting_high = false;
        }
        hw_outb(0x0040, value);
        break;

    /* AdLib OPL2: include canonical ISA bus read delays */
    case 0x0388:
        hw_outb(0x0388, value);
        {
            int i;
            for (i = 0; i < 6; ++i) (void)hw_inb(0x0388);
        }
        break;

    case 0x0389:
        hw_outb(0x0389, value);
        {
            int i;
            for (i = 0; i < 35; ++i) (void)hw_inb(0x0388);
        }
        break;

    /* PIC, PC speaker, keyboard controller, joystick: direct hardware */
    default:
        hw_outb((uint16_t)portid, value);
        break;
    }
}

void outport(int portid, int value)
{
    switch (portid) {
    case EGA_PORT_SEQ_INDEX:
    case EGA_PORT_GC_INDEX:
        ega_out_word((uint16_t)portid, (uint16_t)value);
        break;
    default:
        outportb(portid, (unsigned char)(value & 0xFF));
        outportb(portid + 1, (unsigned char)((value >> 8) & 0xFF));
        break;
    }
}

static uint8_t last_kbd_port60 = 0x80;

unsigned char inportb(int portid)
{
    switch (portid) {
    case EGA_PORT_SEQ_INDEX:
    case EGA_PORT_SEQ_DATA:
    case EGA_PORT_GC_INDEX:
    case EGA_PORT_GC_DATA:
        return ega_in_byte((uint16_t)portid);

    case 0x0060:
        if (hw_in_isr()) {
            static uint8_t prev_scancode;
            uint8_t sc = hw_inb(0x0060);
            if (isInGame && sc != prev_scancode) {
                if (sc == 0x1A) toggle_smooth_req = 1;      /* '[': Smooth 70Hz / Classic 11Hz */
                else if (sc == 0x1B) toggle_turbo_req = 1;  /* ']': Normal / Turbo speed */
            }
            prev_scancode = sc;
            last_kbd_port60 = sc;
            return sc;
        }
        dos_video_idle();
        return last_kbd_port60;

    default:
        return hw_inb((uint16_t)portid);
    }
}

/* ------------------------------------------------------------------------ */
/* BIOS int 10h and console                                                 */
/* ------------------------------------------------------------------------ */

static void bios_video(unsigned char ah, unsigned char al,
                       unsigned char bh, unsigned char bl,
                       unsigned char *out_al)
{
    switch (ah) {
    case 0x00:
        ega_set_video_mode(al);
        if (al == 0x0D) {
            dos_video_drop_motion();
            video_present();
        }
        break;

    case 0x05:
        ega_select_active_page(al);
        platform_page_flipped();
        break;

    case 0x0F:
        if (out_al) *out_al = 0x0D;
        break;

    case 0x10:
        if (al == 0x00) {
            ega_set_palette_register(bl, bh);
        } else if (al == 0x01) {
            ega_set_border_color(bh);
        }
        break;

    default:
        break;
    }
}

int int86(int intno, union REGS *inregs, union REGS *outregs)
{
    unsigned char al = inregs->h.al;
    if (intno == 0x10) {
        bios_video(inregs->h.ah, inregs->h.al,
                   inregs->h.bh, inregs->h.bl, &al);
    }
    if (outregs) {
        if (outregs != inregs) *outregs = *inregs;
        outregs->h.al = al;
        outregs->x.cflag = 0;
    }
    return al;
}

void geninterrupt(int intno)
{
    if (intno == 0x10) {
        unsigned char ah = (unsigned char)(_AH ? _AH : (_AX >> 8));
        unsigned char al = (unsigned char)(_AL ? _AL : (_AX & 0xFF));
        unsigned char out_al = al;
        bios_video(ah, al, _BH, _BL, &out_al);
        _AL = out_al;
        return;
    }
    if (intno >= 0 && intno < 256 && vectors[intno]) {
        vectors[intno]();
    }
}

void textmode(int newmode)
{
    (void)newmode;
    video_shutdown();
}

int getch(void)
{
    return hw_getch();
}

/* ------------------------------------------------------------------------ */
/* Cosmore Platform* hooks                                                  */
/* ------------------------------------------------------------------------ */

static uint16_t camera_scroll_x;
static uint16_t camera_scroll_y;
static bool camera_backdrop_x;
static bool camera_backdrop_y;
static uint16_t camera_frame_ticks = 13;
static bool camera_reported_this_flip = false;
static bool smooth_mode = true;
static bool turbo_mode = false;

void PlatformIdle(void)
{
    if (toggle_smooth_req) {
        toggle_smooth_req = 0;
        smooth_mode = !smooth_mode;
        video_set_motion_interpolation(smooth_mode);
    }
    if (toggle_turbo_req) {
        toggle_turbo_req = 0;
        turbo_mode = !turbo_mode;
    }
    dos_video_idle();
}

void PlatformDelayMicroseconds(unsigned microseconds)
{
    hw_delay_us(microseconds);
}

void PlatformSetCameraPosition(uint16_t scroll_x, uint16_t scroll_y,
                               int backdrop_x, int backdrop_y, uint16_t frame_ticks)
{
    camera_scroll_x = scroll_x;
    camera_scroll_y = scroll_y;
    camera_backdrop_x = backdrop_x != 0;
    camera_backdrop_y = backdrop_y != 0;
    camera_frame_ticks = frame_ticks ? frame_ticks : 13;
    camera_reported_this_flip = true;
}

void platform_page_flipped(void)
{
    if (camera_reported_this_flip) {
        camera_reported_this_flip = false;
        video_capture_motion_frame(camera_scroll_x, camera_scroll_y,
                                   camera_backdrop_x, camera_backdrop_y,
                                   camera_frame_ticks);
    } else {
        dos_video_drop_motion();
        video_present();
    }
}

void PlatformSetMotionInterpolationPaused(int paused)
{
    video_pause_motion_interpolation(paused != 0);
}

FILE *platform_open_mod_asset(int episode, const char *asset_name, uint32_t *length)
{
    (void)episode;
    (void)asset_name;
    if (length) *length = 0;
    return NULL;
}

int PlatformPlayMP3Music(char *asset_name) { (void)asset_name; return 0; }
void PlatformStopMP3Music(void) {}
int PlatformPlayMP3Sound(unsigned sound_number) { (void)sound_number; return 0; }
void PlatformStopMP3Sound(void) {}
void PlatformHapticSound(unsigned sound_number) { (void)sound_number; }
void PlatformHapticMenuInput(uint8_t scancode) { (void)scancode; }
void PlatformSetAudioVolumes(int sound_percent, int music_percent)
{
    (void)sound_percent;
    (void)music_percent;
}
void PlatformSetMusicPaused(int paused) { (void)paused; }
int PlatformLowGravity(void) { return 0; }
int PlatformShieldMode(void) { return 0; }
int PlatformTurboMode(void) { return turbo_mode ? 1 : 0; }
void PlatformRecolorPlayer(uint8_t *tiles, unsigned size) { (void)tiles; (void)size; }
void PlatformSetNameInput(int enabled) { (void)enabled; }
int PlatformReadNameInput(void) { return 0; }
void PlatformSetTouchContext(int context) { (void)context; }
int PlatformTouchControlsEnabled(void) { return 0; }
int PlatformToggleTouchControls(void) { return 0; }
void PlatformSetGameSettingsOpen(int open) { (void)open; }

/* Repurpose the Game Settings -> Filter row to toggle 70 Hz Smooth vs Classic! */
const char *PlatformDisplayFilterName(void)
{
    return smooth_mode ? "Smooth 70Hz" : "Classic 11Hz";
}

int PlatformStepDisplayFilter(int direction)
{
    (void)direction;
    smooth_mode = !smooth_mode;
    video_set_motion_interpolation(smooth_mode);
    return 1;
}

/* ------------------------------------------------------------------------ */
/* Entry point                                                              */
/* ------------------------------------------------------------------------ */

static void dos_cleanup(void)
{
    hw_restore_irqs();
    video_shutdown();
}

int main(int argc, char *argv[])
{
    int i;
    int pass_argc = 1;
    char *pass_argv[16];

    pass_argv[0] = argv[0];
    for (i = 1; i < argc && pass_argc < 15; ++i) {
        if (strcmp(argv[i], "--classic") == 0 || strcmp(argv[i], "/classic") == 0) {
            smooth_mode = false;
        } else if (strcmp(argv[i], "--turbo") == 0 || strcmp(argv[i], "/turbo") == 0) {
            turbo_mode = true;
        } else if (strcmp(argv[i], "--diag") == 0 || strcmp(argv[i], "/diag") == 0) {
            dos_video_enable_diag();
        } else {
            pass_argv[pass_argc++] = argv[i];
        }
    }
    pass_argv[pass_argc] = NULL;

    for (i = 0; i < 256; ++i) vectors[i] = bios_noop;
    vectors[8] = bios_chain_int8;
    vectors[9] = bios_chain_int9;

    atexit(dos_cleanup);

    ega_init();
    video_init("Cosmo", 1, false);
    video_set_motion_interpolation(smooth_mode);
    hw_install_irqs(dispatch_int8, dispatch_int9);

    InnerMain(pass_argc, pass_argv);

    dos_cleanup();
    return 0;
}
