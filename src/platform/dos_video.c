/*
 * dos_video.c -- VGA Mode 13h (320x200x256 @ 70 Hz) presentation and
 *                Smooth Gameplay sub-tile pixel compositor for DOSBox Pure.
 *
 * Keeps Cosmo's exact 10.8 Hz EGA logic and draws interpolated 70 Hz frames
 * in 8-bit indexed color directly to the Mode 13h linear framebuffer (0xA0000).
 */

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cosmo/ega.h"
#include "cosmo/video.h"
#include "dos_hw.h"

#define FB_W EGA_SCREEN_W
#define FB_H EGA_SCREEN_H
#define FB_PIXELS (FB_W * FB_H)

#define MOTION_WORLD_TOP 8
#define MOTION_WORLD_BOTTOM 152
#define MOTION_WORLD_ROWS (MOTION_WORLD_BOTTOM - MOTION_WORLD_TOP)

/* lowlevel_ega.c may write masks up to the widescreen column stride. */
#define WIDE_PRESENT_W (EGA_SCREEN_W + EGA_WIDE_COLS * 8)
#define PREVIEW_MASK_STRIDE (WIDE_PRESENT_W / 8)
#define OCC_STRIDE (FB_W / 8)
#define PREVIEW_BACKDROP_W 320
#define PREVIEW_BACKDROP_H 144
#define PREVIEW_BACKDROP_BASE 0xA300u

#define BLAST_SIDE 128
#define BLAST_RING_LIMIT 24

/* 70.08 Hz VGA Mode 13h frame period in 1,193,182 Hz PIT ticks (~14.27 ms). */
#define FRAME_PERIOD_70HZ 17026ULL

/* Precomputed midpoint rotation for spinning stars: 9 degrees = pi / 20. */
#define SPIN_COS 0.9876883405951378
#define SPIN_SIN 0.15643446504023087
#define SPIN_COS_FP 64729   /* round(SPIN_COS * 65536) */
#define SPIN_SIN_FP 10252   /* round(SPIN_SIN * 65536) */

static inline int imin(int a, int b) { return a < b ? a : b; }
static inline int imax(int a, int b) { return a > b ? a : b; }
static inline uint64_t u64min(uint64_t a, uint64_t b) { return a < b ? a : b; }
static inline int iround(double v) { return (int)(v >= 0.0 ? floor(v + 0.5) : ceil(v - 0.5)); }

volatile int dos_ega_dirty = 1;
volatile int dos_backdrop_dirty = 1;
static bool vga_active = false;
static bool smooth_enabled = true;
static unsigned pause_depth = 0;

/* Two-buffer ping-pong avoids 128 KB of memcpy per game tick. */
static uint8_t motion_buf[2][FB_PIXELS];
static uint8_t *motion_previous = motion_buf[0];
static uint8_t *motion_current = motion_buf[1];
static uint8_t motion_output[FB_PIXELS];

/* 256-entry EGA planar-to-chunky lookup tables (8 pixels per 2x32-bit stores). */
static uint32_t ega_lut_lo[256];
static uint32_t ega_lut_hi[256];
static bool ega_lut_ready = false;

static unsigned motion_frame_count = 0;
static uint64_t motion_previous_time = 0;
static uint64_t motion_current_time = 0;
static uint16_t motion_previous_camera_x = 0;
static uint16_t motion_previous_camera_y = 0;
static uint16_t motion_current_camera_x = 0;
static uint16_t motion_current_camera_y = 0;
static bool motion_camera_valid = false;

static uint64_t preview_frame_period = 0;
static uint64_t preview_frame_start = 0;
static uint64_t next_present_ticks = 0;
static uint32_t last_idle_irq = 0;

/* Optional diagnostics (--diag) for verifying 70 Hz smooth output in DOSBox. */
static bool diag_enabled = false;
static uint64_t diag_start_ticks = 0;
static uint32_t diag_present_count = 0;
static uint32_t diag_smooth_count = 0;
static uint32_t diag_game_flips = 0;
static int diag_snap_title_done = 0;
static int diag_snap_game_done = 0;

void dos_video_enable_diag(void)
{
    diag_enabled = true;
}

static void diag_write_ppm(const char *path, const uint8_t *indexed)
{
    FILE *fp = fopen(path, "wb");
    uint8_t lut[16][3];
    int i;
    if (!fp) return;
    for (i = 0; i < 16; ++i) ega_palette_rgb(ega.palette[i], lut[i]);
    fprintf(fp, "P6\n%d %d\n255\n", FB_W, FB_H);
    for (i = 0; i < FB_PIXELS; ++i) {
        uint8_t idx = indexed[i] & 15;
        fwrite(lut[idx], 1, 3, fp);
    }
    fclose(fp);
}

static void diag_flush_log(uint64_t now)
{
    FILE *fp;
    double secs;
    if (!diag_enabled || !diag_start_ticks || now <= diag_start_ticks) return;
    secs = (double)(now - diag_start_ticks) / (double)DOS_PIT_HZ;
    fp = fopen("COSMO.LOG", "w");
    if (!fp) return;
    fprintf(fp, "elapsed_sec=%.2f\n", secs);
    fprintf(fp, "game_flips=%lu\n", (unsigned long)diag_game_flips);
    fprintf(fp, "total_presents=%lu\n", (unsigned long)diag_present_count);
    fprintf(fp, "smooth_presents=%lu\n", (unsigned long)diag_smooth_count);
    fprintf(fp, "measured_fps=%.2f\n", secs > 0.01 ? (double)diag_present_count / secs : 0.0);
    fclose(fp);
}

