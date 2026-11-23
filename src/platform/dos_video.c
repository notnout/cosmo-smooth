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

/* Match lowlevel_ega.c's column stride even in 320px Mode 13h. */
#define WIDE_PRESENT_W (EGA_SCREEN_W + EGA_WIDE_COLS * 8)
#define PREVIEW_MASK_STRIDE (WIDE_PRESENT_W / 8)
#define PREVIEW_BACKDROP_W 320
#define PREVIEW_BACKDROP_H 144
#define PREVIEW_BACKDROP_BASE 0xA300u

#define BLAST_SIDE 128
#define BLAST_RING_LIMIT 24

/* 70.08 Hz VGA Mode 13h frame period in 1,193,182 Hz PIT ticks (~14.27 ms). */
#define FRAME_PERIOD_70HZ 17026ULL

static inline int imin(int a, int b) { return a < b ? a : b; }
static inline int imax(int a, int b) { return a > b ? a : b; }
static inline uint64_t u64min(uint64_t a, uint64_t b) { return a < b ? a : b; }
static inline int iround(double v) { return (int)(v >= 0.0 ? floor(v + 0.5) : ceil(v - 0.5)); }

volatile int dos_ega_dirty = 1;
static bool vga_active = false;
static bool smooth_enabled = true;
static unsigned pause_depth = 0;

static uint8_t motion_capture[FB_PIXELS];
static uint8_t motion_previous[FB_PIXELS];
static uint8_t motion_current[FB_PIXELS];
static uint8_t motion_output[FB_PIXELS];

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
static uint64_t last_present_ticks = 0;
static int prev_retrace = 0;

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
static uint8_t preview_previous_mask[FB_H][PREVIEW_MASK_STRIDE];
static uint8_t preview_current_mask[FB_H][PREVIEW_MASK_STRIDE];
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
    uint8_t occlusion[FB_H][PREVIEW_MASK_STRIDE];
    unsigned ring_count;
    blast_ring rings[BLAST_RING_LIMIT];
    uint8_t ring_labels[BLAST_SIDE][BLAST_SIDE];
} preview_sprite;

static preview_sprite preview_player_pages[2];
static preview_sprite preview_current_player;
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
static uint8_t preview_star_terrain[2][FB_H][PREVIEW_MASK_STRIDE];

/* ------------------------------------------------------------------------ */
/* Palette & EGA planar de-interleaving                                     */
/* ------------------------------------------------------------------------ */

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
    uint32_t i;

    for (i = 0; i < EGA_PAGE_BYTES; ++i) {
        uint8_t b0 = p0[i];
        uint8_t b1 = p1[i];
        uint8_t b2 = p2[i];
        uint8_t b3 = p3[i];
        uint8_t *dst = out + (i << 3);

        dst[0] = (uint8_t)(((b0 >> 7) & 1) | (((b1 >> 7) & 1) << 1) | (((b2 >> 7) & 1) << 2) | (((b3 >> 7) & 1) << 3));
        dst[1] = (uint8_t)(((b0 >> 6) & 1) | (((b1 >> 6) & 1) << 1) | (((b2 >> 6) & 1) << 2) | (((b3 >> 6) & 1) << 3));
        dst[2] = (uint8_t)(((b0 >> 5) & 1) | (((b1 >> 5) & 1) << 1) | (((b2 >> 5) & 1) << 2) | (((b3 >> 5) & 1) << 3));
        dst[3] = (uint8_t)(((b0 >> 4) & 1) | (((b1 >> 4) & 1) << 1) | (((b2 >> 4) & 1) << 2) | (((b3 >> 4) & 1) << 3));
        dst[4] = (uint8_t)(((b0 >> 3) & 1) | (((b1 >> 3) & 1) << 1) | (((b2 >> 3) & 1) << 2) | (((b3 >> 3) & 1) << 3));
        dst[5] = (uint8_t)(((b0 >> 2) & 1) | (((b1 >> 2) & 1) << 1) | (((b2 >> 2) & 1) << 2) | (((b3 >> 2) & 1) << 3));
        dst[6] = (uint8_t)(((b0 >> 1) & 1) | (((b1 >> 1) & 1) << 1) | (((b2 >> 1) & 1) << 2) | (((b3 >> 1) & 1) << 3));
        dst[7] = (uint8_t)((b0 & 1)        | ((b1 & 1) << 1)        | ((b2 & 1) << 2)        | ((b3 & 1) << 3));
    }
}

