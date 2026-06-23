/* ============================================================
   PengPong — Sega Master System port
   Original MSX game by Aoineko / Pixel Phenix (2025) under CC BY-SA
   Sprites by GrafxKid (CC-BY), Graphics by Yaz, Music by Makoto
   SMS port using devkitSMS (sverx) — retains full original game logic
   ============================================================ */

#include "SMSlib.h"
#include "gfx_data.h"

#ifndef NULL
#define NULL 0
#endif

/* ---- ROM header ---- */
SMS_EMBED_SEGA_ROM_HEADER(0, 0);
SMS_EMBED_SDSC_HEADER_AUTO_DATE(1, 0,
    "Pixel Phenix", "PengPong SMS", "Volleyball penguin game. SMS port.");

/* ============================================================
   TYPE DEFINITIONS  (identical to MSXgl types)
   ============================================================ */
typedef unsigned char  u8;
typedef unsigned int   u16;
typedef signed char    i8;
typedef signed int     i16;
typedef unsigned char  bool;
#define TRUE  1
#define FALSE 0

/* ============================================================
   FIXED-POINT Q4.4 HELPERS  (identical to MSX original)
   ============================================================ */
#define Q4_4_SET(f)   ((i8)((f) * 16.0f))
#define Q4_4_GET(v)   ((i8)((i8)(v) >> 4))
#define Q4_4_FRAC(v)  ((i8)((v) & 0x0F))

/* ============================================================
   GAME-PLAY CONSTANTS  (identical to MSX original)
   ============================================================ */
#define BALL_GRAVITY    Q4_4_SET(0.15f)   /* gravity on ball               */
#define FALL_MAX_SPEED  Q4_4_SET(3.0f)    /* max fall speed                */
#define GRAVITY         Q4_4_SET(0.2f)    /* gravity on player             */
#define JUMP_FORCE      Q4_4_SET(-3.0f)   /* player jump impulse           */
#define MOVE_ACCEL      Q4_4_SET(0.33f)   /* horizontal acceleration       */
#define MOVE_MAX_SPEED  Q4_4_SET(1.5f)    /* max horizontal speed          */
#define COL_DIST        16                 /* ball–player collision radius  */
#define GROUND_Y        168                /* y at which characters rest    */
#define BALL_GROUND_Y   140                /* y threshold to count bounce   */
#define NET_X_LEFT      120                /* net left pixel edge           */
#define NET_X_RIGHT     136                /* net right pixel edge          */
#define NET_TOP_Y       128                /* net top pixel y               */
#define SCREEN_W        256
#define SCREEN_H        192
#define SCORE_MAX       11                 /* default points to win         */

/* ============================================================
   INPUT  (maps MSX INPUT_xxx → SMS joypad bits)
   ============================================================ */
#define INPUT_NONE    0x00
#define INPUT_LEFT    0x01
#define INPUT_RIGHT   0x02
#define INPUT_UP      0x04
#define INPUT_DOWN    0x08
#define INPUT_ACTION  0x10   /* jump or serve */

/* ============================================================
   PLAYER ACTIONS  (same as MSX)
   ============================================================ */
#define ACTION_IDLE   0
#define ACTION_MOVE   1
#define ACTION_JUMP   2
#define ACTION_FALL   3
#define ACTION_HIT    4
#define ACTION_WIN    5
#define ACTION_LOOSE  6

/* ============================================================
   ANIMATION FRAME TABLE
   Maps action → MSX animation-frame indices (same as original Pawn data)
   Each entry is {frame_idx, duration} — frame_idx × 4 = sprite tile offset
   ============================================================ */
typedef struct { u8 frame; u8 dur; } AnimKey;
typedef struct { const AnimKey *keys; u8 n; u8 loop; } AnimDef;

static const AnimKey g_animIdle[] = { {6,64},{7,24} };
static const AnimKey g_animMove[] = { {0,4},{1,4},{2,4},{3,4} };
static const AnimKey g_animJump[] = { {8,4},{3,4} };
static const AnimKey g_animFall[] = { {9,4} };
static const AnimKey g_animHit[]  = { {12,12} };
static const AnimKey g_animWin[]  = { {14,25},{15,25} };
static const AnimKey g_animLost[] = { {13,4} };

static const AnimDef g_anims[] = {
    { g_animIdle, 2, 1 },  /* ACTION_IDLE  */
    { g_animMove, 4, 1 },  /* ACTION_MOVE  */
    { g_animJump, 2, 1 },  /* ACTION_JUMP  */
    { g_animFall, 1, 1 },  /* ACTION_FALL  */
    { g_animHit,  1, 0 },  /* ACTION_HIT   */
    { g_animWin,  2, 1 },  /* ACTION_WIN   */
    { g_animLost, 1, 1 },  /* ACTION_LOOSE */
};

static const AnimKey g_ballAnimIdle[] = { {0,4} };
static const AnimKey g_ballAnimBump[] = { {1,1},{2,5},{1,2} };
static const AnimDef g_ballAnims[]    = {
    { g_ballAnimIdle, 1, 1 },
    { g_ballAnimBump, 3, 0 },
};

/* ============================================================
   CHARACTER STRUCTURE
   ============================================================ */
typedef struct {
    u8   id;           /* 0=left player, 1=right player           */
    u8   pos_x;        /* pixel x position                        */
    u8   pos_y;        /* pixel y position                        */
    i8   vel_x;        /* Q4.4 horizontal velocity                */
    i8   vel_y;        /* Q4.4 vertical velocity                  */
    i8   rest_x;       /* Q4.4 fractional accumulator x          */
    i8   rest_y;       /* Q4.4 fractional accumulator y          */
    u8   score;        /* points scored                           */
    u8   input;        /* current input bitfield                  */
    bool in_air;       /* TRUE if not on ground                   */
    bool moving;       /* TRUE if moving horizontally             */
    bool freeze;       /* TRUE = don't update physics (ball serve)*/
    /* Animation state */
    u8   anim_action;  /* current ACTION_xxx                      */
    u8   anim_key;     /* index into current action's key list    */
    u8   anim_timer;   /* frames remaining for current key        */
    u8   anim_frame;   /* resolved MSX frame index (0-15)         */
    bool anim_done;    /* TRUE when non-looping anim finished     */
} Character;

