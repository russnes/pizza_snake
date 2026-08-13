/*---------------------------------------------------------------------------------
    PIZZA SNAKE - SNES
---------------------------------------------------------------------------------*/
#include <snes.h>
#include <string.h>

#include "trig_table.h"
#include "tiles_charmap.h"
#include "sprite_tiles.h"

extern char gfxsprites, gfxsprites_end;
extern char palsprites, palsprites_end;
extern char gfxwalls, gfxwalls_end;
extern char palwalls, palwalls_end;

//---------------------------------------------------------------------------------
// Arena bounds (pixel space)
#define ARENA_LEFT   14
#define ARENA_RIGHT  242
#define ARENA_TOP    30
#define ARENA_BOTTOM 210

#define TRAILSIZE    1024
#define MAX_LEN      3000  /* effectively unlimited; baking removes the OAM ceiling */
#define SPRITE_WINDOW 55   /* most recent segments rendered as smooth sprites --
                               kept modest so per-frame sprite updates stay cheap;
                               everything beyond this is baked onto the BG layer */
#define SEG_SPACING  2
#define BAKE_STEP    1     /* sample the trail every frame for bake/erase
                               classification, finer than SEG_SPACING so
                               entry/exit points are captured precisely */
#define TURN_RATE    5
#define MOVESPEED    210   /* desired sub-pixel speed per frame, Q7 (128=1px), ~1.6px/frame */
#define SELF_SKIP    8
#define SELF_R       3     /* pixel radius, cheap abs-based check (thin body) */
#define PIZZA_R      6
#define OAM_PIZZA    0
#define OAM_HEAD     4
#define OAM_BODY0    8

#define ST_TITLE     0
#define ST_PLAYING   1
#define ST_GAMEOVER  2
#define ST_ENTERNAME 3
#define ST_HISCORES  4

#define NAME_LEN     3
#define NUM_SCORES   20

//---------------------------------------------------------------------------------
u16 blockmap[0x400];

s16 trailX[TRAILSIZE];
s16 trailY[TRAILSIZE];
u16 headIdx;

u8 angle;
s32 posX, posY; /* Q7 fixed point (scaled by 128) */
u16 curLen;
u16 score;
u16 pizzaX, pizzaY;
u16 pizzaValue;
u8 pizzaBig;
u8 gameState;
u16 randSeed;
u16 pad0;
u8 hudDirty;
u16 oamHighWater;
u16 snakeAge;
u16 nextBakeSample;
u16 nextEraseSample;
u16 lastErasedCell;

/* Baked background body: each tile that a segment ages out into gets
   classified by which two edges the path enters/exits through, and one of
   3 fixed tiles (H, V, corner -- flipped as needed) is picked to connect
   those exact edge-midpoints. Every tile touches its edges at the same
   points, so neighbours always connect at consistent width regardless of
   how much of the cell the path happened to cross -- and picking a
   pre-made tile is far cheaper than computing one. */
u16 pendingCell;
u8 pendingEntryX, pendingEntryY;
u8 pendingExitX, pendingExitY;

#define MAX_DIRTY 16
u16 dirtyCell[MAX_DIRTY];
u8 dirtyCount;

char hiName[NAME_LEN + 1];
u8 nameLetters[NAME_LEN];
u8 nameCursor;

u16 hsScore[NUM_SCORES];
char hsName[NUM_SCORES][NAME_LEN + 1];

//---------------------------------------------------------------------------------
u16 nextRand(void)
{
    /* simple LCG, good enough for pizza placement */
    randSeed = randSeed * 25173 + 13849;
    return randSeed;
}

//---------------------------------------------------------------------------------
void clearMapRegion(u16 startCell, u16 count, u16 tile)
{
    u16 i;
    for (i = 0; i < count; i++)
        blockmap[startCell + i] = tile;
}

//---------------------------------------------------------------------------------
void writeText(u16 col, u16 row, const char *st)
{
    u16 p = row * 32 + col;
    u8 c;
    while ((c = *st))
    {
        blockmap[p] = asciiTile[c & 0x7F];
        p++;
        st++;
    }
}

//---------------------------------------------------------------------------------
void writeNum(u16 col, u16 row, u16 num, u8 len)
{
    u16 p = row * 32 + col + len - 1;
    u8 i;
    for (i = 0; i < len; i++)
    {
        u8 figure = num % 10;
        blockmap[p] = asciiTile[figure + '0'];
        num /= 10;
        p--;
    }
}