static uint8_t preview_page_mask[2][FB_H][PREVIEW_MASK_STRIDE];
static uint8_t preview_mask_buf[2][FB_H][PREVIEW_MASK_STRIDE];
static uint8_t (*preview_previous_mask)[PREVIEW_MASK_STRIDE] = preview_mask_buf[0];
static uint8_t (*preview_current_mask)[PREVIEW_MASK_STRIDE] = preview_mask_buf[1];
static uint8_t preview_backdrop[PREVIEW_BACKDROP_H][PREVIEW_BACKDROP_W];
static bool preview_backdrop_x = false;
static bool preview_backdrop_y = false;

typedef struct {
    int x, y;
} blast_ring;

typedef struct {
    bool visible, tracked, spin, expand, in_front, edge_padding, slow_rise;
    uint32_t identity;
    int x, y;
    int left, right, top, bottom;
    uint8_t pixels[FB_H][FB_W];
    uint8_t occlusion[FB_H][OCC_STRIDE];
    unsigned ring_count;
    blast_ring rings[BLAST_RING_LIMIT];
    uint8_t ring_labels[BLAST_SIDE][BLAST_SIDE];
} preview_sprite;

/* 3 player slots swapped by pointer to avoid copying ~80 KB each tick. */
static preview_sprite preview_player_buf[3];
static preview_sprite *preview_player_pages[2] = { &preview_player_buf[0], &preview_player_buf[1] };
static preview_sprite *preview_current_player = &preview_player_buf[2];
static bool preview_previous_player_tracked = false;
static int preview_previous_player_x = 0;
static int preview_previous_player_y = 0;

typedef struct {
    preview_sprite *sprites;
    size_t count, capacity;
} preview_sprite_list;

static preview_sprite_list preview_star_pages[2];
static preview_sprite_list preview_stars;
static preview_sprite_list preview_previous_stars;
static uint8_t preview_star_terrain[2][FB_H][OCC_STRIDE];

/* ------------------------------------------------------------------------ */
/* Palette & fast LUT-based EGA planar de-interleaving                      */
/* ------------------------------------------------------------------------ */

static void init_ega_lut(void)
{
    unsigned b;
    if (ega_lut_ready) return;
    for (b = 0; b < 256u; ++b) {
        uint32_t lo = 0, hi = 0;
        if (b & 0x80u) lo |= 0x00000001UL;
        if (b & 0x40u) lo |= 0x00000100UL;
        if (b & 0x20u) lo |= 0x00010000UL;
        if (b & 0x10u) lo |= 0x01000000UL;
        if (b & 0x08u) hi |= 0x00000001UL;
        if (b & 0x04u) hi |= 0x00000100UL;
        if (b & 0x02u) hi |= 0x00010000UL;
        if (b & 0x01u) hi |= 0x01000000UL;
        ega_lut_lo[b] = lo;
        ega_lut_hi[b] = hi;
    }
    ega_lut_ready = true;
}

void dos_video_sync_palette_entry(uint8_t index, uint8_t color_value)
{
    uint8_t rgb[3];
    if (!vga_active || index >= 16) return;
    ega_palette_rgb(color_value, rgb);
    hw_vga_set_dac(index, (uint8_t)(rgb[0] >> 2), (uint8_t)(rgb[1] >> 2),
                   (uint8_t)(rgb[2] >> 2));
}

void dos_video_sync_all_palette(void)
{
    uint8_t i;
    if (!vga_active) return;
    for (i = 0; i < 16; ++i) {
        dos_video_sync_palette_entry(i, ega.palette[i]);
    }
}

static void render_ega_indexed(uint8_t *out)
{
    const uint32_t base = ega.display_start;
    const uint8_t *p0 = ega.vram[0] + base;
    const uint8_t *p1 = ega.vram[1] + base;
    const uint8_t *p2 = ega.vram[2] + base;
    const uint8_t *p3 = ega.vram[3] + base;
    uint32_t *dst32 = (uint32_t *)out;
    uint32_t i;

    for (i = 0; i < EGA_PAGE_BYTES; ++i) {
        uint8_t b0 = p0[i];
        uint8_t b1 = p1[i];
        uint8_t b2 = p2[i];
        uint8_t b3 = p3[i];
        dst32[0] = ega_lut_lo[b0] | (ega_lut_lo[b1] << 1) |
                   (ega_lut_lo[b2] << 2) | (ega_lut_lo[b3] << 3);
        dst32[1] = ega_lut_hi[b0] | (ega_lut_hi[b1] << 1) |
                   (ega_lut_hi[b2] << 2) | (ega_lut_hi[b3] << 3);
        dst32 += 2;
    }
}

static void capture_preview_backdrop(void)
{
    unsigned y, x;
    for (y = 0; y < PREVIEW_BACKDROP_H; ++y) {
        unsigned ty = (y >> 3) * 40u;
        unsigned ry = y & 7u;
        uint32_t *dst32 = (uint32_t *)preview_backdrop[y];
        for (x = 0; x < 40u; ++x) {
            unsigned addr = PREVIEW_BACKDROP_BASE + (ty + x) * 8u + ry;
            uint8_t b0 = ega.vram[0][addr];
            uint8_t b1 = ega.vram[1][addr];
            uint8_t b2 = ega.vram[2][addr];
            uint8_t b3 = ega.vram[3][addr];
            dst32[0] = ega_lut_lo[b0] | (ega_lut_lo[b1] << 1) |
                       (ega_lut_lo[b2] << 2) | (ega_lut_lo[b3] << 3);
            dst32[1] = ega_lut_hi[b0] | (ega_lut_hi[b1] << 1) |
                       (ega_lut_hi[b2] << 2) | (ega_lut_hi[b3] << 3);
            dst32 += 2;
        }
    }
    dos_backdrop_dirty = 0;
}

/* ------------------------------------------------------------------------ */
/* Smooth Gameplay sprite capture (bounding-box reset avoids 64 KB memsets) */
/* ------------------------------------------------------------------------ */