/* ============================================================
   GAME STATE
   ============================================================ */
typedef void (*StateFn)(void);

static u8        g_state_timer;
static StateFn   g_current_state;
static u16       g_frame_count;
static bool      g_display_dirty;   /* redraw BG next frame */

static Character g_player[2];
static Character g_ball;
static bool      g_ball_hit;        /* ball hit a player this frame  */
static u8        g_ball_ground_x;   /* AI prediction of ball landing */

/* Rules */
static u8  g_field;        /* 0=left serves, 1=right serves         */
static u8  g_bounce_num;   /* ground bounces in current point       */
static u8  g_dribble_num;  /* player-touch count in current point   */
static u8  g_change_num;   /* side-changes in current point         */
static u8  g_last_touch;   /* last player to touch ball (0 or 1)   */
static u8  g_victorious;   /* winner's ID                           */
static u8  g_game_points;  /* points to win (default 11)            */
static u8  g_max_bounce;   /* max ground bounces (default 1)        */
static u8  g_max_dribble;  /* max consecutive touches (default 3)   */
static u8  g_ai_wait;      /* frames to wait before AI serves       */

/* Game modes */
static bool g_ai_game;     /* TRUE = single-player vs CPU           */

/* Score event */
#define SCORE_OUT     0
#define SCORE_BOUNCE  1
#define SCORE_DRIBBLE 2

/* Menu state */
static u8  g_menu_sel;
static u8  g_menu_page;    /* 0=main, 1=versus, 2=solo, 3=options   */
static bool g_prev_start;

/* Joypad debounce */
static u16 g_prev_keys;
static u16 g_pressed_keys;  /* keys newly pressed this frame         */

/* ============================================================
   FORWARD DECLARATIONS
   ============================================================ */
static void state_menu_init(void);
static void state_menu(void);
static void state_game_init(void);
static void state_kickoff(void);
static void state_game(void);
static void state_point(void);
static void state_victory_init(void);
static void state_victory(void);

/* ============================================================
   PSG SOUND (direct SN76489 register writes)
   Port 0x7F: latch/data byte
     Latch: 1RRccCCC — RR=register(tone/noise/vol), cc=channel, CCC=data bits
     Data:  0xDDDDDD — extended data
   ============================================================ */
static void psg_out(u8 val) {
    __asm
        ld a, 4 (ix)
        out (0x7F), a
    __endasm;
    val;
}
/* Silence all channels */
static void psg_silence(void) {
    psg_out(0x9F);   /* ch0 vol=0 */
    psg_out(0xBF);   /* ch1 vol=0 */
    psg_out(0xDF);   /* ch2 vol=0 */
    psg_out(0xFF);   /* noise vol=0 */
}
/* Play a short tone burst: channel 0-2, period (0-1023), vol 0-15, frames */
static u8  g_sfx_ch;
static u16 g_sfx_period;
static u8  g_sfx_vol;
static u8  g_sfx_frames;

static void psg_play_sfx(u8 period_lo, u8 period_hi, u8 vol, u8 frames) {
    /* channel 0 for SFX */
    psg_out((u8)(0x80 | (period_lo & 0x0F)));          /* tone ch0 low nibble */
    psg_out((u8)(period_hi & 0x3F));                    /* tone ch0 high 6 bits */
    psg_out((u8)(0x90 | (15 - (vol & 0x0F))));         /* ch0 volume */
    g_sfx_frames = frames;
}

static void psg_update(void) {
    if (g_sfx_frames > 0) {
        g_sfx_frames--;
        if (g_sfx_frames == 0) psg_out(0x9F);  /* silence ch0 */
    }
}

/* SFX helpers */
#define SFX_JUMP()   psg_play_sfx(0x1E, 0x00, 12, 6)   /* high short tone  */
#define SFX_LAND()   psg_play_sfx(0x3C, 0x00, 10, 4)   /* mid short tone   */
#define SFX_BUMP()   psg_play_sfx(0x0F, 0x00, 14, 5)   /* high click       */
#define SFX_SCORE()  psg_play_sfx(0x06, 0x00, 14, 12)  /* rising tone      */
#define SFX_CLICK()  psg_play_sfx(0x3C, 0x00,  8, 3)   /* quiet click      */

/* ============================================================
   TEXT / BG RENDERING HELPERS
   ============================================================ */
static void bg_put_tile(u8 x, u8 y, u16 tile) {
    SMS_setNextTileatXY(x, y);
    SMS_setTile(tile);
}

static void bg_print(u8 x, u8 y, const char *str) {
    u8 cx = x;
    while (*str) {
        u8 ch = (u8)*str;
        u16 tile = (u16)(BG_TILE_FONT_BASE + (ch - 32));
        bg_put_tile(cx, y, tile | TILE_USE_SPRITE_PALETTE | TILE_PRIORITY);
        cx++;
        str++;
    }
}

static void bg_print_hi(u8 x, u8 y, const char *str) {
    /* Print with different color (inverse — uses BG palette's color 12=red) */
    u8 cx = x;
    while (*str) {
        u8 ch = (u8)*str;
        u16 tile = (u16)(BG_TILE_FONT_BASE + (ch - 32));
        bg_put_tile(cx, y, tile | TILE_PRIORITY);
        cx++;
        str++;
    }
}

/* Print a decimal number (0-99) at position */
static void bg_print_num2(u8 x, u8 y, u8 n) {
    char buf[3];
    buf[0] = '0' + (n / 10);
    buf[1] = '0' + (n % 10);
    buf[2] = '\0';
    bg_print(x, y, buf);
}

/* Fill a rectangle with a tile */
static void bg_fill_rect(u8 x, u8 y, u8 w, u8 h, u16 tile) {
    u8 r, c;
    for (r = 0; r < h; r++) {
        for (c = 0; c < w; c++) {
            bg_put_tile(x + c, y + r, tile);
        }
    }
}