//---------------------------------------------------------------------------------
void drawWalls(void)
{
    u8 x, y;
    clearMapRegion(0, 0x400, TILE_BLANK);
    for (x = 0; x < 32; x++)
        blockmap[2 * 32 + x] = TILE_WALL;
    for (x = 0; x < 32; x++)
        blockmap[27 * 32 + x] = TILE_WALL;
    for (y = 3; y < 27; y++)
    {
        blockmap[y * 32 + 0] = TILE_WALL;
        blockmap[y * 32 + 31] = TILE_WALL;
    }
}

//---------------------------------------------------------------------------------
void hideAllSprites(void)
{
    u16 i;
    for (i = 0; i < 128; i++)
        oamSetVisible(i * 4, OBJ_HIDE);
    oamHighWater = 0;
}

//---------------------------------------------------------------------------------
void spawnPizza(void)
{
    u16 r;
    pizzaX = ARENA_LEFT + 8 + (nextRand() % (ARENA_RIGHT - ARENA_LEFT - 16));
    pizzaY = ARENA_TOP + 8 + (nextRand() % (ARENA_BOTTOM - ARENA_TOP - 16));

    r = nextRand() % 10;
    if (r < 3)
    {
        /* big pizza: rarer, worth more, grows more */
        pizzaBig = 1;
        pizzaValue = 20 + (nextRand() % 20); /* 20-39 */
        oamSetEx(OAM_PIZZA, OBJ_LARGE, OBJ_SHOW);
        oamSet(OAM_PIZZA, pizzaX - 8, pizzaY - 8, 2, 0, 0, SPR_PIZZA_BIG, 0);
    }
    else
    {
        pizzaBig = 0;
        pizzaValue = 5 + (nextRand() % 10); /* 5-14 */
        oamSetEx(OAM_PIZZA, OBJ_SMALL, OBJ_SHOW);
        oamSet(OAM_PIZZA, pizzaX - 4, pizzaY - 4, 2, 0, 0, SPR_PIZZA_SMALL, 0);
    }
}

//---------------------------------------------------------------------------------
void resetGame(void)
{
    u16 i;
    posX = ((s32)128) << 7;
    posY = ((s32)112) << 7;
    angle = 0;
    curLen = 10;
    score = 0;
    headIdx = 0;
    hudDirty = 1;
    snakeAge = 0;
    nextBakeSample = 0;
    nextEraseSample = 0;
    lastErasedCell = 0xFFFF;
    dirtyCount = 0;
    pendingCell = 0xFFFF;
    for (i = 0; i < TRAILSIZE; i++)
    {
        trailX[i] = 128;
        trailY[i] = 112;
    }
    drawWalls();
    hideAllSprites();
    oamHighWater = 2; /* oam #0 (pizza) and #1 (head) are managed separately below */
    oamSetEx(OAM_HEAD, OBJ_SMALL, OBJ_SHOW);
    oamSet(OAM_HEAD, (u16)(posX >> 7) - 4, (u16)(posY >> 7) - 4, 3, 0, 0, SPR_HEAD, 0);
    spawnPizza();
}

//---------------------------------------------------------------------------------
void drawHud(void)
{
    writeText(1, 0, "SCORE");
    writeNum(7, 0, score, 6);
    writeText(18, 0, "HI");
    if (hsScore[0])
        writeText(21, 0, hsName[0]);
    writeNum(25, 0, hsScore[0], 6);
}

