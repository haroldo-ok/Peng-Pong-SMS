/* smslib_sprites.c - sprite/tile functions missing from packaged SMSlib.rel
 * Provides: SMS_initSprites, SMS_addSprite, SMS_copySpritestoSAT,
 *           SMS_loadTiles, SMS_configureTextRenderer
 *
 * SMS Mode 4 SAT layout (SAT base = 0x3F00 per VDP reg5=0xFF default):
 *   Y table:    VRAM 0x3F00-0x3F3F  (64 bytes, one Y per sprite)
 *   X/tile tbl: VRAM 0x3F80-0x3FFF  (128 bytes, 2 bytes/sprite: X then tile)
 */

__sfr __at 0xBF VDPCtrl;
__sfr __at 0xBE VDPData;

#define DI  __asm di __endasm
#define EI  __asm ei __endasm

#define SAT_Y_BASE    ((unsigned int)0x3F00)
#define SAT_XT_BASE   ((unsigned int)0x3F80)
#define MAXSPRITES    64
#define OFF_SCREEN_Y  0xD0

static unsigned char _sy[MAXSPRITES];
static unsigned char _sx[MAXSPRITES];
static unsigned char _st[MAXSPRITES];
static unsigned char _cnt;

static void vram_wr_addr(unsigned int a) {
    DI;
    VDPCtrl = (unsigned char)(a & 0xFF);
    VDPCtrl = (unsigned char)(((a >> 8) & 0x3F) | 0x40);
    EI;
}

void SMS_initSprites(void) {
    unsigned char i;
    _cnt = 0;
    for (i = 0; i < MAXSPRITES; i++) _sy[i] = OFF_SCREEN_Y;
}

void SMS_addSprite(unsigned char x, unsigned char y, unsigned char tile) {
    if (_cnt >= MAXSPRITES) return;
    _sy[_cnt] = y;
    _sx[_cnt] = x;
    _st[_cnt] = tile;
    _cnt++;
}

void SMS_copySpritestoSAT(void) {
    unsigned char i;
    /* Write Y table */
    vram_wr_addr(SAT_Y_BASE);
    for (i = 0; i < _cnt; i++) VDPData = _sy[i];
    VDPData = OFF_SCREEN_Y;   /* list terminator */
    /* Write X/tile table */
    vram_wr_addr(SAT_XT_BASE);
    for (i = 0; i < _cnt; i++) {
        VDPData = _sx[i];
        VDPData = _st[i];
    }
}

void SMS_loadTiles(void *src, unsigned int tilefrom, unsigned int size) {
    unsigned int i;
    unsigned int addr;
    unsigned char *p;
    p    = (unsigned char *)src;
    addr = tilefrom << 5;   /* tile index * 32 bytes per tile */
    vram_wr_addr(addr);
    for (i = 0; i < size; i++) VDPData = p[i];
}

/* No-op: we do not use the devkitSMS text renderer */
void SMS_configureTextRenderer(signed int offset) {
    (void)offset;
}