/* ============================================================
   COURT / BACKGROUND LAYOUT
   Tile rows:
     0       = score bar (10 px = tile row 0)
     1-2     = sky dark  (y 8-23)
     3-5     = sky mid   (y 24-47)
     6-10    = sky light (y 48-87)
     11      = horizon   (y 88-95)
     12-21   = court     (y 96-175)
     22      = court line (y 176)
     23      = dirt       (y 184)
   ============================================================ */
static void draw_court(void) {
    u8 y;

    /* Row 0: score bar */
    bg_fill_rect(0, 0, 32, 1, BG_TILE_SCORE_BG);

    /* Rows 1-2: dark sky */
    bg_fill_rect(0, 1, 32, 2, BG_TILE_SKY_DARK);

    /* Rows 3-5: mid sky */
    bg_fill_rect(0, 3, 32, 3, BG_TILE_SKY_MID);

    /* Rows 6-10: light sky */
    bg_fill_rect(0, 6, 32, 5, BG_TILE_SKY_LIGHT);

    /* Row 11: horizon */
    bg_fill_rect(0, 11, 32, 1, BG_TILE_HORIZON);

    /* Rows 12-21: court body */
    bg_fill_rect(0, 12, 32, 10, BG_TILE_COURT_BODY);

    /* Row 22: court top line */
    bg_fill_rect(0, 22, 32, 1, BG_TILE_COURT_LINE);

    /* Row 23: dirt */
    bg_fill_rect(0, 23, 32, 1, BG_TILE_DIRT);

    /* Net (columns 15-16, rows 12-22) */
    for (y = 12; y <= 16; y++) {
        bg_put_tile(15, y, BG_TILE_NET_TOP);
        bg_put_tile(16, y, BG_TILE_NET_TOP);
    }
    for (y = 17; y <= 22; y++) {
        bg_put_tile(15, y, BG_TILE_NET_MID);
        bg_put_tile(16, y, BG_TILE_NET_MID);
    }
}

static void draw_score(void) {
    /* Left score at column 12, right score at column 17 */
    bg_print_num2(12, 0, g_player[0].score);
    bg_print(14, 0, "-");
    bg_print_num2(16, 0, g_player[1].score);
}

static void clear_info(void) {
    bg_fill_rect(8, 15, 16, 1, BG_TILE_COURT_BODY);
}

static void draw_info(u8 event) {
    const char *msg = "";
    if (event == SCORE_OUT) {
        if (g_dribble_num == 0 && g_bounce_num == 0)      msg = "   OUT!   ";
        else if (g_dribble_num == 0 && g_change_num == 1) msg = "   ACE!   ";
        else                                                msg = " PASSING! ";
    } else {
        msg = "  FAULT!  ";
    }
    bg_print(8, 15, msg);
}

/* ============================================================
   ANIMATION SYSTEM
   ============================================================ */
static void anim_set_action(Character *c, u8 action, bool force) {
    if (!force && c->anim_action == action) return;
    if (!force && c->anim_done) return;   /* non-looping anims play to end */
    c->anim_action = action;
    c->anim_key    = 0;
    c->anim_done   = FALSE;
    c->anim_timer  = g_anims[action].keys[0].dur;
    c->anim_frame  = g_anims[action].keys[0].frame;
}

static void anim_update_player(Character *c) {
    const AnimDef *anim = &g_anims[c->anim_action];
    if (c->anim_done) return;
    if (c->anim_timer > 0) {
        c->anim_timer--;
        return;
    }
    /* Advance to next key */
    c->anim_key++;
    if (c->anim_key >= anim->n) {
        if (anim->loop) {
            c->anim_key = 0;
        } else {
            c->anim_key = anim->n - 1;
            c->anim_done = TRUE;
        }
    }
    c->anim_frame = anim->keys[c->anim_key].frame;
    c->anim_timer = anim->keys[c->anim_key].dur;
}

static void anim_update_ball(Character *b) {
    const AnimDef *anim = &g_ballAnims[b->anim_action];
    if (b->anim_done) return;
    if (b->anim_timer > 0) {
        b->anim_timer--;
        return;
    }
    b->anim_key++;
    if (b->anim_key >= anim->n) {
        if (anim->loop) {
            b->anim_key = 0;
        } else {
            b->anim_key = anim->n - 1;
            b->anim_done = TRUE;
            /* After bump, return to idle */
            b->anim_action = 0;
            b->anim_key    = 0;
            b->anim_done   = FALSE;
            b->anim_frame  = g_ballAnims[0].keys[0].frame;
            b->anim_timer  = g_ballAnims[0].keys[0].dur;
            return;
        }
    }
    b->anim_frame = anim->keys[b->anim_key].frame;
    b->anim_timer = anim->keys[b->anim_key].dur;
}

static void ball_set_bump(Character *b) {
    if (b->anim_action != 1) {
        b->anim_action = 1;
        b->anim_key    = 0;
        b->anim_done   = FALSE;
        b->anim_frame  = g_ballAnims[1].keys[0].frame;
        b->anim_timer  = g_ballAnims[1].keys[0].dur;
    }
}

/* ============================================================
   SPRITE RENDERING
   Uses 8×16 sprite mode. Each 16×16 character = 2 sprite entries.
   ============================================================ */
static void draw_player_sprite(u8 x, u8 y, u8 frame, u8 p2) {
    /* In 8×16 mode, SMS_addSprite uses pairs: tile_n (top) + tile_n+1 (bottom)
       For a 16×16 char:
         Left  sprite: tiles [frame*4 + 0] and [frame*4 + 1]
         Right sprite: tiles [frame*4 + 2] and [frame*4 + 3]
       We call SMS_addSprite with even tile index (top of pair). */
    u8 base = (u8)(p2 ? SPR_PLY2_BASE : SPR_PLY1_BASE);
    u8 tl = (u8)(base + frame * 4);       /* top-left tile (even, top of left pair)  */
    u8 tr = (u8)(base + frame * 4 + 2);   /* top-right tile (even, top of right pair) */
    if (y < 192) {
        SMS_addSprite(x,     (u8)(y - 1), tl);
        SMS_addSprite(x + 8, (u8)(y - 1), tr);
    }
}