//---------------------------------------------------------------------------------
void drawSprites(void)
{
    u16 i;
    u16 oamId;
    u16 hx, hy;
    u16 activeCount;
    u16 bodyCount;

    /* head: position only -- tile/attrs were set once in resetGame().
       Written directly into the OAM shadow buffer (auto-DMA'd at vblank by
       the library) instead of going through oamSetXY -- avoids a function
       call per sprite per frame, which matters once dozens of segments are
       being updated every single frame. */
    hx = (u16)(posX >> 7);
    hy = (u16)(posY >> 7);
    oamMemory[OAM_HEAD + 0] = (u8)(hx - 4);
    oamMemory[OAM_HEAD + 1] = (u8)(hy - 4);

    /* only the most recent SPRITE_WINDOW segments are rendered as sprites;
       older segments get permanently baked onto the BG layer in stepSnake(),
       which is how the snake can grow far past the 128-sprite hardware limit */
    bodyCount = curLen;
    if (bodyCount > SPRITE_WINDOW)
        bodyCount = SPRITE_WINDOW;

    /* newly active body slots: set tile/attr once (never changes again --
       a single fixed shape, so there's nothing to update per frame beyond
       position, and no risk of the flicker a per-frame-changing shape
       would cause) */
    activeCount = 2 + bodyCount;
    if (activeCount > oamHighWater)
    {
        for (i = oamHighWater; i < activeCount; i++)
        {
            oamSetEx(i * 4, OBJ_SMALL, OBJ_SHOW);
            oamMemory[i * 4 + 2] = SPR_BODY;
            oamMemory[i * 4 + 3] = OAM_ATTR(1, 0, 0, SPR_BODY, 0);
        }
        oamHighWater = activeCount;
    }

    /* every active body segment: cheap position-only update each frame */
    for (i = 0; i < bodyCount; i++)
    {
        u16 tIdx = (headIdx + TRAILSIZE - (u16)((i + 1) * SEG_SPACING)) % TRAILSIZE;
        oamId = OAM_BODY0 + i * 4;
        oamMemory[oamId + 0] = (u8)(trailX[tIdx] - 4);
        oamMemory[oamId + 1] = (u8)(trailY[tIdx] - 4);
    }
}

//---------------------------------------------------------------------------------
u8 classifyEdge(u8 x, u8 y)
{
    /* Which of the tile's 4 edges a local point is closest to: 0=left,
       1=right, 2=top, 3=bottom. */
    u8 best = x;          /* distance to left edge */
    u8 edge = 0;
    u8 d;
    d = 7 - x;             /* distance to right edge */
    if (d < best) { best = d; edge = 1; }
    d = y;                  /* distance to top edge */
    if (d < best) { best = d; edge = 2; }
    d = 7 - y;              /* distance to bottom edge */
    if (d < best) { best = d; edge = 3; }
    return edge;
}

//---------------------------------------------------------------------------------
void finalizeTile(u16 cell, u8 ex, u8 ey, u8 xx, u8 xy)
{
    /* Classify which edges the path entered and exited this tile through,
       then pick whichever of the 3 fixed connector tiles (H, V, corner --
       flipped as needed) joins those two edges. All 3 tiles touch their
       edges at identical points, so however the path actually curved
       within this specific cell, the result connects to its neighbours at
       constant width -- no per-tile shape variation, and no runtime
       drawing beyond picking one of 3 known tiles. */
    u8 e1 = classifyEdge(ex, ey);
    u8 e2 = classifyEdge(xx, xy);
    u16 tileVal;

    if ((e1 == 0 && e2 == 1) || (e1 == 1 && e2 == 0))
        tileVal = TILE_BODY_H;
    else if ((e1 == 2 && e2 == 3) || (e1 == 3 && e2 == 2))
        tileVal = TILE_BODY_V;
    else if (e1 == e2)
        tileVal = (e1 == 0 || e1 == 1) ? TILE_BODY_V : TILE_BODY_H;
    else
    {
        u8 hasTop = (e1 == 2 || e2 == 2);
        u8 hasLeft = (e1 == 0 || e2 == 0);
        u8 hasBottom = (e1 == 3 || e2 == 3);

        tileVal = TILE_BODY_CORNER;
        if (hasTop && hasLeft)
            ; /* base art: top-mid to left-mid, no flip */
        else if (hasTop && !hasLeft)
            tileVal |= 0x4000; /* hflip: top-mid to right-mid */
        else if (hasBottom && hasLeft)
            tileVal |= 0x8000; /* vflip: bottom-mid to left-mid */
        else
            tileVal |= 0xC000; /* hflip+vflip: bottom-mid to right-mid */
    }

    blockmap[cell] = tileVal;
    if (dirtyCount < MAX_DIRTY)
        dirtyCell[dirtyCount++] = cell;
}

