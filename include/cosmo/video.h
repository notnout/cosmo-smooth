/*
 * video.h -- VGA Mode 13h presentation and Smooth Gameplay compositor API.
 */

#ifndef COSMO_VIDEO_H
#define COSMO_VIDEO_H

#include <stdbool.h>
#include <stdint.h>

extern volatile int dos_ega_dirty;
extern volatile int dos_backdrop_dirty;

bool video_init(const char *title, int scale, bool widescreen);
void video_shutdown(void);
bool video_is_widescreen(void);
void video_present(void);

void dos_video_idle(void);
void dos_video_drop_motion(void);
void dos_video_enable_diag(void);
void dos_video_sync_palette_entry(uint8_t index, uint8_t color_value);
void dos_video_sync_all_palette(void);

void video_set_motion_interpolation(bool enabled);
void video_pause_motion_interpolation(bool paused);
void video_capture_motion_frame(uint16_t scroll_x, uint16_t scroll_y,
                                bool backdrop_x, bool backdrop_y, unsigned frame_ticks);

uint8_t *video_preview_mask_row(unsigned page, unsigned y);
void video_preview_begin_player(unsigned page);
bool video_preview_player_begin(unsigned page, int x, int y, bool in_front);
void video_preview_player_track(unsigned page, int x, int y);
void video_preview_player_tile(unsigned page, const uint8_t *pixels,
                               unsigned x, unsigned y, bool white);
void video_preview_player_occlude(unsigned page, unsigned x, unsigned y, uint8_t bits);
bool video_preview_star_begin(unsigned page, uint32_t identity, int x, int y,
                              bool spin, bool in_front, bool expand, bool edge_padding);
void video_preview_effect_mark_slow_rise(unsigned page);
void video_preview_star_tile(unsigned page, const uint8_t *pixels,
                             unsigned x, unsigned y, bool white);
void video_preview_star_occlude(unsigned page, unsigned x, unsigned y, uint8_t bits);

#endif /* COSMO_VIDEO_H */