static void reset_preview_sprite(preview_sprite *sprite, bool clear_occlusion)
{
    int y;
    if (sprite->right > sprite->left && sprite->bottom > sprite->top) {
        size_t span = (size_t)(sprite->right - sprite->left);
        for (y = sprite->top; y < sprite->bottom; ++y) {
            memset(&sprite->pixels[y][sprite->left], 0, span);
        }
        if (sprite->expand) {
            int w = imin(BLAST_SIDE, sprite->right - sprite->left);
            int h = imin(BLAST_SIDE, sprite->bottom - sprite->top);
            for (y = 0; y < h; ++y) {
                memset(sprite->ring_labels[y], 0, (size_t)w);
            }
        }
    }
    if (clear_occlusion) {
        memset(sprite->occlusion[MOTION_WORLD_TOP], 0,
               (size_t)MOTION_WORLD_ROWS * OCC_STRIDE);
    }
    sprite->visible = false;
    sprite->tracked = false;
    sprite->spin = false;
    sprite->expand = false;
    sprite->in_front = false;
    sprite->edge_padding = false;
    sprite->slow_rise = false;
    sprite->identity = 0;
    sprite->x = 0;
    sprite->y = 0;
    sprite->left = 0;
    sprite->right = 0;
    sprite->top = 0;
    sprite->bottom = 0;
    sprite->ring_count = 0;
}

static bool reserve_preview_stars(preview_sprite_list *list, size_t count)
{
    size_t capacity;
    preview_sprite *sprites;
    if (list->capacity >= count) return true;
    capacity = list->capacity ? list->capacity * 2 : 8;
    if (capacity < count) capacity = count;
    sprites = (preview_sprite *)realloc(list->sprites, capacity * sizeof *sprites);
    if (!sprites) return false;
    memset(sprites + list->capacity, 0, (capacity - list->capacity) * sizeof *sprites);
    list->sprites = sprites;
    list->capacity = capacity;
    return true;
}

static void identify_blast_rings(preview_sprite *sprite)
{
    static uint16_t queue[BLAST_SIDE * BLAST_SIDE];
    int width = sprite->right - sprite->left;
    int height = sprite->bottom - sprite->top;
    int y, x;
    if (!sprite->expand || width <= 0 || height <= 0 ||
        width > BLAST_SIDE || height > BLAST_SIDE) return;

    for (y = 0; y < height; ++y) {
        for (x = 0; x < width; ++x) {
            unsigned label, read, write, sum_x, sum_y;
            if (sprite->ring_labels[y][x] ||
                !sprite->pixels[sprite->top + y][sprite->left + x]) continue;
            if (sprite->ring_count == BLAST_RING_LIMIT) return;
            label = ++sprite->ring_count;
            read = write = sum_x = sum_y = 0;
            queue[write++] = (uint16_t)(y * BLAST_SIDE + x);
            sprite->ring_labels[y][x] = (uint8_t)label;
            while (read < write) {
                unsigned point = queue[read++];
                int px = (int)(point % BLAST_SIDE);
                int py = (int)(point / BLAST_SIDE);
                int ny, nx;
                sum_x += (unsigned)px;
                sum_y += (unsigned)py;
                for (ny = imax(0, py - 1); ny <= imin(height - 1, py + 1); ++ny) {
                    for (nx = imax(0, px - 1); nx <= imin(width - 1, px + 1); ++nx) {
                        if (sprite->ring_labels[ny][nx] ||
                            !sprite->pixels[sprite->top + ny][sprite->left + nx]) continue;
                        sprite->ring_labels[ny][nx] = (uint8_t)label;
                        queue[write++] = (uint16_t)(ny * BLAST_SIDE + nx);
                    }
                }
            }
            sprite->rings[label - 1].x = sprite->left + (int)((sum_x + write / 2) / write);
            sprite->rings[label - 1].y = sprite->top + (int)((sum_y + write / 2) / write);
        }
    }
}

uint8_t *video_preview_mask_row(unsigned page, unsigned y)
{
    if (!smooth_enabled || page >= 2 || y >= FB_H) return NULL;
    return preview_page_mask[page][y];
}

void video_preview_begin_player(unsigned page)
{
    if (!smooth_enabled || page >= 2) return;
    reset_preview_sprite(preview_player_pages[page], true);
    preview_star_pages[page].count = 0;
    memset(preview_star_terrain[page][MOTION_WORLD_TOP], 0,
           (size_t)MOTION_WORLD_ROWS * OCC_STRIDE);
}

bool video_preview_player_begin(unsigned page, int x, int y, bool in_front)
{
    preview_sprite *player;
    if (!smooth_enabled || page >= 2) return false;
    player = preview_player_pages[page];
    player->visible = true;
    player->in_front = in_front;
    player->edge_padding = in_front;
    if (in_front) {
        memset(player->occlusion[MOTION_WORLD_TOP], 0,
               (size_t)MOTION_WORLD_ROWS * OCC_STRIDE);
    }
    video_preview_player_track(page, x, y);
    return true;
}

void video_preview_player_track(unsigned page, int x, int y)
{
    preview_sprite *player;
    if (!smooth_enabled || page >= 2) return;
    player = preview_player_pages[page];
    player->tracked = true;
    player->x = x;
    player->y = y;
}