static void capture_preview_backdrop(void)
{
    unsigned y, x;
    for (y = 0; y < PREVIEW_BACKDROP_H; ++y) {
        unsigned ty = (y >> 3) * 40u;
        unsigned ry = y & 7u;
        for (x = 0; x < 40u; ++x) {
            unsigned addr = PREVIEW_BACKDROP_BASE + (ty + x) * 8u + ry;
            uint8_t b0 = ega.vram[0][addr];
            uint8_t b1 = ega.vram[1][addr];
            uint8_t b2 = ega.vram[2][addr];
            uint8_t b3 = ega.vram[3][addr];
            uint8_t *dst = &preview_backdrop[y][x << 3];
            unsigned bit;
            for (bit = 0; bit < 8; ++bit) {
                unsigned shift = 7u - bit;
                dst[bit] = (uint8_t)(((b0 >> shift) & 1u) |
                                     (((b1 >> shift) & 1u) << 1) |
                                     (((b2 >> shift) & 1u) << 2) |
                                     (((b3 >> shift) & 1u) << 3));
            }
        }
    }
}

/* ------------------------------------------------------------------------ */
/* Smooth Gameplay sprite capture                                           */
/* ------------------------------------------------------------------------ */

static bool reserve_preview_stars(preview_sprite_list *list, size_t count)
{
    size_t capacity;
    preview_sprite *sprites;
    if (list->capacity >= count) return true;
    capacity = list->capacity ? list->capacity * 2 : 8;
    if (capacity < count) capacity = count;
    sprites = (preview_sprite *)realloc(list->sprites, capacity * sizeof *sprites);
    if (!sprites) return false;
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
    memset(&preview_player_pages[page], 0, sizeof preview_player_pages[page]);
    preview_star_pages[page].count = 0;
    memset(preview_star_terrain[page], 0, sizeof preview_star_terrain[page]);
}

bool video_preview_player_begin(unsigned page, int x, int y, bool in_front)
{
    if (!smooth_enabled || page >= 2) return false;
    preview_player_pages[page].visible = true;
    preview_player_pages[page].in_front = in_front;
    preview_player_pages[page].edge_padding = in_front;
    if (in_front) {
        memset(preview_player_pages[page].occlusion, 0,
               sizeof preview_player_pages[page].occlusion);
    }
    video_preview_player_track(page, x, y);
    return true;
}

void video_preview_player_track(unsigned page, int x, int y)
{
    if (!smooth_enabled || page >= 2) return;
    preview_player_pages[page].tracked = true;
    preview_player_pages[page].x = x;
    preview_player_pages[page].y = y;
}