//---------------------------------------------------------------------------------
u8 stepSnake(void)
{
    s32 dx, dy;
    u16 hx, hy;
    u16 i;

    /* left/right reversed per player preference */
    if (pad0 & KEY_LEFT)
        angle += TURN_RATE;
    if (pad0 & KEY_RIGHT)
        angle -= TURN_RATE;

    dx = ((s32)cosTable[angle] * MOVESPEED) >> 7;
    dy = -(((s32)sinTable[angle] * MOVESPEED) >> 7);
    posX += dx;
    posY += dy;

    hx = (u16)(posX >> 7);
    hy = (u16)(posY >> 7);

    headIdx = (headIdx + 1) % TRAILSIZE;
    trailX[headIdx] = hx;
    trailY[headIdx] = hy;
    snakeAge++;

    if (hx < ARENA_LEFT || hx > ARENA_RIGHT || hy < ARENA_TOP || hy > ARENA_BOTTOM)
        return 1; /* wall death */

    /* Bake segments that have aged out of the sprite-rendering window onto
       the BG tilemap, and erase the oldest baked segment once it falls
       outside the snake's actual current length. Since the snake only grows
       via eating (not via distance travelled), the *total* body length must
       stay fixed at curLen -- without erasing, the baked tail would just
       accumulate the snake's entire lifetime path forever, which is wrong.

       Both cursors are kept pinned to "now" whenever they're not actively
       needed, rather than left frozen, so there is never a backlog to
       catch up on in one frame (which would either freeze the game for a
       long stretch, or -- once the backlog exceeds TRAILSIZE -- read
       positions already overwritten in the circular trail buffer and bake
       tiles at garbage locations). */
    {
        s32 bakeThreshold = (s32)snakeAge - (s32)SPRITE_WINDOW * SEG_SPACING;
        s32 eraseThreshold = (s32)snakeAge - (s32)curLen * SEG_SPACING;

        if (curLen <= SPRITE_WINDOW)
        {
            nextBakeSample = (bakeThreshold > 0) ? (u16)bakeThreshold : 0;
            nextEraseSample = nextBakeSample;
            lastErasedCell = 0xFFFF;
            pendingCell = 0xFFFF;
        }
        else
        {
            u8 guard = 12; /* defensive cap: never more than a handful of ops/frame */
            while ((s32)nextBakeSample <= bakeThreshold && guard--)
            {
                u16 bIdx = nextBakeSample % TRAILSIZE;
                u16 bx = (u16)trailX[bIdx];
                u16 by = (u16)trailY[bIdx];
                u16 cell = (by >> 3) * 32 + (bx >> 3);
                u8 localX = (u8)(bx & 7);
                u8 localY = (u8)(by & 7);

                if (cell != pendingCell)
                {
                    if (pendingCell != 0xFFFF)
                        finalizeTile(pendingCell, pendingEntryX, pendingEntryY,
                                      pendingExitX, pendingExitY);
                    pendingCell = cell;
                    pendingEntryX = localX;
                    pendingEntryY = localY;
                }
                pendingExitX = localX;
                pendingExitY = localY;

                nextBakeSample += BAKE_STEP;
            }

            guard = 12;
            while ((s32)nextEraseSample <= eraseThreshold && guard--)
            {
                u16 eIdx = nextEraseSample % TRAILSIZE;
                u16 ex = (u16)trailX[eIdx];
                u16 ey = (u16)trailY[eIdx];
                u16 cell = (ey >> 3) * 32 + (ex >> 3);
                if (cell != lastErasedCell)
                {
                    blockmap[cell] = TILE_BLANK;
                    if (dirtyCount < MAX_DIRTY)
                        dirtyCell[dirtyCount++] = cell;
                    lastErasedCell = cell;
                }
                nextEraseSample += BAKE_STEP;
            }
        }
    }

    /* cheap abs-based self collision against the sprite-rendered window,
       sampling every other segment. Never look further back than the snake
       has actually been alive for, or we'd read the trail buffer's initial
       spawn-point stub data and get a false "collision" near the start of
       a life. */
    {
        u16 maxCheck = curLen;
        u16 ageLimit = snakeAge / SEG_SPACING;
        if (ageLimit < maxCheck)
            maxCheck = ageLimit;
        if (maxCheck > SPRITE_WINDOW)
            maxCheck = SPRITE_WINDOW;

        for (i = SELF_SKIP; i < maxCheck; i += 2)
        {
            u16 tIdx = (headIdx + TRAILSIZE - (u16)(i * SEG_SPACING)) % TRAILSIZE;
            s16 adx = hx - trailX[tIdx];
            s16 ady = hy - trailY[tIdx];
            if (adx < 0) adx = -adx;
            if (ady < 0) ady = -ady;
            if (adx < SELF_R && ady < SELF_R)
                return 1; /* self collision */
        }
    }

    /* collision against the older, permanently-baked tail: O(1) BG lookup */
    if (curLen > SPRITE_WINDOW)
    {
        u16 cell = (hy >> 3) * 32 + (hx >> 3);
        u16 t = blockmap[cell] & 0x3FF; /* mask off flip bits */
        if (t == TILE_BODY_H || t == TILE_BODY_V || t == TILE_BODY_CORNER)
            return 1;
    }

    {
        s16 adx = (s16)hx - (s16)pizzaX;
        s16 ady = (s16)hy - (s16)pizzaY;
        u8 r = pizzaBig ? (PIZZA_R + 4) : PIZZA_R;
        if (adx < 0) adx = -adx;
        if (ady < 0) ady = -ady;
        if (adx < r && ady < r)
        {
            score += pizzaValue;
            curLen += pizzaValue / 3;
            if (curLen > MAX_LEN)
                curLen = MAX_LEN;
            hudDirty = 1;
            spawnPizza();
        }
    }

    return 0;
}