static void preview_sprite_tile(preview_sprite *sprite, const uint8_t *pixels,
                                unsigned x, unsigned y, bool white)
{
    unsigned row, column;
    int base_x, base_y;
    if (x >= FB_W / 8 || y >= FB_H / 8) return;
    base_x = (int)(x << 3);
    base_y = (int)(y << 3);
    for (row = 0; row < 8; ++row) {
        const uint8_t *tile = pixels + row * 5;
        uint8_t mask = tile[0];
        uint8_t *dst_row;
        int pixel_y;
        if (mask == 0xFF) continue;
        pixel_y = base_y + (int)row;
        dst_row = sprite->pixels[pixel_y];
        for (column = 0; column < 8; ++column) {
            unsigned bit = 0x80u >> column;
            unsigned color;
            int pixel_x;
            if (mask & bit) continue;
            if (white) {
                color = 15;
            } else {
                color = 0;
                if (tile[1] & bit) color |= 1u;
                if (tile[2] & bit) color |= 2u;
                if (tile[3] & bit) color |= 4u;
                if (tile[4] & bit) color |= 8u;
            }
            pixel_x = base_x + (int)column;
            dst_row[pixel_x] = (uint8_t)(color + 1);
            if (sprite->right == 0 || pixel_x < sprite->left) sprite->left = pixel_x;
            if (pixel_x >= sprite->right) sprite->right = pixel_x + 1;
            if (sprite->bottom == 0 || pixel_y < sprite->top) sprite->top = pixel_y;
            if (pixel_y >= sprite->bottom) sprite->bottom = pixel_y + 1;
        }
    }
}

void video_preview_player_tile(unsigned page, const uint8_t *pixels,
                               unsigned x, unsigned y, bool white)
{
    if (page < 2) preview_sprite_tile(preview_player_pages[page], pixels, x, y, white);
}

void video_preview_player_occlude(unsigned page, unsigned x, unsigned y, uint8_t bits)
{
    if (smooth_enabled && page < 2 && x < OCC_STRIDE && y < FB_H)
        preview_player_pages[page]->occlusion[y][x] |= bits;
}

bool video_preview_star_begin(unsigned page, uint32_t identity, int x, int y,
                              bool spin, bool in_front, bool expand, bool edge_padding)
{
    preview_sprite_list *list;
    preview_sprite *star;
    if (!smooth_enabled || page >= 2) return false;
    list = &preview_star_pages[page];
    if (!reserve_preview_stars(list, list->count + 1)) return false;
    star = &list->sprites[list->count++];
    reset_preview_sprite(star, in_front);
    if (!in_front) {
        memcpy(star->occlusion[MOTION_WORLD_TOP],
               preview_star_terrain[page][MOTION_WORLD_TOP],
               (size_t)MOTION_WORLD_ROWS * OCC_STRIDE);
    }
    star->visible = true;
    star->spin = spin;
    star->expand = expand;
    star->edge_padding = edge_padding;
    star->identity = identity;
    star->x = x;
    star->y = y;
    return true;
}

void video_preview_effect_mark_slow_rise(unsigned page)
{
    preview_sprite_list *list;
    if (page >= 2) return;
    list = &preview_star_pages[page];
    if (list->count) list->sprites[list->count - 1].slow_rise = true;
}

void video_preview_star_tile(unsigned page, const uint8_t *pixels,
                             unsigned x, unsigned y, bool white)
{
    preview_sprite_list *list;
    if (page >= 2) return;
    list = &preview_star_pages[page];
    if (list->count)
        preview_sprite_tile(&list->sprites[list->count - 1], pixels, x, y, white);
}

void video_preview_star_occlude(unsigned page, unsigned x, unsigned y, uint8_t bits)
{
    preview_sprite_list *list;
    size_t i;
    if (!smooth_enabled || page >= 2 || x >= OCC_STRIDE || y < MOTION_WORLD_TOP ||
        y >= MOTION_WORLD_BOTTOM) return;
    list = &preview_star_pages[page];
    if (!list->count) preview_star_terrain[page][y][x] |= bits;
    for (i = 0; i < list->count; ++i)
        list->sprites[i].occlusion[y][x] |= bits;
}

/* ------------------------------------------------------------------------ */
/* Smooth Gameplay 70 Hz compositor (8-bit indexed, division-free loops)    */
/* ------------------------------------------------------------------------ */

static void advance_preview_clock(uint64_t now, unsigned frame_ticks)
{
    uint64_t period = DOS_PIT_HZ * (uint64_t)frame_ticks / 140ULL;
    uint64_t next, correction;
    if (!period || motion_frame_count == 0 || period != preview_frame_period ||
        now - motion_current_time > period * 2 ||
        now - motion_current_time < period / 2) {
        preview_frame_period = period;
        preview_frame_start = now;
        return;
    }
    next = preview_frame_start + period;
    correction = period / 8;
    if (now >= next) next += u64min(now - next, correction);
    else next -= u64min(next - now, correction);
    preview_frame_start = next;
}

static inline void render_camera_span(uint8_t *dest, int x0, int x1, int src_x_offset,
                                      const uint8_t *src_row, const uint8_t *mask_row,
                                      const uint8_t *bd_row, int bx)
{
    int x;
    for (x = x0; x < x1; ++x) {
        int sx = x + src_x_offset;
        if (mask_row[sx >> 3] & (0x80u >> (sx & 7))) {
            dest[x] = src_row[sx];
        } else {
            dest[x] = bd_row[bx];
        }
        if (++bx == PREVIEW_BACKDROP_W) bx = 0;
    }
}