static void preview_sprite_tile(preview_sprite *sprite, const uint8_t *pixels,
                                unsigned x, unsigned y, bool white)
{
    unsigned row, column;
    if (x >= FB_W / 8 || y >= FB_H / 8) return;
    for (row = 0; row < 8; ++row) {
        const uint8_t *tile = pixels + row * 5;
        for (column = 0; column < 8; ++column) {
            unsigned bit = 0x80u >> column;
            unsigned color;
            int pixel_x, pixel_y;
            if (tile[0] & bit) continue;
            if (white) {
                color = 15;
            } else {
                color = 0;
                if (tile[1] & bit) color |= 1u;
                if (tile[2] & bit) color |= 2u;
                if (tile[3] & bit) color |= 4u;
                if (tile[4] & bit) color |= 8u;
            }
            pixel_x = (int)(x * 8 + column);
            pixel_y = (int)(y * 8 + row);
            sprite->pixels[pixel_y][pixel_x] = (uint8_t)(color + 1);
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
    if (page < 2) preview_sprite_tile(&preview_player_pages[page], pixels, x, y, white);
}

void video_preview_player_occlude(unsigned page, unsigned x, unsigned y, uint8_t bits)
{
    if (smooth_enabled && page < 2 && x < PREVIEW_MASK_STRIDE && y < FB_H)
        preview_player_pages[page].occlusion[y][x] |= bits;
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
    memset(star, 0, sizeof *star);
    if (!in_front)
        memcpy(star->occlusion, preview_star_terrain[page], sizeof star->occlusion);
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
    if (!smooth_enabled || page >= 2 || x >= PREVIEW_MASK_STRIDE || y >= FB_H) return;
    list = &preview_star_pages[page];
    if (!list->count) preview_star_terrain[page][y][x] |= bits;
    for (i = 0; i < list->count; ++i)
        list->sprites[i].occlusion[y][x] |= bits;
}

/* ------------------------------------------------------------------------ */
/* Smooth Gameplay 70 Hz compositor (8-bit indexed)                         */
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

static void render_pixel_camera(uint8_t *out, int delta_x, int delta_y, double alpha)
{
    const int left = 8;
    const int right = FB_W - 8;
    const int shift_x = iround((1.0 - alpha) * delta_x);
    const int shift_y = iround((1.0 - alpha) * delta_y);
    int backdrop_x, backdrop_y, y, x;
    if (shift_x == 0 && shift_y == 0) return;

    backdrop_x = preview_backdrop_x
        ? (int)motion_current_camera_x * 4 - iround((1.0 - alpha) * delta_x * 0.5) : 0;
    backdrop_y = preview_backdrop_y
        ? (int)motion_current_camera_y * 4 - iround((1.0 - alpha) * delta_y * 0.5) : 0;

    for (y = MOTION_WORLD_TOP; y < MOTION_WORLD_BOTTOM; ++y) {
        uint8_t *dest = out + y * FB_W;
        int by = (y - MOTION_WORLD_TOP + backdrop_y) % PREVIEW_BACKDROP_H;
        if (by < 0) by += PREVIEW_BACKDROP_H;
        for (x = left; x < right; ++x) {
            int source_x = x - shift_x;
            int source_y = y - shift_y;
            bool use_current = source_x >= left && source_x < right &&
                               source_y >= MOTION_WORLD_TOP && source_y < MOTION_WORLD_BOTTOM;
            const uint8_t *mask;
            if (!use_current) {
                source_x += delta_x;
                source_y += delta_y;
                if (source_x < left || source_x >= right ||
                    source_y < MOTION_WORLD_TOP || source_y >= MOTION_WORLD_BOTTOM) continue;
            }
            mask = use_current ? preview_current_mask[source_y] : preview_previous_mask[source_y];
            if (mask[source_x >> 3] & (0x80u >> (source_x & 7))) {
                const uint8_t *src_fb = use_current ? motion_current : motion_previous;
                dest[x] = src_fb[source_y * FB_W + source_x];
            } else {
                int bx = (x - left + backdrop_x) % PREVIEW_BACKDROP_W;
                if (bx < 0) bx += PREVIEW_BACKDROP_W;
                dest[x] = preview_backdrop[by][bx];
            }
        }
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
        for (sx = sprite->left; sx < sprite->right; ++sx) {
            unsigned color = sprite->pixels[sy][sx];
            unsigned ring;
            int x, y, mx, my;
            if (!color) continue;
            ring = sprite->ring_labels[sy - sprite->top][sx - sprite->left];
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
                                  int dx, int dy, double alpha, double angle, int max_rise)
{
    int world_dx, world_dy, camera_x, camera_y, player_x, player_y;
    bool cropped_on_ascent;
    double center_x, center_y, c, s;
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

    center_x = (sprite->left + sprite->right - 1) * 0.5;
    center_y = (sprite->top + sprite->bottom - 1) * 0.5;
    left = sprite->left;
    right = sprite->right;
    top = sprite->top;
    bottom = sprite->bottom;
    c = 1.0;
    s = 0.0;

    if (angle != 0.0) {
        double half_w = (right - left) * 0.5;
        double half_h = (bottom - top) * 0.5;
        double extent_x, extent_y;
        c = cos(angle);
        s = sin(angle);
        extent_x = fabs(c) * half_w + fabs(s) * half_h;
        extent_y = fabs(s) * half_w + fabs(c) * half_h;
        left = (int)floor(center_x - extent_x);
        right = (int)ceil(center_x + extent_x) + 1;
        top = (int)floor(center_y - extent_y);
        bottom = (int)ceil(center_y + extent_y) + 1;
    }

    first_x = imax(8, left + player_x);
    end_x = imin(FB_W - 8, right + player_x);
    first_y = imax(MOTION_WORLD_TOP, top + player_y);
    end_y = imin(MOTION_WORLD_BOTTOM, bottom + player_y);

    for (y = first_y; y < end_y; ++y) {
        int mask_y = y - camera_y;
        uint8_t *dst_row = out + y * FB_W;
        for (x = first_x; x < end_x; ++x) {
            int source_x = x - player_x;
            int source_y = y - player_y;
            unsigned color;
            int mask_x;
            if (angle != 0.0) {
                double rx = source_x - center_x;
                double ry = source_y - center_y;
                source_x = iround(center_x + c * rx + s * ry);
                source_y = iround(center_y - s * rx + c * ry);
            }
            if (source_x < sprite->left || source_x >= sprite->right ||
                source_y < sprite->top || source_y >= sprite->bottom) continue;
            color = sprite->pixels[source_y][source_x];
            if (!color) continue;
            mask_x = x - camera_x;
            if (mask_x >= 8 && mask_x < FB_W - 8 &&
                mask_y >= MOTION_WORLD_TOP && mask_y < MOTION_WORLD_BOTTOM &&
                (sprite->occlusion[mask_y][mask_x >> 3] & (0x80u >> (mask_x & 7))))
                continue;
            dst_row[x] = (uint8_t)(color - 1);
        }
    }
}

static void render_preview_sprites(uint8_t *out, int dx, int dy, double alpha)
{
    const double pose_step = 0.3141592653589793; /* 18 degrees */
    size_t i, j;

    render_preview_sprite(out, &preview_current_player, preview_previous_player_tracked,
                          preview_previous_player_x, preview_previous_player_y, NULL,
                          dx, dy, alpha, 0.0, 3);

    for (i = 0; i < preview_stars.count; ++i) {
        const preview_sprite *star = &preview_stars.sprites[i];
        const preview_sprite *previous = NULL;
        double angle;
        for (j = 0; j < preview_previous_stars.count; ++j) {
            if (preview_previous_stars.sprites[j].identity == star->identity) {
                previous = &preview_previous_stars.sprites[j];
                break;
            }
        }
        angle = (star->spin && previous && alpha < 0.5) ? (pose_step * 0.5) : 0.0;
        render_preview_sprite(out, star, previous != NULL,
                              previous ? previous->x : star->x,
                              previous ? previous->y : star->y,
                              previous, dx, dy, alpha, angle, 2);
    }
}

static bool render_motion_frame(uint8_t *out, uint64_t now)
{
    bool sprite_visible;
    int dx, dy;
    double alpha;

    if (!smooth_enabled || pause_depth || motion_frame_count == 0) return false;

    sprite_visible = preview_current_player.visible || preview_stars.count != 0;
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
    hw_vga_set_mode(0x13);
    vga_active = true;
    dos_video_sync_all_palette();
    dos_ega_dirty = 1;
    last_present_ticks = hw_read_pit_ticks();
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
    preview_current_player.visible = false;
    preview_current_player.tracked = false;
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

    if (!smooth_enabled || pause_depth) {
        dos_ega_dirty = 1;
        video_present();
        return;
    }

    render_ega_indexed(motion_capture);
    page = ega.display_start / EGA_PAGE_SIZE;
    if (page < 2) {
        preview_sprite_list recycled;
        memcpy(preview_previous_mask, preview_current_mask, sizeof preview_previous_mask);
        memcpy(preview_current_mask, preview_page_mask[page], sizeof preview_current_mask);
        preview_backdrop_x = backdrop_x;
        preview_backdrop_y = backdrop_y;
        capture_preview_backdrop();

        preview_previous_player_tracked = motion_frame_count &&
            preview_current_player.tracked &&
            preview_current_player.in_front == preview_player_pages[page].in_front;
        preview_previous_player_x = preview_current_player.x;
        preview_previous_player_y = preview_current_player.y;
        preview_current_player = preview_player_pages[page];

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

    if (motion_frame_count != 0) {
        memcpy(motion_previous, motion_current, FB_PIXELS);
        motion_previous_time = motion_current_time;
        motion_previous_camera_x = motion_current_camera_x;
        motion_previous_camera_y = motion_current_camera_y;
    } else {
        motion_previous_camera_x = scroll_x;
        motion_previous_camera_y = scroll_y;
    }
    memcpy(motion_current, motion_capture, FB_PIXELS);
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
    last_present_ticks = now;
}

void dos_video_idle(void)
{
    uint64_t now;
    int retrace;
    bool retrace_edge;
    bool due;

    if (!vga_active || hw_in_isr()) return;

    now = hw_read_pit_ticks();
    retrace = hw_vga_in_retrace();
    retrace_edge = (retrace && !prev_retrace);
    prev_retrace = retrace;
    due = (now - last_present_ticks >= FRAME_PERIOD_70HZ) ||
          (retrace_edge && (now - last_present_ticks >= FRAME_PERIOD_70HZ / 2));

    if (!due) return;

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
            last_present_ticks = now;
            dos_ega_dirty = 0;
            return;
        }
    }

    if (dos_ega_dirty) {
        render_ega_indexed(motion_output);
        memcpy(hw_vga_framebuffer(), motion_output, FB_PIXELS);
        dos_ega_dirty = 0;
        last_present_ticks = now;
    }
    if (diag_enabled && !diag_snap_title_done && now > DOS_PIT_HZ * 9ULL &&
        ega.palette[15] != 0) {
        diag_snap_title_done = 1;
        render_ega_indexed(motion_output);
        diag_write_ppm("SNAP1.PPM", motion_output);
    }
}