static void draw_ball_sprite(u8 x, u8 y, u8 frame) {
    u8 tl = (u8)(SPR_BALL_BASE + frame * 4);
    u8 tr = (u8)(SPR_BALL_BASE + frame * 4 + 2);
    if (y < 192) {
        SMS_addSprite(x,     (u8)(y - 1), tl);
        SMS_addSprite(x + 8, (u8)(y - 1), tr);
    }
}

/* Shadow size based on character height (how far from ground) */
static u8 shadow_tile_for_y(u8 char_y) {
    i8 dist = (i8)(GROUND_Y - (i8)char_y);
    if (dist <= 8)  return (u8)(SPR_SHADOW_BASE + 0);
    if (dist <= 20) return (u8)(SPR_SHADOW_BASE + 1);
    if (dist <= 40) return (u8)(SPR_SHADOW_BASE + 2);
    return (u8)(SPR_SHADOW_BASE + 3);
}

static void draw_shadow(u8 x, u8 char_y) {
    u8 tile = shadow_tile_for_y(char_y);
    /* Shadow always at ground level, 8×8 (8×16 mode but tile is 1 tile of shadow) */
    SMS_addSprite(x, 182, tile);
}

/* ============================================================
   PHYSICS / MOVEMENT
   Replicates the MSX Pawn physics system behaviour.
   ============================================================ */
static void move_character(Character *c, i8 dx, i8 dy) {
    i16 nx = (i16)c->pos_x + dx;
    i16 ny = (i16)c->pos_y + dy;
    bool collided_y = FALSE;

    /* --- Vertical (dy) --- */
    if (dy > 0) {  /* Moving down */
        if ((i16)(ny + 16) >= SCREEN_H) {
            /* Bottom border */
            ny = (i16)(SCREEN_H - 16);
            c->vel_y = 0;
            collided_y = TRUE;
        } else {
            /* Ground */
            if ((i16)(ny + 16) >= (GROUND_Y + 8)) {
                ny = (i16)(GROUND_Y);
                c->vel_y = 0;
                collided_y = TRUE;
            } else {
                /* Net collision (vertical) */
                /* Ball check: if at net X range */
                if (c->id == 0xFF) {   /* ball only: no vertical net block */
                }
            }
        }
        if (collided_y && c->in_air) {
            c->in_air = FALSE;
            if (c->id != 0xFF) SFX_LAND();
        }
    } else if (dy < 0) {
        if (ny < 0) { ny = 0; c->vel_y = 0; }
    }

    /* --- Check falling (no ground below) --- */
    if (!collided_y && !c->in_air && dy >= 0) {
        if ((i16)(ny + 16) < GROUND_Y + 4) {
            /* No ground detected: begin fall */
            c->in_air = TRUE;
            if (c->vel_y == 0) c->vel_y = 0;
        }
    }

    /* --- Horizontal (dx) --- */
    if (dx != 0) {
        /* Left/right borders */
        if (nx < 0) {
            nx = 0;
            if (c->id == 0xFF) { c->vel_x = (i8)(-c->vel_x); nx = 0; }
        } else if ((i16)(nx + 16) > SCREEN_W) {
            nx = (i16)(SCREEN_W - 16);
            if (c->id == 0xFF) { c->vel_x = (i8)(-c->vel_x); nx = (i16)(SCREEN_W - 16); }
        }

        /* Net collision (horizontal) — only for ball */
        if (c->id == 0xFF) {
            i16 bx_new_right = nx + 15;
            i16 bx_new_left  = nx;
            i16 bx_old_right = (i16)c->pos_x + 15;
            i16 bx_old_left  = (i16)c->pos_x;
            i16 by_bot = ny + 15;

            if (by_bot >= NET_TOP_Y) {
                /* Moving right: hit left wall of net */
                if (dx > 0 && bx_old_right < NET_X_LEFT && bx_new_right >= NET_X_LEFT) {
                    nx = (i16)(NET_X_LEFT - 16);
                    c->vel_x = (i8)(-c->vel_x);
                }
                /* Moving left: hit right wall of net */
                if (dx < 0 && bx_old_left >= NET_X_RIGHT && bx_new_left < NET_X_RIGHT) {
                    nx = (i16)NET_X_RIGHT;
                    c->vel_x = (i8)(-c->vel_x);
                }
            }
        } else {
            /* Player net wall: stop player at net */
            i16 px_right = nx + 15;
            i16 px_left  = nx;
            i16 old_right = (i16)c->pos_x + 15;
            i16 old_left  = (i16)c->pos_x;
            if (dx > 0 && old_right < NET_X_LEFT && px_right >= NET_X_LEFT)
                nx = (i16)(NET_X_LEFT - 16);
            if (dx < 0 && old_left >= NET_X_RIGHT && px_left < NET_X_RIGHT)
                nx = (i16)NET_X_RIGHT;
        }
    }

    c->pos_x = (u8)nx;
    c->pos_y = (u8)ny;
}

/* Apply sub-pixel velocity to character (same accumulation as MSX original) */
static void apply_velocity(Character *c) {
    i8 dx, dy;

    dx = Q4_4_GET(c->vel_x);
    c->rest_x += (i8)(c->vel_x - dx);
    dx += Q4_4_GET(c->rest_x);
    c->rest_x = Q4_4_FRAC(c->rest_x);

    dy = Q4_4_GET(c->vel_y);
    c->rest_y += (i8)(c->vel_y - dy);
    dy += Q4_4_GET(c->rest_y);
    c->rest_y = Q4_4_FRAC(c->rest_y);

    move_character(c, dx, dy);
}

/* ============================================================
   RULES ENGINE  (identical logic to MSX original)
   ============================================================ */
static void rules_change_field(u8 field) {
    g_field       = field;
    g_bounce_num  = 0;
    g_dribble_num = 0;
}

static void rules_init(void) {
    g_player[0].score = 0;
    g_player[1].score = 0;
    draw_score();
    rules_change_field(g_ai_game ? 1 : 0);
}