static void render_pixel_camera(uint8_t *out, int delta_x, int delta_y, double alpha)
{
    const int left = 8;
    const int right = FB_W - 8;
    const int shift_x = iround((1.0 - alpha) * delta_x);
    const int shift_y = iround((1.0 - alpha) * delta_y);
    int backdrop_x, backdrop_y, bx0, by, y;
    int cur_x0, cur_x1, prev_x_off, prev_x0, prev_x1;
    if (shift_x == 0 && shift_y == 0) return;

    backdrop_x = preview_backdrop_x
        ? (int)motion_current_camera_x * 4 - iround((1.0 - alpha) * delta_x * 0.5) : 0;
    backdrop_y = preview_backdrop_y
        ? (int)motion_current_camera_y * 4 - iround((1.0 - alpha) * delta_y * 0.5) : 0;

    bx0 = backdrop_x % PREVIEW_BACKDROP_W;
    if (bx0 < 0) bx0 += PREVIEW_BACKDROP_W;

    by = backdrop_y % PREVIEW_BACKDROP_H;
    if (by < 0) by += PREVIEW_BACKDROP_H;

    /* Partition [left, right) into current-frame and previous-frame spans. */
    cur_x0 = imax(left, left + shift_x);
    cur_x1 = imin(right, right + shift_x);
    prev_x_off = delta_x - shift_x;
    prev_x0 = imax(left, left - prev_x_off);
    prev_x1 = imin(right, right - prev_x_off);

    for (y = MOTION_WORLD_TOP; y < MOTION_WORLD_BOTTOM; ++y) {
        uint8_t *dest = out + y * FB_W;
        const uint8_t *bd_row = preview_backdrop[by];
        int sy_cur = y - shift_y;

        if (sy_cur >= MOTION_WORLD_TOP && sy_cur < MOTION_WORLD_BOTTOM) {
            int sy_prev = sy_cur + delta_y;
            bool prev_y_ok = (sy_prev >= MOTION_WORLD_TOP && sy_prev < MOTION_WORLD_BOTTOM);

            if (cur_x0 > left && prev_y_ok) {
                int s0 = imax(left, prev_x0);
                int s1 = imin(cur_x0, prev_x1);
                if (s0 < s1) {
                    int bx = bx0 + (s0 - left);
                    if (bx >= PREVIEW_BACKDROP_W) bx -= PREVIEW_BACKDROP_W;
                    render_camera_span(dest, s0, s1, prev_x_off,
                                       motion_previous + sy_prev * FB_W,
                                       preview_previous_mask[sy_prev], bd_row, bx);
                }
            }
            if (cur_x0 < cur_x1) {
                int bx = bx0 + (cur_x0 - left);
                if (bx >= PREVIEW_BACKDROP_W) bx -= PREVIEW_BACKDROP_W;
                render_camera_span(dest, cur_x0, cur_x1, -shift_x,
                                   motion_current + sy_cur * FB_W,
                                   preview_current_mask[sy_cur], bd_row, bx);
            }
            if (cur_x1 < right && prev_y_ok) {
                int s0 = imax(cur_x1, prev_x0);
                int s1 = imin(right, prev_x1);
                if (s0 < s1) {
                    int bx = bx0 + (s0 - left);
                    if (bx >= PREVIEW_BACKDROP_W) bx -= PREVIEW_BACKDROP_W;
                    render_camera_span(dest, s0, s1, prev_x_off,
                                       motion_previous + sy_prev * FB_W,
                                       preview_previous_mask[sy_prev], bd_row, bx);
                }
            }
        } else {
            int sy_prev = sy_cur + delta_y;
            if (sy_prev >= MOTION_WORLD_TOP && sy_prev < MOTION_WORLD_BOTTOM &&
                prev_x0 < prev_x1) {
                int bx = bx0 + (prev_x0 - left);
                if (bx >= PREVIEW_BACKDROP_W) bx -= PREVIEW_BACKDROP_W;
                render_camera_span(dest, prev_x0, prev_x1, prev_x_off,
                                   motion_previous + sy_prev * FB_W,
                                   preview_previous_mask[sy_prev], bd_row, bx);
            }
        }

        if (++by == PREVIEW_BACKDROP_H) by = 0;
    }
}

static void render_blast_rings(uint8_t *out, const preview_sprite *sprite,
                               const preview_sprite *previous, double alpha,
                               int shift_x, int shift_y, int camera_x, int camera_y)
{
    int offset_x[BLAST_RING_LIMIT + 1];
    int offset_y[BLAST_RING_LIMIT + 1];
    bool matched[BLAST_RING_LIMIT];
    bool new_pulse;
    int sy, sx;

    memset(offset_x, 0, sizeof offset_x);
    memset(offset_y, 0, sizeof offset_y);
    memset(matched, 0, sizeof matched);

    new_pulse = previous &&
        sprite->right - sprite->left + 8 < previous->right - previous->left;
    if (previous && previous->ring_count && !new_pulse) {
        int origin_dx = (sprite->x - previous->x) * 8;
        int origin_dy = (sprite->y - previous->y) * 8;
        unsigned i, j;
        for (i = 0; i < sprite->ring_count; ++i) {
            blast_ring ring = sprite->rings[i];
            int best = -1, distance = 17 * 17;
            for (j = 0; j < previous->ring_count; ++j) {
                int dx, dy, squared;
                if (matched[j]) continue;
                dx = previous->rings[j].x + origin_dx - ring.x;
                dy = previous->rings[j].y + origin_dy - ring.y;
                squared = dx * dx + dy * dy;
                if (squared < distance) {
                    distance = squared;
                    best = (int)j;
                }
            }
            if (best < 0) continue;
            matched[best] = true;
            offset_x[i + 1] = iround((1.0 - alpha) * (previous->rings[best].x + origin_dx - ring.x));
            offset_y[i + 1] = iround((1.0 - alpha) * (previous->rings[best].y + origin_dy - ring.y));
        }
    }

    for (sy = sprite->top; sy < sprite->bottom; ++sy) {
        const uint8_t *pix_row = sprite->pixels[sy];
        const uint8_t *lbl_row = sprite->ring_labels[sy - sprite->top];
        for (sx = sprite->left; sx < sprite->right; ++sx) {
            unsigned color = pix_row[sx];
            unsigned ring;
            int x, y, mx, my;
            if (!color) continue;
            ring = lbl_row[sx - sprite->left];
            x = sx + shift_x + offset_x[ring];
            y = sy + shift_y + offset_y[ring];
            if (x < 8 || x >= FB_W - 8 ||
                y < MOTION_WORLD_TOP || y >= MOTION_WORLD_BOTTOM) continue;
            mx = x - camera_x;
            my = y - camera_y;
            if (mx >= 8 && mx < FB_W - 8 && my >= MOTION_WORLD_TOP &&
                my < MOTION_WORLD_BOTTOM &&
                (sprite->occlusion[my][mx >> 3] & (0x80u >> (mx & 7)))) continue;
            out[y * FB_W + x] = (uint8_t)(color - 1);
        }
    }
}