//---------------------------------------------------------------------------------
u8 qualifiesForHighScore(void)
{
    return (score > 0 && score > hsScore[NUM_SCORES - 1]);
}

//---------------------------------------------------------------------------------
void insertHighScore(u16 s, const char *name)
{
    s8 slot, i;
    slot = -1;
    for (i = 0; i < NUM_SCORES; i++)
    {
        if (s > hsScore[i])
        {
            slot = i;
            break;
        }
    }
    if (slot < 0)
        return;
    for (i = NUM_SCORES - 1; i > slot; i--)
    {
        hsScore[i] = hsScore[i - 1];
        hsName[i][0] = hsName[i - 1][0];
        hsName[i][1] = hsName[i - 1][1];
        hsName[i][2] = hsName[i - 1][2];
        hsName[i][3] = hsName[i - 1][3];
    }
    hsScore[slot] = s;
    hsName[slot][0] = name[0];
    hsName[slot][1] = name[1];
    hsName[slot][2] = name[2];
    hsName[slot][3] = 0;
}

//---------------------------------------------------------------------------------
void drawNameRow(u16 row, u8 blinkOn)
{
    u8 i;
    for (i = 0; i < NAME_LEN; i++)
    {
        if (i == nameCursor && !blinkOn)
            blockmap[row * 32 + 14 + i] = TILE_BLANK;
        else
            blockmap[row * 32 + 14 + i] = asciiTile[(u8)('A' + nameLetters[i])];
    }
}

//---------------------------------------------------------------------------------
void nameEntryScreen(void)
{
    u16 padOld = 0;
    u16 frameCount = 0;
    u16 pressed;
    u8 i;

    for (i = 0; i < NAME_LEN; i++)
        nameLetters[i] = 0;
    nameCursor = 0;

    drawWalls();
    writeText(6, 11, "NEW HIGH SCORE");
    writeText(9, 13, "SCORE");
    writeNum(15, 13, score, 6);
    writeText(7, 17, "ENTER YOUR NAME");
    writeText(9, 21, "LEFT RIGHT PICK");
    writeText(6, 22, "UP DOWN CHANGE LETTER");

    for (;;)
    {
        pad0 = padsCurrent(0);
        pressed = pad0 & (u16)(~padOld);

        if (pressed & KEY_LEFT)
            nameCursor = (nameCursor == 0) ? 0 : nameCursor - 1;
        if (pressed & KEY_RIGHT)
            nameCursor = (nameCursor >= NAME_LEN - 1) ? nameCursor : nameCursor + 1;
        if (pressed & KEY_UP)
            nameLetters[nameCursor] = (nameLetters[nameCursor] + 25) % 26;
        if (pressed & KEY_DOWN)
            nameLetters[nameCursor] = (nameLetters[nameCursor] + 1) % 26;
        padOld = pad0;

        drawNameRow(19, (frameCount >> 4) & 1);

        WaitForVBlank();
        dmaCopyVram((u8 *)blockmap, 0x0000, 0x800);
        frameCount++;

        if (pressed & KEY_START)
            break;
    }

    for (i = 0; i < NAME_LEN; i++)
        hiName[i] = 'A' + nameLetters[i];
    hiName[NAME_LEN] = 0;

    insertHighScore(score, hiName);
}