static void rules_score(u8 ply, u8 event) {
    g_victorious = ply;
    g_player[ply].score++;
    draw_score();
    draw_info(event);
    SFX_SCORE();
    if (g_player[ply].score >= g_game_points &&
        g_player[ply].score > g_player[1 - ply].score + 1) {
        g_current_state = state_victory_init;
    } else {
        g_state_timer   = 60;
        g_current_state = state_point;
    }
}

static void rules_bounce(void) {
    g_bounce_num++;
    if (g_max_bounce != 0 && g_bounce_num > g_max_bounce)
        rules_score((u8)(1 - g_field), SCORE_BOUNCE);
    g_last_touch = g_field;
}

static void rules_dribble(void) {
    g_dribble_num++;
    if (g_max_dribble != 0 && g_dribble_num > g_max_dribble)
        rules_score((u8)(1 - g_field), SCORE_DRIBBLE);
    g_last_touch = g_field;
}

static void rules_out(void) {
    rules_score((u8)(1 - g_last_touch), SCORE_OUT);
}

/* ============================================================
   AI INPUT  (identical to MSX)
   ============================================================ */
static u8 ai_predict_medium(void) {
    i16 hit_x = (i16)g_ball.pos_x +
                Q4_4_GET((i8)(g_ball.vel_x * (i8)((168 - g_ball.pos_y) / 3)));
    if (hit_x < 0)   hit_x = 0;
    if (hit_x > 255) hit_x = 255;
    return (u8)hit_x;
}

static u8 check_ai_input(void) {
    u8 ball_x  = g_ball.pos_x;
    u8 ball_y  = g_ball.pos_y;
    u8 ply_x   = g_player[0].pos_x;

    /* Serve */
    if (ball_x < 128 && g_ball.freeze) {
        if (g_ai_wait > 0) { g_ai_wait--; return INPUT_NONE; }
        if (ply_x < 40) return (u8)(INPUT_ACTION | INPUT_RIGHT);
        return INPUT_LEFT;
    }

    g_ball_ground_x = ai_predict_medium();

    /* Reposition when ball is on far side */
    if (ball_x > 128 && g_ball_ground_x > 128) {
        if (ply_x < 32) return INPUT_RIGHT;
        if (ply_x > 64) return INPUT_LEFT;
        return INPUT_NONE;
    }

    /* Shoot */
    {
        i8 dx = (i8)(g_ball_ground_x - ply_x);
        i8 sx = (i8)(ball_x - ply_x);
        if (dx > 0 && sx < 24 && ball_x < 128 && ball_y > 128) {
            if (sx > 12) return (u8)(INPUT_ACTION | INPUT_RIGHT);
            return INPUT_ACTION;
        }
    }

    /* Reception */
    if (ply_x < g_ball_ground_x) return INPUT_RIGHT;
    return INPUT_LEFT;
}

/* ============================================================
   JOYPAD INPUT
   ============================================================ */
static u8 read_player_input(u8 id) {
    u16 keys;
    u8 ret = INPUT_NONE;

    if (id == 0) {
        keys = SMS_getKeysStatus();
        if (keys & PORT_A_KEY_LEFT)  ret |= INPUT_LEFT;
        if (keys & PORT_A_KEY_RIGHT) ret |= INPUT_RIGHT;
        if (keys & PORT_A_KEY_1)     ret |= INPUT_ACTION;
        if (keys & PORT_A_KEY_2)     ret |= INPUT_ACTION;
        if (keys & PORT_A_KEY_UP)    ret |= INPUT_ACTION;
    } else {
        keys = SMS_getKeysStatus();
        if (keys & PORT_B_KEY_LEFT)  ret |= INPUT_LEFT;
        if (keys & PORT_B_KEY_RIGHT) ret |= INPUT_RIGHT;
        if (keys & PORT_B_KEY_1)     ret |= INPUT_ACTION;
        if (keys & PORT_B_KEY_2)     ret |= INPUT_ACTION;
        if (keys & PORT_B_KEY_UP)    ret |= INPUT_ACTION;
    }
    return ret;
}

/* ============================================================
   PLAYER UPDATE  (identical logic to MSX original)
   ============================================================ */
static void update_player(Character *ply) {
    u8 act;

    /* Horizontal movement */
    if (ply->input & INPUT_RIGHT) {
        ply->vel_x += MOVE_ACCEL;
        ply->moving = TRUE;
    } else if (ply->input & INPUT_LEFT) {
        ply->vel_x -= MOVE_ACCEL;
        ply->moving = TRUE;
    } else {
        ply->vel_x = 0;
        ply->moving = FALSE;
    }
    /* Clamp speed */
    if (ply->vel_x >  MOVE_MAX_SPEED) ply->vel_x = MOVE_MAX_SPEED;
    if (ply->vel_x < -MOVE_MAX_SPEED) ply->vel_x = (i8)(-MOVE_MAX_SPEED);

    /* Vertical */
    if (ply->in_air) {
        ply->vel_y += GRAVITY;
        if (ply->vel_y > FALL_MAX_SPEED) ply->vel_y = FALL_MAX_SPEED;
    } else if (ply->input & INPUT_ACTION) {
        SFX_JUMP();
        ply->in_air = TRUE;
        ply->vel_y  = JUMP_FORCE;
    } else {
        ply->vel_y = 0;
    }

    /* Animation action */
    if (ply->in_air && ply->vel_y < 0) act = ACTION_JUMP;
    else if (ply->in_air)              act = ACTION_FALL;
    else if (ply->moving)              act = ACTION_MOVE;
    else                               act = ACTION_IDLE;

    anim_set_action(ply, act, FALSE);
    anim_update_player(ply);

    /* Physics */
    apply_velocity(ply);
}

/* ============================================================
   BALL UPDATE  (identical logic to MSX original)
   ============================================================ */