static void render_preview_sprite(uint8_t *out, const preview_sprite *sprite,
                                  bool previous_visible, int previous_x, int previous_y,
                                  const preview_sprite *previous,
                                  int dx, int dy, double alpha, bool spin_half, int max_rise)
{
    int world_dx, world_dy, camera_x, camera_y, player_x, player_y;
    bool cropped_on_ascent;
    int left, right, top, bottom, first_x, end_x, first_y, end_y, y, x;

    if (!sprite->visible) return;
    world_dx = dx + sprite->x - previous_x;
    world_dy = dy + sprite->y - previous_y;
    camera_x = iround((1.0 - alpha) * dx * 8);
    camera_y = iround((1.0 - alpha) * dy * 8);
    player_x = camera_x;
    player_y = camera_y;
    cropped_on_ascent = !sprite->edge_padding && world_dy < 0 &&
                        sprite->top < MOTION_WORLD_TOP + 8;

    if (previous_visible && !cropped_on_ascent && abs(world_dx) <= 2 &&
        world_dy >= -max_rise && world_dy <= 2) {
        double vertical_offset = -(1.0 - alpha) * world_dy * 8;
        player_x -= iround((1.0 - alpha) * world_dx * 8);
        if (sprite->slow_rise && world_dy == -1)
            vertical_offset += alpha * 4.0;
        else if (previous && previous->slow_rise && world_dy == 0)
            vertical_offset = (1.0 - alpha) * 4.0;
        player_y += iround(vertical_offset);
    }

    if (sprite->right <= sprite->left || sprite->bottom <= sprite->top) return;
    if (sprite->expand && sprite->ring_count) {
        render_blast_rings(out, sprite, previous_visible ? previous : NULL, alpha,
                           player_x, player_y, camera_x, camera_y);
        return;
    }

    if (!spin_half) {
        /* Fast integer-only path for Cosmo, enemies, pickups, scores, debris. */
        first_x = imax(8, sprite->left + player_x);
        end_x = imin(FB_W - 8, sprite->right + player_x);
        first_y = imax(MOTION_WORLD_TOP, sprite->top + player_y);
        end_y = imin(MOTION_WORLD_BOTTOM, sprite->bottom + player_y);

        for (y = first_y; y < end_y; ++y) {
            int mask_y = y - camera_y;
            bool check_occ = (mask_y >= MOTION_WORLD_TOP && mask_y < MOTION_WORLD_BOTTOM);
            const uint8_t *src_row = sprite->pixels[y - player_y];
            const uint8_t *occ_row = check_occ ? sprite->occlusion[mask_y] : NULL;
            uint8_t *dst_row = out + y * FB_W;

            if (check_occ) {
                for (x = first_x; x < end_x; ++x) {
                    unsigned color = src_row[x - player_x];
                    int mask_x;
                    if (!color) continue;
                    mask_x = x - camera_x;
                    if (mask_x >= 8 && mask_x < FB_W - 8 &&
                        (occ_row[mask_x >> 3] & (0x80u >> (mask_x & 7))))
                        continue;
                    dst_row[x] = (uint8_t)(color - 1);
                }
            } else {
                for (x = first_x; x < end_x; ++x) {
                    unsigned color = src_row[x - player_x];
                    if (color) dst_row[x] = (uint8_t)(color - 1);
                }
            }
        }
        return;
    }

    /* Fixed-point 16.16 rotated path for midpoint spinning stars (9 deg). */
    {
        double center_x = (sprite->left + sprite->right - 1) * 0.5;
        double center_y = (sprite->top + sprite->bottom - 1) * 0.5;
        double half_w = (sprite->right - sprite->left) * 0.5;
        double half_h = (sprite->bottom - sprite->top) * 0.5;
        double extent_x = SPIN_COS * half_w + SPIN_SIN * half_h;
        double extent_y = SPIN_SIN * half_w + SPIN_COS * half_h;
        int32_t cx_fp = (int32_t)iround(center_x * 65536.0);
        int32_t cy_fp = (int32_t)iround(center_y * 65536.0);

        left = (int)floor(center_x - extent_x);
        right = (int)ceil(center_x + extent_x) + 1;
        top = (int)floor(center_y - extent_y);
        bottom = (int)ceil(center_y + extent_y) + 1;

        first_x = imax(8, left + player_x);
        end_x = imin(FB_W - 8, right + player_x);
        first_y = imax(MOTION_WORLD_TOP, top + player_y);
        end_y = imin(MOTION_WORLD_BOTTOM, bottom + player_y);

        for (y = first_y; y < end_y; ++y) {
            int mask_y = y - camera_y;
            bool check_occ = (mask_y >= MOTION_WORLD_TOP && mask_y < MOTION_WORLD_BOTTOM);
            const uint8_t *occ_row = check_occ ? sprite->occlusion[mask_y] : NULL;
            uint8_t *dst_row = out + y * FB_W;
            int32_t ry_fp = ((int32_t)(y - player_y) << 16) - cy_fp;
            int32_t rx0_fp = ((int32_t)(first_x - player_x) << 16) - cx_fp;
            int32_t sx_fp = cx_fp + (int32_t)(((int64_t)SPIN_COS_FP * rx0_fp + (int64_t)SPIN_SIN_FP * ry_fp) >> 16) + 32768;
            int32_t sy_fp = cy_fp + (int32_t)(((int64_t)SPIN_COS_FP * ry_fp - (int64_t)SPIN_SIN_FP * rx0_fp) >> 16) + 32768;

            for (x = first_x; x < end_x; ++x) {
                int source_x = sx_fp >> 16;
                int source_y = sy_fp >> 16;
                sx_fp += SPIN_COS_FP;
                sy_fp -= SPIN_SIN_FP;
                if (source_x < sprite->left || source_x >= sprite->right ||
                    source_y < sprite->top || source_y >= sprite->bottom) continue;
                {
                    unsigned color = sprite->pixels[source_y][source_x];
                    int mask_x;
                    if (!color) continue;
                    if (check_occ) {
                        mask_x = x - camera_x;
                        if (mask_x >= 8 && mask_x < FB_W - 8 &&
                            (occ_row[mask_x >> 3] & (0x80u >> (mask_x & 7))))
                            continue;
                    }
                    dst_row[x] = (uint8_t)(color - 1);
                }
            }
        }
    }
}

