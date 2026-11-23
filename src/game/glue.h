/**
 * Cosmore
 * Copyright (c) 2020-2024 Scott Smitelli and contributors
 *
 * Based on COSMO{1..3}.EXE distributed with "Cosmo's Cosmic Adventure"
 * Copyright (c) 1992 Apogee Software, Ltd.
 *
 * This source code is licensed under the MIT license found in the LICENSE file
 * in the root directory of this source tree.
 */

/*****************************************************************************
 *                         COSMORE MAIN "GLUE" HEADER                        *
 *                                                                           *
 * The apparent intention of the author(s) of the original game was to have  *
 * every C function and variable in a single massive file. This is because   *
 * discrete C files compile into discrete OBJ files, and function calls that *
 * cross an OBJ file boundary become "far" calls which reload the            *
 * processor's CS register and take more clock cycles to execute. There is   *
 * too much code to compile as a single unit under DOS, so there are two     *
 * C files: GAME1 and GAME2.                                                 *
 *                                                                           *
 * This header file is literally the glue that holds these two files         *
 * together, declaring every include, typedef, variable, and function that   *
 * the pieces share.                                                         *
 *****************************************************************************/

#ifndef GLUE_H
#define GLUE_H

/* Replace the in-game status bar with a snapshot of some global variables */
/*#define DEBUG_BAR*/

/*
Enable this to prevent falling out of the map with god mode enabled. When
standing on the safety net, the jump command will allow long-jumping out.
*/
/*#define SAFETY_NET*/

/* Enable this to add vanity text inside the game */
/*#define VANITY*/

#define GAME_VERSION "1.20"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Don't typedef here, or bools can't use registers and get forced on stack. */
enum {false = 0, true};

typedef uint8_t  byte;
typedef uint16_t word;
typedef uint32_t dword;
typedef word bool;
typedef byte bbool;  /* bool in a byte */
typedef void interrupt (*InterruptFunction)(void);

#define BYTE_MAX 0xffU
#define WORD_MAX 0xffffU

#include "actor.h"
#include "def.h"
#include "graphics.h"
#include "lowlevel.h"
#include "music.h"
#include "player.h"
#include "scancode.h"
#include "sound.h"
#include "sprite.h"

#if EPISODE == 1
#   include "episode1.h"
#elif EPISODE == 2
#   include "episode2.h"
#elif EPISODE == 3
#   include "episode3.h"
#elif EPISODE == 4
#   include "episode4.h"
#else
#   error "EPISODE must be defined to 1, 2, 3, or 4"
#endif  /* EPISODE == ... */

/*****************************************************************************
 * GAME1.C                                                                   *
 *****************************************************************************/

typedef char HighScoreName[16];
typedef void (*ActorTickFunction)(word);
typedef void (*DrawFunction)(byte *, word, word);

typedef struct {
    word sprite, frame, x, y;
    bool forceactive, stayactive, acrophile, weighted;
    word westfree, eastfree, data1, data2, data3, data4, data5;
    bool dead;
    word falltime;
    byte hurtcooldown;
    ActorTickFunction tickfunc;
} Actor;

typedef struct {
    bool alive;
    word sprite, numframes, x, y, dir, numtimes;
} Decoration;

typedef struct {
    word age, x, y;
} Explosion;

typedef struct {
    word x, y, dir, stepcount, height, stepmax, delayleft;
} Fountain;

typedef struct {
    word side, x, y;
    word junk;  /* unused; required to keep struct padding correct */
} Light;

typedef struct {
    word length, datahead;
} Music;

typedef struct {
    word x, y;

    /* This must begin at word offset 2; see LoadMapData(), MovePlatforms(). */
    word mapstash[5];
} Platform;

typedef struct {
    word sprite, x, y, frame, age, xmode;
    bool bounced;
} Shard;

typedef struct {
    word actor, x, y, age;
} Spawner;

extern bbool isInGame;
extern bool winGame;
extern dword gameScore, gameStars;
extern word miscDataContents;
extern word playerHealth, playerHealthCells, playerBombs;
extern byte demoState;
extern word activePage;
extern volatile word gameTickCount;
extern bool isGodMode;
extern char *stnGroupFilename, *volGroupFilename;
extern char *musicNames[];
extern dword totalMemFreeBefore, totalMemFreeAfter;
extern HighScoreName highScoreNames[];
extern dword highScoreValues[];
extern byte *fontTileData, *maskedTileData, *miscData;
extern dword lastGroupEntryLength;
extern volatile byte lastScancode;
extern volatile bbool isKeyDown[];
extern bool isJoystickReady;
extern bbool cmdWest, cmdEast, cmdNorth, cmdSouth, cmdJump, cmdBomb;
extern bool isMusicEnabled, isSoundEnabled;
extern byte scancodeWest, scancodeEast, scancodeNorth, scancodeSouth, scancodeJump, scancodeBomb;
extern Music *activeMusic;
extern word numActors;