static void update_ball(void) {
    u8 old_x;
    Character *ply;
    i8 dx, dy;
    u16 sq_dist;

    /* Gravity */
    g_ball.vel_y += BALL_GRAVITY;
    if (g_ball.vel_y > FALL_MAX_SPEED) g_ball.vel_y = FALL_MAX_SPEED;

    /* Select nearest player */
    ply = (g_ball.pos_x < 128 - 8) ? &g_player[0] : &g_player[1];

    /* Ball–player collision */
    dx = (i8)(g_ball.pos_x - ply->pos_x);
    dy = (i8)(g_ball.pos_y - ply->pos_y);
    sq_dist = (u16)((i16)dx * dx + (i16)dy * dy);

    if (sq_dist < (u16)(COL_DIST * COL_DIST)) {
        if (!g_ball_hit) {
            g_ball_hit = TRUE;
            SFX_BUMP();
            rules_dribble();
        }
        g_ball.vel_x = (i8)(dx * 2);
        g_ball.vel_x += (i8)(ply->vel_x / 2);
        g_ball.vel_y  = Q4_4_SET(-1.5f);
        if (ply->vel_y < 0) g_ball.vel_y += (i8)(ply->vel_y / 2);
        g_ball.freeze = FALSE;
        ball_set_bump(&g_ball);
        anim_set_action(ply, ACTION_HIT, TRUE);
    } else {
        g_ball_hit = FALSE;
    }

    /* Ground bounce detection — before moving */
    old_x = g_ball.pos_x;

    /* Physics move (also handles ground/net/border events) */
    if (!g_ball.freeze) {
        i8 dx2, dy2;
        dx2 = Q4_4_GET(g_ball.vel_x);
        g_ball.rest_x += (i8)(g_ball.vel_x - dx2);
        dx2 += Q4_4_GET(g_ball.rest_x);
        g_ball.rest_x = Q4_4_FRAC(g_ball.rest_x);

        dy2 = Q4_4_GET(g_ball.vel_y);
        g_ball.rest_y += (i8)(g_ball.vel_y - dy2);
        dy2 += Q4_4_GET(g_ball.rest_y);
        g_ball.rest_y = Q4_4_FRAC(g_ball.rest_y);

        /* Check for border out before moving */
        {
            i16 nx = (i16)g_ball.pos_x + dx2;
            if (nx < 0 || (i16)(nx + 16) > SCREEN_W) {
                rules_out();
                return;
            }
        }

        move_character(&g_ball, dx2, dy2);

        /* Ground bounce */
        if (!g_ball.in_air && g_ball.pos_y >= GROUND_Y - 2) {
            g_ball.vel_y = (i8)(-g_ball.vel_y);
            g_ball.pos_y  = (u8)(GROUND_Y - 2);
            g_ball.in_air = TRUE;
            ball_set_bump(&g_ball);
            SFX_BUMP();
            if (g_ball.pos_y > BALL_GROUND_Y)
                rules_bounce();
        }
    }

    anim_update_ball(&g_ball);

    /* Field change detection */
    if ((old_x <= 120) && (g_ball.pos_x > 120)) {
        rules_change_field(1);
        g_change_num++;
    } else if ((old_x > 120) && (g_ball.pos_x <= 120)) {
        rules_change_field(0);
        g_change_num++;
    }
}

/* ============================================================
   CHARACTER INIT
   ============================================================ */
static void init_player(u8 id) {
    Character *p = &g_player[id];
    u8 i;
    /* Zero the structure */
    for (i = 0; i < sizeof(Character); i++) ((u8*)p)[i] = 0;
    p->id         = id;
    p->pos_x      = (id == 0) ? 32 : (u8)(255u - 32u - 16u);
    p->pos_y      = GROUND_Y;
    p->in_air     = FALSE;
    p->anim_frame = g_anims[ACTION_IDLE].keys[0].frame;
    p->anim_timer = g_anims[ACTION_IDLE].keys[0].dur;
}

static void init_ball(void) {
    u8 i;
    for (i = 0; i < sizeof(Character); i++) ((u8*)&g_ball)[i] = 0;
    g_ball.id     = 0xFF;   /* sentinel: not a player */
    g_ball.pos_x  = (g_field == 0) ? 56 : 184;
    g_ball.pos_y  = 128;
    g_ball.freeze = TRUE;
    g_ball.in_air = TRUE;
    g_ball.anim_frame = g_ballAnims[0].keys[0].frame;
    g_ball.anim_timer = g_ballAnims[0].keys[0].dur;
    g_ball_hit    = FALSE;
}

/* ============================================================
   MENU SYSTEM
   ============================================================ */
/* Pages */
#define PAGE_MAIN    0
#define PAGE_VERSUS  1
#define PAGE_SOLO    2
#define PAGE_OPTIONS 3

/* Menu item strings per page */
static const char *g_main_items[]   = { "PLY VS PLY", "PLY VS CPU", "OPTIONS   " };
static const char *g_versus_items[] = { "START >   ", "P1: PORT A", "P2: PORT B", "BACK      " };
static const char *g_solo_items[]   = { "START >   ", "CTRL:PTR A", "BACK      " };
static const char *g_opt_items[]    = { "POINTS:   ", "BOUNCES:  ", "DRIBBLES: ", "BACK      " };

static u8 g_menu_num_items[] = { 3, 4, 3, 4 };

static void draw_menu_page(void) {
    /* Select items array for the current page (avoid non-const pointer array) */
    const char **items;
    u8 i, num = g_menu_num_items[g_menu_page];
    u8 start_row = 6;
    switch (g_menu_page) {
        case PAGE_VERSUS:  items = g_versus_items; break;
        case PAGE_SOLO:    items = g_solo_items;   break;
        case PAGE_OPTIONS: items = g_opt_items;    break;
        default:           items = g_main_items;   break;
    }

    /* Clear menu area */
    bg_fill_rect(7, start_row, 18, 14, BG_TILE_SKY_DARK);

    /* Title */
    bg_print(10, 4, "PENGPONG");

    for (i = 0; i < num; i++) {
        if (i == g_menu_sel)
            bg_print_hi(8, (u8)(start_row + i * 2), "> ");
        else
            bg_print   (8, (u8)(start_row + i * 2), "  ");
        bg_print(10, (u8)(start_row + i * 2), items[i]);
    }

    /* Show values for options page */
    if (g_menu_page == PAGE_OPTIONS) {
        char buf[4];
        buf[0] = '0' + g_game_points / 10;
        buf[1] = '0' + g_game_points % 10;
        buf[2] = '\0';
        bg_print(21, start_row + 0, buf);
        buf[0] = '0' + g_max_bounce;
        buf[1] = '\0';
        bg_print(21, start_row + 2, buf);
        buf[0] = '0' + g_max_dribble;
        buf[1] = '\0';
        bg_print(21, start_row + 4, buf);
    }
}