static void render_preview_sprites(uint8_t *out, int dx, int dy, double alpha)
{
    size_t i, j;

    render_preview_sprite(out, preview_current_player, preview_previous_player_tracked,
                          preview_previous_player_x, preview_previous_player_y, NULL,
                          dx, dy, alpha, false, 3);

    for (i = 0; i < preview_stars.count; ++i) {
        const preview_sprite *star = &preview_stars.sprites[i];
        const preview_sprite *previous = NULL;
        bool spin_half;
        for (j = 0; j < preview_previous_stars.count; ++j) {
            if (preview_previous_stars.sprites[j].identity == star->identity) {
                previous = &preview_previous_stars.sprites[j];
                break;
            }
        }
        spin_half = (star->spin && previous && alpha < 0.5);
        render_preview_sprite(out, star, previous != NULL,
                              previous ? previous->x : star->x,
                              previous ? previous->y : star->y,
                              previous, dx, dy, alpha, spin_half, 2);
    }
}

static bool render_motion_frame(uint8_t *out, uint64_t now)
{
    bool sprite_visible;
    int dx, dy;
    double alpha;

    if (!smooth_enabled || pause_depth || motion_frame_count == 0) return false;

    sprite_visible = preview_current_player->visible || preview_stars.count != 0;
    if (sprite_visible && (motion_frame_count == 1 || !motion_camera_valid)) {
        memcpy(out, motion_current, FB_PIXELS);
        render_preview_sprites(out, 0, 0, 1.0);
        return true;
    }
    if (motion_frame_count < 2 || !motion_camera_valid ||
        motion_current_time <= motion_previous_time) {
        return false;
    }

    memcpy(out, motion_current, FB_PIXELS);
    if (preview_frame_period != 0) {
        uint64_t elapsed = now > preview_frame_start ? now - preview_frame_start : 0;
        alpha = (double)u64min(elapsed, preview_frame_period) / (double)preview_frame_period;
    } else {
        uint64_t interval = motion_current_time - motion_previous_time;
        uint64_t elapsed = now > motion_current_time ? now - motion_current_time : 0;
        alpha = (double)u64min(elapsed, interval) / (double)interval;
    }

    dx = (int)motion_current_camera_x - (int)motion_previous_camera_x;
    dy = (int)motion_current_camera_y - (int)motion_previous_camera_y;
    render_pixel_camera(out, dx * 8, dy * 8, alpha);
    render_preview_sprites(out, dx, dy, alpha);
    return true;
}

/* ------------------------------------------------------------------------ */
/* Public video interface                                                   */
/* ------------------------------------------------------------------------ */

bool video_init(const char *title, int scale, bool widescreen)
{
    (void)title;
    (void)scale;
    (void)widescreen;
    init_ega_lut();
    hw_vga_set_mode(0x13);
    vga_active = true;
    dos_video_sync_all_palette();
    dos_ega_dirty = 1;
    dos_backdrop_dirty = 1;
    next_present_ticks = hw_read_pit_ticks() + FRAME_PERIOD_70HZ;
    last_idle_irq = hw_irq8_count();
    return true;
}

void video_shutdown(void)
{
    if (vga_active) {
        hw_vga_set_mode(0x03);
        vga_active = false;
    }
    free(preview_star_pages[0].sprites);
    free(preview_star_pages[1].sprites);
    free(preview_stars.sprites);
    free(preview_previous_stars.sprites);
    memset(preview_star_pages, 0, sizeof preview_star_pages);
    memset(&preview_stars, 0, sizeof preview_stars);
    memset(&preview_previous_stars, 0, sizeof preview_previous_stars);
}

bool video_is_widescreen(void)
{
    return false;
}

void video_set_motion_interpolation(bool enabled)
{
    smooth_enabled = enabled;
    motion_frame_count = 0;
    preview_current_player->visible = false;
    preview_current_player->tracked = false;
    preview_stars.count = 0;
    preview_previous_stars.count = 0;
}

void video_pause_motion_interpolation(bool paused)
{
    bool was_paused = pause_depth != 0;
    if (paused) ++pause_depth;
    else if (pause_depth) --pause_depth;
    if (was_paused != (pause_depth != 0)) {
        motion_frame_count = 0;
        dos_ega_dirty = 1;
        if (pause_depth) video_present();
    }
}