void DrawTextLine(word x_origin, word y_origin, char *text);
void DrawFullscreenImage(word image_num);
void StartSound(word sound_num);
void PCSpeakerService(void);
void ShowStarBonus(void);
void InnerMain(int argc, char *argv[]);

/*****************************************************************************
 * GAME2.C                                                                   *
 *****************************************************************************/

typedef char KeyName[6];

typedef struct {
    word junk;  /* in IDLIBC.C/ControlJoystick(), this is movement direction */
    bool button1, button2;
} JoystickState;

extern word yOffsetTable[];
extern bbool isAdLibPresent;

void StartAdLib(void);
void StopAdLib(void);
void WaitHard(word delay);
void WaitSoft(word delay);
void FadeWhiteCustom(word delay);
void FadeOutCustom(word delay);
void FadeIn(void);
void FadeOut(void);
byte WaitSpinner(word x, word y);
void ClearScreen(void);
JoystickState ReadJoystickState(word stick_num);
word UnfoldTextFrame(int top, int height, int width, char *top_text, char *bottom_text);
void ReadAndEchoText(word x_origin, word y_origin, char *dest, word max_length);
void DrawNumberFlushRight(word x_origin, word y_origin, dword value);
void AddScore(dword points);
void UpdateStars(void);
void UpdateBombs(void);
void UpdateHealth(void);
void ShowHighScoreTable(void);
void CheckHighScoreAndShow(void);
FILE *GroupEntryFp(char *entry_name);
void ShowOrderingInformation(void);
void ShowStory(void);
void StartGameMusic(word music_num);
void StartMenuMusic(word music_num);
void StopMusic(void);
int PlatformPlayMP3Music(char *asset_name);
void PlatformStopMP3Music(void);
int PlatformPlayMP3Sound(unsigned sound_number);
void PlatformStopMP3Sound(void);
void PlatformHapticSound(unsigned sound_number);
void PlatformHapticMenuInput(byte scancode);
void PlatformSetAudioVolumes(int sound_percent, int music_percent);
void PlatformSetCameraPosition(word scroll_x, word scroll_y, int backdrop_x, int backdrop_y, word frame_ticks);
void PlatformSetMotionInterpolationPaused(int paused);
const char *PlatformDisplayFilterName(void);
int PlatformStepDisplayFilter(int direction);
#ifdef COSMO_TOUCH_PREVIEW
int PlatformTouchControlsEnabled(void);
#define TOUCH_MENU_TEXT(keyboard, touch) (PlatformTouchControlsEnabled() ? (touch) : (keyboard))
#else
#define TOUCH_MENU_TEXT(keyboard, touch) (keyboard)
#endif
void PlatformSetMusicPaused(int paused);
int PlatformLowGravity(void);
int PlatformShieldMode(void);
int PlatformTurboMode(void);
void PlatformRecolorPlayer(byte *tiles, unsigned size);
#ifdef COSMO_WIDESCREEN
word WideTerrainColumns(void);
void BeginWideTerrain(void);
void ClearWideTerrain(void);
void DrawWideTerrainTile(word solid, const byte *masked, word x, word y);
#endif
void DrawScancodeCharacter(word x, word y, byte scancode);
void WrapBackdropVertical(byte *src, byte *dest, byte *scratch);
void WrapBackdropHorizontal(byte *src, byte *dest);
void ShowHintsAndKeys(word top);
void ShowInstructions(void);
void ShowPublisherBBS(void);
#ifdef FOREIGN_ORDERS
void ShowForeignOrders(void);
#endif  /* FOREIGN_ORDERS */
void ShowRestoreGameError(void);
void ShowCopyright(void);
void ShowAlteredFileError(void);
void ShowRescuedDNMessage(void);
void ShowE1CliffhangerMessage(word actor_type);
bbool PromptQuitConfirm(void);
void ToggleSound(void);
void ToggleMusic(void);
void ShowPauseMessage(void);
void ToggleGodMode(void);
void ShowMemoryUsage(void);
void ShowGameRedefineMenu(void);
void LoadConfigurationData(char *filename);
void SaveConfigurationData(char *filename);
void ShowEnding(void);
void ShowHintGlobeMessage(word hint_num);
void ShowCheatMessage(void);
void AddScoreForSprite(word sprite_type);
void DrawStaticGameScreen(void);
void DrawMainMenu(byte selected_option);
void MoveMainMenuCursor(byte previous_option, byte selected_option);
void ShowBombHint(void);
void ShowPounceHint(void);
void ShowLevelIntro(word level_num);
void ShowHealthHint(void);

#endif  /* GLUE_H */