/* ============================================================
   GAME STATES
   ============================================================ */

/* ---- MENU INIT ---- */
static void state_menu_init(void) {
    SMS_displayOff();
    SMS_VDPturnOffFeature(VDPFEATURE_SHOWDISPLAY);

    /* Reload BG and sprite tiles */
    SMS_loadTiles(g_bg_tiles, 0, G_BG_TILES_SIZE);
    SMS_loadTiles(g_spr_p1_tiles,     256, G_SPR_P1_TILES_SIZE);
    SMS_loadTiles(g_spr_p2_tiles,     320, G_SPR_P2_TILES_SIZE);
    SMS_loadTiles(g_spr_ball_tiles,   384, G_SPR_BALL_TILES_SIZE);
    SMS_loadTiles(g_spr_shadow_tiles, 396, G_SPR_SHADOW_TILES_SIZE);

    SMS_loadBGPalette(g_bg_palette);
    SMS_loadSpritePalette(g_spr_palette);
    SMS_setBackdropColor(0);

    draw_court();

    /* Title screen logo (simple text) */
    bg_fill_rect(0, 0, 32, 24, BG_TILE_SKY_DARK);
    bg_print(10, 4, "PENGPONG");
    bg_print(9, 5, "=========");
    bg_print(6, 20, "PRESS A TO START");

    g_menu_page = PAGE_MAIN;
    g_menu_sel  = 0;
    draw_menu_page();

    SMS_displayOn();
    g_current_state = state_menu;
}

/* ---- MENU UPDATE ---- */
static void state_menu(void) {
    u16 keys   = SMS_getKeysStatus();
    u16 pressed = keys & ~g_prev_keys;
    g_prev_keys = keys;

    u8 num = g_menu_num_items[g_menu_page];

    if (pressed & PORT_A_KEY_DOWN) {
        g_menu_sel = (u8)((g_menu_sel + 1) % num);
        SFX_CLICK();
        draw_menu_page();
    }
    if (pressed & PORT_A_KEY_UP) {
        g_menu_sel = (u8)((g_menu_sel + num - 1) % num);
        SFX_CLICK();
        draw_menu_page();
    }

    if ((pressed & PORT_A_KEY_1) || (pressed & PORT_A_KEY_2)) {
        SFX_CLICK();
        /* Handle selection */
        if (g_menu_page == PAGE_MAIN) {
            if (g_menu_sel == 0) { g_menu_page = PAGE_VERSUS;  g_menu_sel = 0; draw_menu_page(); }
            if (g_menu_sel == 1) { g_menu_page = PAGE_SOLO;    g_menu_sel = 0; draw_menu_page(); }
            if (g_menu_sel == 2) { g_menu_page = PAGE_OPTIONS; g_menu_sel = 0; draw_menu_page(); }
        } else if (g_menu_page == PAGE_VERSUS) {
            if (g_menu_sel == 0) {  /* Start versus */
                g_ai_game = FALSE;
                g_current_state = state_game_init;
            }
            if (g_menu_sel == 3) { g_menu_page = PAGE_MAIN; g_menu_sel = 0; draw_menu_page(); }
        } else if (g_menu_page == PAGE_SOLO) {
            if (g_menu_sel == 0) {  /* Start solo */
                g_ai_game = TRUE;
                g_current_state = state_game_init;
            }
            if (g_menu_sel == 2) { g_menu_page = PAGE_MAIN; g_menu_sel = 0; draw_menu_page(); }
        } else if (g_menu_page == PAGE_OPTIONS) {
            if (g_menu_sel == 0) {
                g_game_points = (u8)(g_game_points < 60 ? g_game_points + 1 : 1);
                draw_menu_page();
            }
            if (g_menu_sel == 1) {
                g_max_bounce = (u8)(g_max_bounce < 5 ? g_max_bounce + 1 : 0);
                draw_menu_page();
            }
            if (g_menu_sel == 2) {
                g_max_dribble = (u8)(g_max_dribble < 5 ? g_max_dribble + 1 : 0);
                draw_menu_page();
            }
            if (g_menu_sel == 3) { g_menu_page = PAGE_MAIN; g_menu_sel = 0; draw_menu_page(); }
        }
    }
    /* Also accept Start (key 1) from port B for second player to navigate */
    if (pressed & PORT_B_KEY_1) {
        if (g_menu_page == PAGE_VERSUS && g_menu_sel == 0) {
            g_ai_game = FALSE;
            g_current_state = state_game_init;
        }
    }
}

/* ---- GAME INIT ---- */
static void state_game_init(void) {
    SMS_displayOff();
    SMS_VDPturnOffFeature(VDPFEATURE_SHOWDISPLAY);

    draw_court();
    draw_score();

    init_player(0);
    init_player(1);
    init_ball();

    g_change_num = 0;
    rules_init();

    SMS_displayOn();
    g_current_state = state_kickoff;
}

/* ---- KICK-OFF ---- */
static void state_kickoff(void) {
    g_player[0].pos_x  = 32;
    g_player[0].pos_y  = GROUND_Y;
    g_player[0].vel_x  = 0;
    g_player[0].vel_y  = 0;
    g_player[0].in_air = FALSE;
    g_player[1].pos_x  = (u8)(255u - 32u - 16u);
    g_player[1].pos_y  = GROUND_Y;
    g_player[1].vel_x  = 0;
    g_player[1].vel_y  = 0;
    g_player[1].in_air = FALSE;

    g_ai_wait    = 30;
    g_change_num = 0;

    init_ball();

    g_current_state = state_game;
}