void video_capture_motion_frame(uint16_t scroll_x, uint16_t scroll_y,
                                bool backdrop_x, bool backdrop_y, unsigned frame_ticks)
{
    uint64_t now;
    unsigned page;
    size_t i;
    uint8_t *tmp_fb;

    if (!smooth_enabled || pause_depth) {
        dos_ega_dirty = 1;
        video_present();
        return;
    }

    /* Ping-pong framebuffers in O(1) and unpack directly into motion_current. */
    if (motion_frame_count != 0) {
        tmp_fb = motion_previous;
        motion_previous = motion_current;
        motion_current = tmp_fb;
        motion_previous_time = motion_current_time;
        motion_previous_camera_x = motion_current_camera_x;
        motion_previous_camera_y = motion_current_camera_y;
    } else {
        motion_previous_camera_x = scroll_x;
        motion_previous_camera_y = scroll_y;
    }
    render_ega_indexed(motion_current);

    page = ega.display_start / EGA_PAGE_SIZE;
    if (page < 2) {
        uint8_t (*tmp_mask)[PREVIEW_MASK_STRIDE];
        preview_sprite *tmp_player;
        preview_sprite_list recycled;

        tmp_mask = preview_previous_mask;
        preview_previous_mask = preview_current_mask;
        preview_current_mask = tmp_mask;
        memcpy(preview_current_mask[MOTION_WORLD_TOP],
               preview_page_mask[page][MOTION_WORLD_TOP],
               (size_t)MOTION_WORLD_ROWS * PREVIEW_MASK_STRIDE);

        preview_backdrop_x = backdrop_x;
        preview_backdrop_y = backdrop_y;
        if (dos_backdrop_dirty) capture_preview_backdrop();

        preview_previous_player_tracked = motion_frame_count &&
            preview_current_player->tracked &&
            preview_current_player->in_front == preview_player_pages[page]->in_front;
        preview_previous_player_x = preview_current_player->x;
        preview_previous_player_y = preview_current_player->y;
        tmp_player = preview_current_player;
        preview_current_player = preview_player_pages[page];
        preview_player_pages[page] = tmp_player;

        recycled = preview_previous_stars;
        preview_previous_stars = preview_stars;
        if (!motion_frame_count) preview_previous_stars.count = 0;
        preview_stars = preview_star_pages[page];
        for (i = 0; i < preview_stars.count; ++i)
            identify_blast_rings(&preview_stars.sprites[i]);
        preview_star_pages[page] = recycled;
        preview_star_pages[page].count = 0;
    }

    now = hw_read_pit_ticks();
    advance_preview_clock(now, frame_ticks);

    motion_current_time = now;
    motion_current_camera_x = scroll_x;
    motion_current_camera_y = scroll_y;
    motion_camera_valid = abs((int)motion_current_camera_x - (int)motion_previous_camera_x) <= 2 &&
                          abs((int)motion_current_camera_y - (int)motion_previous_camera_y) <= 2;
    if (motion_frame_count < 2) ++motion_frame_count;
    if (diag_enabled) {
        if (!diag_start_ticks) diag_start_ticks = now;
        ++diag_game_flips;
        if (diag_game_flips % 15 == 0) diag_flush_log(now);
    }

    /* Present immediately so the start of the tick appears on time. */
    video_present();
}

void dos_video_drop_motion(void)
{
    motion_frame_count = 0;
}

void video_present(void)
{
    uint64_t now;
    bool smooth_frame = false;
    if (!vga_active) return;
    now = hw_read_pit_ticks();
    if (render_motion_frame(motion_output, now)) {
        memcpy(hw_vga_framebuffer(), motion_output, FB_PIXELS);
        smooth_frame = true;
    } else {
        render_ega_indexed(motion_output);
        memcpy(hw_vga_framebuffer(), motion_output, FB_PIXELS);
    }
    if (diag_enabled && diag_start_ticks) {
        ++diag_present_count;
        if (smooth_frame) ++diag_smooth_count;
    }
    dos_ega_dirty = 0;
    next_present_ticks = now + FRAME_PERIOD_70HZ;
}

static inline void advance_present_schedule(uint64_t now)
{
    next_present_ticks += FRAME_PERIOD_70HZ;
    if (next_present_ticks <= now) {
        next_present_ticks = now + FRAME_PERIOD_70HZ;
    }
}

void dos_video_idle(void)
{
    uint32_t irq;
    uint64_t now;

    if (!vga_active || hw_in_isr()) return;

    /* Avoid PIT latch port I/O & cli/sti spam between timer interrupts. */
    irq = hw_irq8_count();
    if (irq == last_idle_irq && !dos_ega_dirty) return;
    last_idle_irq = irq;

    now = hw_read_pit_ticks();
    if (now < next_present_ticks) return;

    if (smooth_enabled && !pause_depth && motion_frame_count >= 1) {
        if (render_motion_frame(motion_output, now)) {
            memcpy(hw_vga_framebuffer(), motion_output, FB_PIXELS);
            if (diag_enabled && diag_start_ticks) {
                ++diag_present_count;
                ++diag_smooth_count;
                if (!diag_snap_game_done && diag_game_flips >= 25) {
                    diag_snap_game_done = 1;
                    diag_write_ppm("SNAP2.PPM", motion_output);
                    diag_flush_log(now);
                }
            }
            advance_present_schedule(now);
            dos_ega_dirty = 0;
            return;
        }
    }

    if (dos_ega_dirty) {
        render_ega_indexed(motion_output);
        memcpy(hw_vga_framebuffer(), motion_output, FB_PIXELS);
        dos_ega_dirty = 0;
        advance_present_schedule(now);
    }
    if (diag_enabled && !diag_snap_title_done && now > DOS_PIT_HZ * 9ULL &&
        ega.palette[15] != 0) {
        diag_snap_title_done = 1;
        render_ega_indexed(motion_output);
        diag_write_ppm("SNAP1.PPM", motion_output);
    }
}