//---------------------------------------------------------------------------------
void hiScoresScreen(void)
{
    u8 i;
    drawWalls();
    writeText(11, 3, "HIGH SCORES");
    for (i = 0; i < NUM_SCORES; i++)
    {
        u16 row = 5 + i;
        writeNum(3, row, i + 1, 2);
        if (hsScore[i])
        {
            writeText(7, row, hsName[i]);
            writeNum(16, row, hsScore[i], 6);
        }
    }
    WaitForVBlank();
    dmaCopyVram((u8 *)blockmap, 0x0000, 0x800);
    for (;;)
    {
        pad0 = padsCurrent(0);
        WaitForVBlank();
        if (pad0 & (KEY_START | KEY_B | KEY_A))
            break;
    }
    gameState = ST_TITLE;
}

//---------------------------------------------------------------------------------
void titleScreen(void)
{
    drawWalls();
    writeText(10, 8, "PIZZA SNAKE");
    writeText(8, 12, "PRESS START");
    writeText(3, 14, "PRESS SELECT FOR SCORES");
    if (hsScore[0])
    {
        writeText(7, 18, "HI SCORE");
        writeText(16, 18, hsName[0]);
        writeNum(20, 18, hsScore[0], 6);
    }
    WaitForVBlank();
    dmaCopyVram((u8 *)blockmap, 0x0000, 0x800);
    for (;;)
    {
        pad0 = padsCurrent(0);
        WaitForVBlank();
        if (pad0 & KEY_START)
        {
            resetGame();
            gameState = ST_PLAYING;
            return;
        }
        if (pad0 & KEY_SELECT)
        {
            gameState = ST_HISCORES;
            return;
        }
    }
}

//---------------------------------------------------------------------------------
void gameOverScreen(void)
{
    hideAllSprites();

    if (qualifiesForHighScore())
    {
        nameEntryScreen();
        gameState = ST_TITLE;
        return;
    }

    drawWalls();
    writeText(11, 12, "GAME OVER");
    writeText(9, 15, "SCORE");
    writeNum(15, 15, score, 6);
    writeText(6, 18, "PRESS START");
    WaitForVBlank();
    dmaCopyVram((u8 *)blockmap, 0x0000, 0x800);
    for (;;)
    {
        pad0 = padsCurrent(0);
        WaitForVBlank();
        if (pad0 & KEY_START)
            break;
    }
    gameState = ST_TITLE;
}

//---------------------------------------------------------------------------------
int main(void)
{
    u16 i;

    setBrightness(0);
    WaitForVBlank();

    randSeed = 0xACE1;
    for (i = 0; i < NUM_SCORES; i++)
    {
        hsScore[i] = 0;
        hsName[i][0] = 'A'; hsName[i][1] = 'A'; hsName[i][2] = 'A'; hsName[i][3] = 0;
    }
    hiName[0] = 'A'; hiName[1] = 'A'; hiName[2] = 'A'; hiName[3] = 0;
    gameState = ST_TITLE;

    oamInitGfxSet(&gfxsprites, (&gfxsprites_end - &gfxsprites),
                   &palsprites, (&palsprites_end - &palsprites),
                   0, 0x4000, OBJ_SIZE8_L16);

    dmaCopyVram(&gfxwalls, 0x1000, (&gfxwalls_end - &gfxwalls));
    dmaCopyCGram((u8 *)&palwalls, 0, (&palwalls_end - &palwalls));

    drawWalls();
    WaitForVBlank();
    dmaCopyVram((u8 *)blockmap, 0x0000, 0x800);

    bgSetGfxPtr(0, 0x1000);
    bgSetMapPtr(0, 0x0000, SC_32x32);

    setMode(BG_MODE1, 0);
    bgSetDisable(1);
    bgSetDisable(2);

    hideAllSprites();
    setScreenOn();

    for (;;)
    {
        switch (gameState)
        {
        case ST_TITLE:
            titleScreen();
            break;

        case ST_HISCORES:
            hiScoresScreen();
            break;

        case ST_PLAYING:
            pad0 = padsCurrent(0);
            if (stepSnake())
            {
                gameState = ST_GAMEOVER;
                break;
            }
            if (hudDirty)
                drawHud();
            drawSprites();
            WaitForVBlank();
            if (hudDirty)
            {
                /* HUD text only ever touches row 0 -- 32 cells, 64 bytes */
                dmaCopyVram((u8 *)blockmap, 0x0000, 64);
                hudDirty = 0;
            }
            {
                u8 i;
                for (i = 0; i < dirtyCount; i++)
                    dmaCopyVram((u8 *)&blockmap[dirtyCell[i]], dirtyCell[i], 2);
                dirtyCount = 0;
            }
            break;

        case ST_GAMEOVER:
            gameOverScreen();
            break;
        }
    }
    return 0;
}