/* ---- GAME UPDATE ---- */
static void state_game(void) {
    u16 keys   = SMS_getKeysStatus();
    u16 pressed = keys & ~g_prev_keys;
    g_prev_keys = keys;

    /* Pause / back to menu */
    if (pressed & PORT_A_KEY_2) {
        psg_silence();
        g_current_state = state_menu_init;
        return;
    }

    /* Read player inputs */
    g_player[0].input = g_ai_game ? check_ai_input() : read_player_input(0);
    g_player[1].input = read_player_input(1);

    /* Ball serve on action press */
    if (g_ball.freeze) {
        u8 server = g_field;
        if (!g_ai_game || server == 1) {
            if (g_player[server].input & INPUT_ACTION) g_ball.freeze = FALSE;
        }
    }

    update_player(&g_player[0]);
    update_player(&g_player[1]);
    update_ball();
}

/* ---- POINT WON ---- */
static void state_point(void) {
    u16 keys   = SMS_getKeysStatus();
    u16 pressed = keys & ~g_prev_keys;
    g_prev_keys = keys;

    g_state_timer--;
    if (g_state_timer == 0 || (pressed & PORT_A_KEY_1) || (pressed & PORT_B_KEY_1)) {
        clear_info();
        rules_change_field((u8)(1 - g_victorious));
        g_current_state = state_kickoff;
    }
}

/* ---- VICTORY INIT ---- */
static void state_victory_init(void) {
    g_state_timer = 240;
    g_player[0].pos_y = GROUND_Y;
    g_player[1].pos_y = GROUND_Y;
    g_ball.freeze     = TRUE;

    anim_set_action(&g_player[g_victorious],     ACTION_WIN,   TRUE);
    anim_set_action(&g_player[1-g_victorious],   ACTION_LOOSE, TRUE);

    /* Display winner */
    clear_info();
    if (g_victorious == 0)
        bg_print(8, 15, "  P1 WINS!");
    else
        bg_print(8, 15, "  P2 WINS!");

    SFX_SCORE();
    g_current_state = state_victory;
}

/* ---- VICTORY UPDATE ---- */
static void state_victory(void) {
    u16 keys   = SMS_getKeysStatus();
    u16 pressed = keys & ~g_prev_keys;
    g_prev_keys = keys;

    anim_update_player(&g_player[0]);
    anim_update_player(&g_player[1]);

    g_state_timer--;
    if (g_state_timer == 0 || (pressed & PORT_A_KEY_1) || (pressed & PORT_B_KEY_1)) {
        clear_info();
        psg_silence();
        g_current_state = state_menu_init;
    }
}

/* ============================================================
   SPRITE DRAWING FOR CURRENT FRAME
   ============================================================ */
static void draw_all_sprites(void) {
    SMS_initSprites();

    /* Ball */
    if (!g_ball.freeze || (g_frame_count & 4))  /* blink while frozen */
        draw_ball_sprite(g_ball.pos_x, g_ball.pos_y, g_ball.anim_frame);

    /* Player 1 (faces right, so no flip = p2=FALSE) */
    draw_player_sprite(g_player[0].pos_x, g_player[0].pos_y,
                       g_player[0].anim_frame, FALSE);

    /* Player 2 (faces left = mirrored = p2=TRUE) */
    draw_player_sprite(g_player[1].pos_x, g_player[1].pos_y,
                       g_player[1].anim_frame, TRUE);

    /* Shadows */
    draw_shadow(g_ball.pos_x + 4,     g_ball.pos_y);
    draw_shadow(g_player[0].pos_x + 4, g_player[0].pos_y);
    draw_shadow(g_player[1].pos_x + 4, g_player[1].pos_y);

    SMS_copySpritestoSAT();
}

/* ============================================================
   MAIN  (entry point; crt0_sms.rel calls main())
   ============================================================ */
void main(void) {
    /* VDP configuration */
    SMS_VDPturnOnFeature(VDPFEATURE_FRAMEIRQ);
    SMS_setSpriteMode(SPRITEMODE_TALL);          /* 8×16 sprite mode          */
    SMS_useFirstHalfTilesforSprites(0);          /* sprites use tiles 256-511 */
    SMS_setBackdropColor(0);

    /* Palettes */
    SMS_loadBGPalette(g_bg_palette);
    SMS_loadSpritePalette(g_spr_palette);

    /* Load tiles into VRAM */
    SMS_loadTiles(g_bg_tiles,         0,   G_BG_TILES_SIZE);
    SMS_loadTiles(g_spr_p1_tiles,     256, G_SPR_P1_TILES_SIZE);
    SMS_loadTiles(g_spr_p2_tiles,     320, G_SPR_P2_TILES_SIZE);
    SMS_loadTiles(g_spr_ball_tiles,   384, G_SPR_BALL_TILES_SIZE);
    SMS_loadTiles(g_spr_shadow_tiles, 396, G_SPR_SHADOW_TILES_SIZE);

    /* Hide all sprites initially */
    SMS_initSprites();
    SMS_copySpritestoSAT();

    /* Configure text renderer: ASCII 'A'=65 → tile BG_TILE_FONT_BASE + (65-32) */
    SMS_configureTextRenderer((signed int)(BG_TILE_FONT_BASE - 32));

    /* Default game settings */
    g_game_points  = SCORE_MAX;
    g_max_bounce   = 1;
    g_max_dribble  = 3;
    g_frame_count  = 0;
    g_prev_keys    = 0;
    g_sfx_frames   = 0;

    /* Start at menu */
    g_current_state = state_menu_init;

    SMS_displayOn();

    /* ======== MAIN LOOP ======== */
    for (;;) {
        SMS_waitForVBlank();

        /* Always draw sprites for the current frame */
        if (g_current_state == state_game    ||
            g_current_state == state_point   ||
            g_current_state == state_victory) {
            draw_all_sprites();
        } else {
            SMS_initSprites();
            SMS_copySpritestoSAT();
        }

        psg_update();     /* decay SFX */

        /* Run game state */
        g_current_state();

        g_frame_count++;
    }
}
