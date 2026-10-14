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
//
// The walls are a ring of 8x8 tiles (tilemap rows 2 and 27, columns 0 and 31 --
// see drawWalls()), so the tile interior is x 8..247, y 24..215. Every wall tile
// has a 1px near-black outline on all four sides (41,16,16 against the 8,8,8
// floor): effectively invisible, so the wall *looks* 1px thinner than it is.
// That innermost outline pixel is therefore treated as free space (WALL_BORDER),
// and the snake only dies when it would overlap the bright part of the wall.
//
// The head, like every body segment, is a 5x5 dot reaching HEAD_R = 2px out from
// its centre in each direction, and it's the head's *centre* that is tested
// against the ARENA_* bounds. Letting the centre go to (free edge + HEAD_R) puts
// the dot's outer edge exactly on the last free pixel; one pixel further would
// overlap the visible wall, and that is when the snake dies. (These were
// 14/242/30/210 originally, which killed the snake 3-4px before it visibly
// reached the wall.)
//
// The bounds still keep the head's centre inside the tile interior (x 8..247,
// y 24..215), so the head's tile is never a wall tile and baking the tail can
// never overwrite (or erase) a wall.
#define WALL_BORDER  1
#define PLAY_LEFT    (8 - WALL_BORDER)
#define PLAY_RIGHT   (247 + WALL_BORDER)
#define PLAY_TOP     (24 - WALL_BORDER)
#define PLAY_BOTTOM  (215 + WALL_BORDER)
#define HEAD_R       2
#define ARENA_LEFT   (PLAY_LEFT + HEAD_R)
#define ARENA_RIGHT  (PLAY_RIGHT - HEAD_R)
#define ARENA_TOP    (PLAY_TOP + HEAD_R)
#define ARENA_BOTTOM (PLAY_BOTTOM - HEAD_R)

/* Pizzas spawn in the region they always have (kept clear of the walls for the
   16x16 big pizza); deliberately NOT tied to the death bounds above, so fixing
   the walls doesn't move pizzas or change the random sequence. */
#define SPAWN_LEFT   14
#define SPAWN_RIGHT  242
#define SPAWN_TOP    30
#define SPAWN_BOTTOM 210

#define TRAILSIZE    1024  /* MUST be a power of two: indices wrap with & TRAIL_MASK, not %.
                               The 65816 has no divide instruction, so a u16 % is a slow
                               software routine -- it used to run ~100x per frame and was
                               what made the game drop to half speed past ~45 segments. */
#define TRAIL_MASK   (TRAILSIZE - 1)
#define MAX_LEN      3000  /* effectively unlimited; baking removes the OAM ceiling */
#define SPRITE_WINDOW 55   /* most recent segments rendered as smooth sprites --
                               kept modest so per-frame sprite updates stay cheap;
                               everything beyond this is baked onto the BG layer */
#define SEG_SPACING  2
#define SEG_SHIFT    1     /* log2(SEG_SPACING): drawSprites() uses shifts, not divide/multiply */
#if (1 << SEG_SHIFT) != SEG_SPACING
#error SEG_SHIFT must equal log2(SEG_SPACING)
#endif
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

#define NAME_LEN     8
#define NUM_SCORES   20
#define SCORE_DIGITS 5     /* score is a u16 (max 65535), so 5 digits is exact */
#define NO_RANK      0xFF  /* "this game didn't make the table" */

/* Name entry: each position cycles through A-Z, 0-9 and space. */
#define NAME_CHAR_COUNT 37
#define NAME_ENTRY_COL  ((32 - NAME_LEN) / 2)
#define REPEAT_DELAY    20 /* frames Up/Down must be held before auto-repeat starts */
#define REPEAT_RATE     4  /* frames between repeats once it has started */

/* High score table screen layout (tile columns/rows). */
#define HS_COL_RANK   6
#define HS_COL_NAME   10
#define HS_COL_SCORE  20
#define HS_ROW_FIRST  5

/* Battery-backed SRAM save format (see saveHighScores()):
     [0..3]  magic "PZSN"
     [4]     NAME_LEN        } so changing either constant invalidates old
     [5]     NUM_SCORES      } saves instead of misreading them
     [6..]   NUM_SCORES x { NAME_LEN name bytes, score lo, score hi }
     last 2  checksum over everything before it (lo, hi) */
#define SAVE_HDR_LEN    6
#define SAVE_ENTRY_LEN  (NAME_LEN + 2)
#define SAVE_BODY_LEN   (SAVE_HDR_LEN + NUM_SCORES * SAVE_ENTRY_LEN)
#define SAVE_SIZE       (SAVE_BODY_LEN + 2)

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
u8 hudFull;               /* 1 = labels + hi score must be (re)drawn too, not just the score */
u16 oamHighWater;
u16 snakeAge;
u16 nextBakeSample;
u16 nextEraseSample;
u16 lastErasedCell;
u16 prevEraseX, prevEraseY;

/* Baked background body: every touch point the path makes with a tile
   boundary is represented as (edge, along) -- which of the 4 edges, and
   which of the 8 pixel positions along it -- packed into a single 0-31
   index via POINT(edge,along). This is deliberately NOT a raw (x,y)
   pixel: a point sitting exactly at a tile corner is genuinely ambiguous
   as a bare coordinate (it's simultaneously "column 0" and "row 0"), and
   the two readings need different stub directions when rendered. Keeping
   the edge explicit removes the ambiguity at its source instead of
   re-guessing it later.

   TAIL_CONNECTOR_TILE[a*32+b] (generated by gen_tiles.py) maps any pair
   of touch points straight to a background tile value (index + flip
   bits) -- a plain lookup, no runtime branching on cases.

   Both cells sharing a boundary must reference the EXACT same touch
   point for the connection to render seamlessly, so whichever cell is
   entered borrows its along-coordinate from the cell being exited (see
   stepSnake): a raw per-frame sample lands within ~2px of the boundary
   it just crossed, not exactly on it, so using each side's own sample
   independently can disagree by a pixel and leave a hairline seam. A
   diagonal step (the path clips a tile corner between two samples, never
   landing in either cell that shares a full edge with both neighbours)
   is resolved by interpolating the straight line between the last two
   samples to find which of the two candidate cells it actually passed
   through, and synthesizing that skipped cell too. */
#define POINT(edge, along) ((u8)((edge) * 8 + (along)))
u16 pendingCell;
u8 pendingEntryPoint;
u16 prevBakeX, prevBakeY;

#define MAX_DIRTY 16
u16 dirtyCell[MAX_DIRTY];
u8 dirtyCount;

char hiName[NAME_LEN + 1];
u8 nameLetters[NAME_LEN];
u8 nameCursor;

u16 hsScore[NUM_SCORES];
char hsName[NUM_SCORES][NAME_LEN + 1];
u8 newRank;               /* table slot of the score just entered, or NO_RANK */
u8 saveBuf[SAVE_SIZE];    /* staging buffer for SRAM reads/writes */

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
u16 div10(u16 n)
{
    /* n / 10 without a divide (shift-and-add, Hacker's Delight). Exhaustively
       checked equal to n / 10 for all 65536 u16 values. The 65816 has no
       divide instruction, so a real u16 / and % are slow software routines;
       formatting the HUD used to do 20 of them in the one frame a pizza was
       eaten, which on a long snake was enough to blow the frame budget. */
    u16 q = (n >> 1) + (n >> 2);
    u16 r;
    q += q >> 4;
    q += q >> 8;
    q >>= 3;
    r = n - ((q << 3) + (q << 1));   /* q * 10 */
    return q + ((r + 6) >> 4);
}

//---------------------------------------------------------------------------------
void writeNum(u16 col, u16 row, u16 num, u8 len)
{
    u16 p = row * 32 + col + len - 1;
    u16 q;
    u8 i;
    for (i = 0; i < len; i++)
    {
        q = div10(num);
        blockmap[p] = asciiTile[(u8)(num - ((q << 3) + (q << 1))) + '0'];
        num = q;
        p--;
    }
}

//---------------------------------------------------------------------------------
void writeRepeat(u16 col, u16 row, char ch, u8 count)
{
    u16 p = row * 32 + col;
    u8 i;
    for (i = 0; i < count; i++)
        blockmap[p + i] = asciiTile[(u8)ch & 0x7F];
}

//---------------------------------------------------------------------------------
void waitReleased(u16 mask)
{
    /* Block until none of the buttons in `mask` is held. Every screen that
       reacts to START/A/B calls this before it starts listening: the button
       press that brought the player here (e.g. START confirming a name) is
       usually still physically down for a few frames, and without this it
       would instantly dismiss the *next* screen too -- the high score table
       would flash past unseen. Only the buttons the screen itself uses are
       waited on, so holding a d-pad direction from the moment of death
       doesn't stall anything. */
    while (padsCurrent(0) & mask)
        WaitForVBlank();
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
    pizzaX = SPAWN_LEFT + 8 + (nextRand() % (SPAWN_RIGHT - SPAWN_LEFT - 16));
    pizzaY = SPAWN_TOP + 8 + (nextRand() % (SPAWN_BOTTOM - SPAWN_TOP - 16));

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
    hudFull = 1;
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
    /* "SCORE nnnnn  HI NAMENAME nnnnn" -- fills cols 1..30 of row 0.
       Only the score value changes during a game, so the labels and the
       high score are drawn just once (hudFull, set by resetGame()); a pizza
       then costs one number instead of the whole line. Text/number drawing
       is slow on this CPU and this runs in the same frame as the pizza
       respawn, so it matters. */
    if (hudFull)
    {
        writeText(1, 0, "SCORE");
        writeText(14, 0, "HI");
        if (hsScore[0])
            writeText(17, 0, hsName[0]);
        writeNum(26, 0, hsScore[0], SCORE_DIGITS);
        hudFull = 0;
    }
    writeNum(7, 0, score, SCORE_DIGITS);
}

//---------------------------------------------------------------------------------
void drawSprites(void)
{
    u16 i;
    u16 hx, hy;
    u16 activeCount;
    u16 bodyCount;
    u16 tIdx;
    u8 *op;

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

    /* every active body segment: cheap position-only update each frame.
       Walks the trail backwards from the head one SEG_SPACING at a time
       (rather than recomputing each index from scratch). */
    tIdx = (headIdx - SEG_SPACING) & TRAIL_MASK;
    op = &oamMemory[OAM_BODY0];
    while (bodyCount)
    {
        /* One contiguous run of the ring buffer: from tIdx back toward index 0
           in steps of SEG_SPACING. Pointer walking is much cheaper than
           re-indexing the arrays every iteration on this compiler. */
        u16 run = (tIdx >> SEG_SHIFT) + 1;   /* entries tIdx, tIdx-2, ... that stay >= 0 */
        s16 *px, *py;
        if (run > bodyCount)
            run = bodyCount;
        px = &trailX[tIdx];
        py = &trailY[tIdx];
        bodyCount -= run;
        tIdx = (tIdx - (run << SEG_SHIFT)) & TRAIL_MASK;   /* wraps to the top of the buffer */
        while (run--)
        {
            op[0] = (u8)(*px - 4);
            op[1] = (u8)(*py - 4);
            op += 4;
            px -= SEG_SPACING;
            py -= SEG_SPACING;
        }
    }
}

//---------------------------------------------------------------------------------
s16 abs16(s16 v)
{
    return (v < 0) ? -v : v;
}

//---------------------------------------------------------------------------------
s16 divRound(s16 num, s16 den)
{
    /* Round num/den to the nearest integer (ties away from zero) instead
       of truncating toward zero -- plain integer division here would bias
       the rare diagonal-interpolation case by up to a pixel, e.g. -1/-2
       truncates to 0 when the mathematically nearer answer is the same
       either way, but 103.5 truncates down to 103 instead of rounding to
       104. Doesn't affect whether two cells' touch points agree (both
       sides of a junction always use this same computed value), only how
       close that shared value lands to the true crossing point.
       Deliberately a real function, not a macro: num/den are often
       themselves multi-step expressions (see interpAlong), and a macro
       that mentions its arguments more than once would recompute those
       expressions repeatedly after expansion -- which is exactly what
       blew up 816-tcc's stack-temporary count enough to overflow the
       assembler's 8-bit stack-relative addressing here. */
    s16 aNum = abs16(num);
    s16 aDen = abs16(den);
    s16 mag;
    /* This is all 16-bit on purpose: every value here is tiny (positions are
       within a tile of each other, per-frame movement is at most 2 px, so
       products stay under ~250 -- checked exhaustively against the old 32-bit
       version over every reachable input), and a 32-bit multiply/divide is a
       very slow software routine on the 65816. The denominator is a one-frame
       movement delta, so it is nearly always 1 or 2 -- shifts, not a divide. */
    if (aDen == 1)
        mag = aNum;
    else if (aDen == 2)
        mag = (aNum + 1) >> 1;
    else
        mag = (aNum + aDen / 2) / aDen;
    return ((num < 0) == (den < 0)) ? mag : -mag;
}

u8 axisDir(u16 fromCell, u16 toCell)
{
    /* Which edge of fromCell is crossed to reach toCell (0=left,1=right,
       2=top,3=bottom) -- fromCell's EXIT edge, exact, no guessing.
       toCell's matching entry edge is always the opposite (XOR 1, since
       this encoding pairs 0<->1 and 2<->3). Returns 0xFF if the two cells
       aren't simple axis neighbours (the path clipped a corner and
       stepped diagonally between samples). */
    s16 dCol = (s16)(toCell & 31) - (s16)(fromCell & 31);   /* & 31 / >> 5 == % 32 / / 32 for u16, */
    s16 dRow = (s16)(toCell >> 5) - (s16)(fromCell >> 5);   /* minus the slow software divide      */
    if (dCol == 1 && dRow == 0) return 1;  /* exits fromCell's right edge */
    if (dCol == -1 && dRow == 0) return 0; /* exits fromCell's left edge */
    if (dRow == 1 && dCol == 0) return 3;  /* exits fromCell's bottom edge */
    if (dRow == -1 && dCol == 0) return 2; /* exits fromCell's top edge */
    return 0xFF;
}

//---------------------------------------------------------------------------------
void finalizeTile(u16 cell, u8 entryPoint, u8 exitPoint)
{
    /* tailConnectorTile[] (generated by gen_tiles.py) already covers
       every one of the 32x32 possible touch-point combinations, so
       there's no case analysis left to do here -- just look up the tile
       that was pre-built for this exact pair. */
    blockmap[cell] = tailConnectorTile[(u16)entryPoint * 32 + exitPoint];
    if (dirtyCount < MAX_DIRTY)
        dirtyCell[dirtyCount++] = cell;
}

//---------------------------------------------------------------------------------
u8 bakeSimpleCrossing(u16 pendingCell, u8 pendingEntryPoint, u16 cell, u8 d, u8 localX, u8 localY)
{
    /* Simple axis crossing: borrow the along-edge coordinate from THIS
       sample (the cell being entered) for both cells' shared touch point.
       A raw sample lands within ~2px of the boundary it just crossed, not
       exactly on it, so using each side's own independent sample can
       disagree by a pixel and leave a hairline seam -- sharing one value
       guarantees both sides agree exactly. Returns the touch point the
       newly-entered cell should use as ITS entry. */
    u8 along = (d == 0 || d == 1) ? localY : localX;
    finalizeTile(pendingCell, pendingEntryPoint, POINT(d, along));
    return POINT(d ^ 1, along);
}

//---------------------------------------------------------------------------------
u8 interpAlong(u16 refCell, u8 d, s16 ax, s16 ay, s16 ddx, s16 ddy)
{
    /* Where the straight line from (ax,ay) to (ax+ddx,ay+ddy) crosses
       refCell's edge d, expressed as the local along-edge coordinate.
       Split out from bakeDiagonalCrossing (which needs this twice) purely
       to keep any single function's expression count -- and therefore its
       816-tcc stack-temporary usage -- small enough to fit an 8-bit
       stack-relative offset; see bakeDiagonalCrossing's comment. */
    s16 b;
    if (d == 0 || d == 1)
    {
        b = ((s16)(refCell & 31) + (d == 1 ? 1 : 0)) * 8;
        if (ddx == 0) return (u8)((u16)ay & 7);
        return (u8)((u16)(ay + divRound((b - ax) * ddy, ddx)) & 7);
    }
    b = ((s16)(refCell >> 5) + (d == 3 ? 1 : 0)) * 8;
    if (ddy == 0) return (u8)((u16)ax & 7);
    return (u8)((u16)(ax + divRound((b - ay) * ddx, ddy)) & 7);
}

//---------------------------------------------------------------------------------
u16 diagonalMidCell(u16 pendingCell, u16 cell, s16 ax, s16 ay, s16 ddx, s16 ddy)
{
    /* Which of the two cells diagonally between pendingCell and cell the
       straight line between the last two samples actually clipped
       through -- split out for the same stack-frame-size reason as
       interpAlong above. */
    s16 dCol = (s16)(cell & 31) - (s16)(pendingCell & 31);
    s16 dRow = (s16)(cell >> 5) - (s16)(pendingCell >> 5);
    s16 colBoundary = ((s16)(pendingCell & 31) + (dCol > 0 ? 1 : 0)) * 8;
    s16 rowBoundary = ((s16)(pendingCell >> 5) + (dRow > 0 ? 1 : 0)) * 8;
    s16 tCol = abs16(colBoundary - ax) * abs16(ddy);
    s16 tRow = abs16(rowBoundary - ay) * abs16(ddx);
    if (tCol < tRow)
        return (dCol > 0) ? (u16)(pendingCell + 1) : (u16)(pendingCell - 1);
    return (dRow > 0) ? (u16)(pendingCell + 32) : (u16)(pendingCell - 32);
}

//---------------------------------------------------------------------------------
u8 bakeDiagonalCrossing(u16 pendingCell, u8 pendingEntryPoint, u16 cell,
                         u16 prevX, u16 prevY, u16 curX, u16 curY)
{
    /* Diagonal: the path clipped a tile corner between the last sample and
       this one, skipping the cell that would normally share a full edge
       with both neighbours -- the two cells here only share a single
       corner pixel. Find which of the two candidate cells the straight
       line between the last two samples actually passed through, and
       bake that skipped cell too. The real work is split into
       diagonalMidCell()/interpAlong() above: 816-tcc allocates a stack
       slot per sub-expression rather than per C variable, so a function
       with this many nested computations in one body can overflow the
       8-bit stack-relative addressing the assembler needs -- keeping each
       function's own expression count small avoids that regardless of
       how much total work the whole crossing needs. */
    s16 ax = (s16)prevX, ay = (s16)prevY;
    s16 ddx = (s16)curX - ax, ddy = (s16)curY - ay;
    u16 midCell = diagonalMidCell(pendingCell, cell, ax, ay, ddx, ddy);
    u8 d1 = axisDir(pendingCell, midCell);
    u8 along1 = interpAlong(pendingCell, d1, ax, ay, ddx, ddy);
    u8 d2 = axisDir(midCell, cell);
    u8 along2 = interpAlong(midCell, d2, ax, ay, ddx, ddy);

    finalizeTile(pendingCell, pendingEntryPoint, POINT(d1, along1));
    finalizeTile(midCell, POINT(d1 ^ 1, along1), POINT(d2, along2));
    return POINT(d2 ^ 1, along2);
}

//---------------------------------------------------------------------------------
u8 bakeFirstCell(u8 localX, u8 localY)
{
    /* First cell ever baked: no previous cell to derive an exact touch
       point from, so fall back to whichever edge the raw sample is
       nearest -- a one-time approximation that only ever affects the very
       start of the baked trail. */
    u8 d0 = 0, best = localX, dd;
    dd = (u8)(7 - localX); if (dd < best) { best = dd; d0 = 1; }
    dd = localY;           if (dd < best) { best = dd; d0 = 2; }
    dd = (u8)(7 - localY); if (dd < best) { best = dd; d0 = 3; }
    return POINT(d0, (d0 == 0 || d0 == 1) ? localY : localX);
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

    /* 16-bit multiply on purpose: |table| <= 128 and MOVESPEED = 210 give at
       most 26880, which fits in 16 bits -- and a 32-bit multiply is a slow
       software routine on the 65816 (2 of them ran every single frame) */
    dx = (cosTable[angle] * MOVESPEED) >> 7;
    dy = -((sinTable[angle] * MOVESPEED) >> 7);
    posX += dx;
    posY += dy;

    hx = (u16)(posX >> 7);
    hy = (u16)(posY >> 7);

    headIdx = (headIdx + 1) & TRAIL_MASK;
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
                u16 bIdx = nextBakeSample & TRAIL_MASK;
                u16 bx = (u16)trailX[bIdx];
                u16 by = (u16)trailY[bIdx];
                u16 cell = (by >> 3) * 32 + (bx >> 3);
                u8 localX = (u8)(bx & 7);
                u8 localY = (u8)(by & 7);

                if (cell != pendingCell)
                {
                    u8 entryForNext;
                    if (pendingCell != 0xFFFF)
                    {
                        u8 d = axisDir(pendingCell, cell);
                        if (d != 0xFF)
                            entryForNext = bakeSimpleCrossing(pendingCell, pendingEntryPoint, cell, d, localX, localY);
                        else
                            entryForNext = bakeDiagonalCrossing(pendingCell, pendingEntryPoint, cell,
                                                                 prevBakeX, prevBakeY, bx, by);
                    }
                    else
                    {
                        entryForNext = bakeFirstCell(localX, localY);
                    }
                    pendingCell = cell;
                    pendingEntryPoint = entryForNext;
                }
                prevBakeX = bx;
                prevBakeY = by;

                nextBakeSample += BAKE_STEP;
            }

            guard = 12;
            while ((s32)nextEraseSample <= eraseThreshold && guard--)
            {
                u16 eIdx = nextEraseSample & TRAIL_MASK;
                u16 ex = (u16)trailX[eIdx];
                u16 ey = (u16)trailY[eIdx];
                u16 cell = (ey >> 3) * 32 + (ex >> 3);
                if (cell != lastErasedCell)
                {
                    /* A diagonal skip during baking wrote a tile into a
                       synthesized intermediate cell that no raw sample
                       ever actually lands in (see bakeDiagonalCrossing) --
                       so without this, erasure -- which only ever walks
                       real samples -- would never visit that cell either,
                       leaving it permanently baked: still solid for
                       collision, and still visibly on screen, long after
                       the rest of the tail has moved past it. Detecting
                       the same diagonal skip here and erasing the same
                       synthesized cell keeps erase exactly in step with
                       bake. */
                    if (lastErasedCell != 0xFFFF && axisDir(lastErasedCell, cell) == 0xFF)
                    {
                        u16 midCell = diagonalMidCell(lastErasedCell, cell,
                                                       (s16)prevEraseX, (s16)prevEraseY,
                                                       (s16)ex - (s16)prevEraseX, (s16)ey - (s16)prevEraseY);
                        blockmap[midCell] = TILE_BLANK;
                        if (dirtyCount < MAX_DIRTY)
                            dirtyCell[dirtyCount++] = midCell;
                    }
                    blockmap[cell] = TILE_BLANK;
                    if (dirtyCount < MAX_DIRTY)
                        dirtyCell[dirtyCount++] = cell;
                    lastErasedCell = cell;
                }
                prevEraseX = ex;
                prevEraseY = ey;
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

        u16 tIdx = (headIdx - SELF_SKIP * SEG_SPACING) & TRAIL_MASK;
        for (i = SELF_SKIP; i < maxCheck; i += 2)
        {
            /* |dx| < SELF_R && |dy| < SELF_R, written as one unsigned compare per
               axis: (u16)(d + SELF_R - 1) < 2*SELF_R - 1 is true exactly when
               -SELF_R < d < SELF_R. Same result as abs(), without the branches. */
            u16 adx = (u16)(hx - trailX[tIdx] + (SELF_R - 1));
            u16 ady = (u16)(hy - trailY[tIdx] + (SELF_R - 1));
            if (adx < (2 * SELF_R - 1) && ady < (2 * SELF_R - 1))
                return 1; /* self collision */
            tIdx = (tIdx - 2 * SEG_SPACING) & TRAIL_MASK; /* next sample: i += 2 */
        }
    }

    /* collision against the older, permanently-baked tail: O(1) BG lookup.
       Every body-connector tile generated by gen_tiles.py occupies one
       contiguous block of tile indices starting at TILE_BODY_BASE, so
       "is this cell solid" is a single range check regardless of which
       of the many connector shapes it happens to be. */
    if (curLen > SPRITE_WINDOW)
    {
        u16 cell = (hy >> 3) * 32 + (hx >> 3);
        u16 t = blockmap[cell] & 0x3FF; /* mask off flip bits */
        if (t >= TILE_BODY_BASE && t < TILE_BODY_BASE + TILE_BODY_COUNT)
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
// High score persistence (battery-backed SRAM, 2 KB at $70:0000 on LoROM)
//---------------------------------------------------------------------------------
u16 saveChecksum(u16 len)
{
    /* Position-sensitive rotate-and-add over saveBuf[0..len). Not
       cryptographic -- just enough to notice a blank/garbage/foreign SRAM. */
    u16 sum = 0x5A5A;
    u16 i;
    for (i = 0; i < len; i++)
    {
        sum = (u16)((sum << 1) | (sum >> 15));
        sum += saveBuf[i];
    }
    return sum;
}

//---------------------------------------------------------------------------------
void clearHighScores(void)
{
    u8 i, j;
    for (i = 0; i < NUM_SCORES; i++)
    {
        hsScore[i] = 0;
        for (j = 0; j < NAME_LEN; j++)
            hsName[i][j] = ' ';
        hsName[i][NAME_LEN] = 0;
    }
}

//---------------------------------------------------------------------------------
void saveHighScores(void)
{
    u16 p, sum;
    u8 i, j;

    saveBuf[0] = 'P';
    saveBuf[1] = 'Z';
    saveBuf[2] = 'S';
    saveBuf[3] = 'N';
    saveBuf[4] = NAME_LEN;
    saveBuf[5] = NUM_SCORES;

    p = SAVE_HDR_LEN;
    for (i = 0; i < NUM_SCORES; i++)
    {
        for (j = 0; j < NAME_LEN; j++)
        {
            saveBuf[p] = (u8)hsName[i][j];
            p++;
        }
        saveBuf[p] = (u8)(hsScore[i] & 0xFF);
        saveBuf[p + 1] = (u8)(hsScore[i] >> 8);
        p += 2;
    }

    sum = saveChecksum(SAVE_BODY_LEN);
    saveBuf[SAVE_BODY_LEN] = (u8)(sum & 0xFF);
    saveBuf[SAVE_BODY_LEN + 1] = (u8)(sum >> 8);

    consoleCopySram(saveBuf, SAVE_SIZE);
}

//---------------------------------------------------------------------------------
u8 saveBufIsValid(void)
{
    u16 sum;
    if (saveBuf[0] != 'P' || saveBuf[1] != 'Z' || saveBuf[2] != 'S' || saveBuf[3] != 'N')
        return 0;
    if (saveBuf[4] != NAME_LEN || saveBuf[5] != NUM_SCORES)
        return 0;
    sum = saveChecksum(SAVE_BODY_LEN);
    if (saveBuf[SAVE_BODY_LEN] != (u8)(sum & 0xFF))
        return 0;
    if (saveBuf[SAVE_BODY_LEN + 1] != (u8)(sum >> 8))
        return 0;
    return 1;
}

//---------------------------------------------------------------------------------
void loadHighScores(void)
{
    /* Called once at power-on. Blank, corrupt or foreign SRAM (first boot,
       an emulator that hands back zeroes/0xFF, a flashcart without SRAM
       mapped) falls back to an empty table, which is then written back so
       the next boot finds a valid save. The game still works with no SRAM
       present -- scores just last until power-off. */
    u16 p;
    u8 i, j;

    consoleLoadSram(saveBuf, SAVE_SIZE);
    if (!saveBufIsValid())
    {
        clearHighScores();
        saveHighScores();
        return;
    }

    p = SAVE_HDR_LEN;
    for (i = 0; i < NUM_SCORES; i++)
    {
        for (j = 0; j < NAME_LEN; j++)
        {
            hsName[i][j] = (char)saveBuf[p];
            p++;
        }
        hsName[i][NAME_LEN] = 0;
        hsScore[i] = (u16)saveBuf[p] | ((u16)saveBuf[p + 1] << 8);
        p += 2;
    }
}

//---------------------------------------------------------------------------------
u8 qualifiesForHighScore(void)
{
    return (score > 0 && score > hsScore[NUM_SCORES - 1]);
}

//---------------------------------------------------------------------------------
u8 insertHighScore(u16 s, const char *name)
{
    /* Returns the slot the score landed in, or NO_RANK if it didn't make
       the table. Equal scores rank below existing ones (first to reach a
       score keeps the higher spot). */
    s8 slot, i;
    u8 j;
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
        return NO_RANK;
    for (i = NUM_SCORES - 1; i > slot; i--)
    {
        hsScore[i] = hsScore[i - 1];
        for (j = 0; j <= NAME_LEN; j++)
            hsName[i][j] = hsName[i - 1][j];
    }
    hsScore[slot] = s;
    for (j = 0; j < NAME_LEN; j++)
        hsName[slot][j] = name[j];
    hsName[slot][NAME_LEN] = 0;
    return (u8)slot;
}

//---------------------------------------------------------------------------------
char nameCharFromIndex(u8 idx)
{
    if (idx < 26)
        return (char)('A' + idx);
    if (idx < 36)
        return (char)('0' + (idx - 26));
    return ' ';
}

//---------------------------------------------------------------------------------
void drawNameRow(u16 row, u8 blinkOn)
{
    /* Spaces are drawn as '_' placeholders so all NAME_LEN slots are visible
       while typing (an actual space is invisible), and the cursor blinks
       between its glyph and blank -- which also works on a placeholder. */
    u8 i;
    char c;
    for (i = 0; i < NAME_LEN; i++)
    {
        c = nameCharFromIndex(nameLetters[i]);
        if (c == ' ')
            c = '_';
        if (i == nameCursor && !blinkOn)
            blockmap[row * 32 + NAME_ENTRY_COL + i] = TILE_BLANK;
        else
            blockmap[row * 32 + NAME_ENTRY_COL + i] = asciiTile[(u8)c];
    }
}

//---------------------------------------------------------------------------------
void nameStepChar(u8 up)
{
    if (up)
        nameLetters[nameCursor] = (nameLetters[nameCursor] + NAME_CHAR_COUNT - 1) % NAME_CHAR_COUNT;
    else
        nameLetters[nameCursor] = (nameLetters[nameCursor] + 1) % NAME_CHAR_COUNT;
}

//---------------------------------------------------------------------------------
void nameEntryScreen(void)
{
    u16 padOld;
    u16 frameCount = 0;
    u16 pressed;
    u16 hold = 0;
    u8 i;

    /* Start as "A" followed by spaces: a short name needs no cleanup, and
       with 8 positions that saves a lot of button-mashing. */
    nameLetters[0] = 0;
    for (i = 1; i < NAME_LEN; i++)
        nameLetters[i] = NAME_CHAR_COUNT - 1;
    nameCursor = 0;

    drawWalls();
    writeText(9, 8, "NEW HIGH SCORE");
    writeText(10, 10, "SCORE");
    writeNum(16, 10, score, SCORE_DIGITS);
    writeText(8, 13, "ENTER YOUR NAME");
    writeText(8, 19, "LEFT RIGHT MOVE");
    writeText(9, 21, "UP DOWN CHANGE");
    writeText(9, 23, "START CONFIRM");

    /* Treat anything already held when the game ended (typically the
       d-pad turn button) as old, not as a fresh press. */
    padOld = padsCurrent(0);

    for (;;)
    {
        pad0 = padsCurrent(0);
        pressed = pad0 & (u16)(~padOld);
        padOld = pad0;

        if (pressed & KEY_LEFT)
            nameCursor = (nameCursor == 0) ? 0 : nameCursor - 1;
        if (pressed & KEY_RIGHT)
            nameCursor = (nameCursor >= NAME_LEN - 1) ? nameCursor : nameCursor + 1;

        /* Up/Down: step on press, then auto-repeat while held (there are
           37 characters to cycle through, one press each would be tedious) */
        if (pressed & (KEY_UP | KEY_DOWN))
        {
            nameStepChar((pressed & KEY_UP) ? 1 : 0);
            hold = 0;
        }
        else if (pad0 & (KEY_UP | KEY_DOWN))
        {
            hold++;
            if (hold >= REPEAT_DELAY && ((hold - REPEAT_DELAY) % REPEAT_RATE) == 0)
                nameStepChar((pad0 & KEY_UP) ? 1 : 0);
        }
        else
        {
            hold = 0;
        }

        drawNameRow(15, (frameCount >> 4) & 1);

        WaitForVBlank();
        dmaCopyVram((u8 *)blockmap, 0x0000, 0x800);
        frameCount++;

        if (pressed & KEY_START)
            break;
    }

    for (i = 0; i < NAME_LEN; i++)
        hiName[i] = nameCharFromIndex(nameLetters[i]);
    hiName[NAME_LEN] = 0;

    newRank = insertHighScore(score, hiName);
    if (newRank != NO_RANK)
        saveHighScores();
}

//---------------------------------------------------------------------------------
void drawScoreRow(u8 i, u8 show)
{
    /* One table row: rank always, name + score only when `show` (so the
       freshly-entered row can blink without its rank number jumping). Empty
       slots show dashes. */
    u16 row = HS_ROW_FIRST + i;
    writeNum(HS_COL_RANK, row, i + 1, 2);
    if (!show)
    {
        clearMapRegion(row * 32 + HS_COL_NAME, HS_COL_SCORE + SCORE_DIGITS - HS_COL_NAME, TILE_BLANK);
        return;
    }
    if (hsScore[i])
    {
        writeText(HS_COL_NAME, row, hsName[i]);
        writeNum(HS_COL_SCORE, row, hsScore[i], SCORE_DIGITS);
    }
    else
    {
        writeRepeat(HS_COL_NAME, row, '-', NAME_LEN);
        writeRepeat(HS_COL_SCORE, row, '-', SCORE_DIGITS);
    }
}

//---------------------------------------------------------------------------------
void hiScoresScreen(void)
{
    u8 i;
    u8 blinkOn = 1;
    u8 rowDirty = 0;
    u16 frameCount = 0;

    drawWalls();
    writeText(10, 1, "HIGH SCORES");
    writeText(HS_COL_RANK, 3, "NO");
    writeText(HS_COL_NAME, 3, "NAME");
    writeText(HS_COL_SCORE, 3, "SCORE");
    for (i = 0; i < NUM_SCORES; i++)
        drawScoreRow(i, 1);
    writeText(10, 26, "PRESS START");
    WaitForVBlank();
    dmaCopyVram((u8 *)blockmap, 0x0000, 0x800);

    waitReleased(KEY_START | KEY_A | KEY_B);
    for (;;)
    {
        pad0 = padsCurrent(0);

        /* blink the row the player just earned, so it's easy to spot in a
           table of 20 */
        if (newRank != NO_RANK)
        {
            u8 phase = (u8)((frameCount >> 4) & 1) ^ 1; /* start visible */
            if (phase != blinkOn)
            {
                blinkOn = phase;
                drawScoreRow(newRank, blinkOn);
                rowDirty = 1;
            }
        }

        WaitForVBlank();
        if (rowDirty)
        {
            u16 cell = (u16)(HS_ROW_FIRST + newRank) * 32;
            dmaCopyVram((u8 *)&blockmap[cell], cell, 64);
            rowDirty = 0;
        }
        frameCount++;

        if (pad0 & (KEY_START | KEY_B | KEY_A))
            break;
    }
    newRank = NO_RANK;
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
        writeText(4, 18, "HI SCORE");
        writeText(13, 18, hsName[0]);
        writeNum(22, 18, hsScore[0], SCORE_DIGITS);
    }
    WaitForVBlank();
    dmaCopyVram((u8 *)blockmap, 0x0000, 0x800);

    waitReleased(KEY_START | KEY_SELECT);
    for (;;)
    {
        pad0 = padsCurrent(0);
        WaitForVBlank();
        if (pad0 & KEY_START)
        {
            resetGame();
            /* resetGame() only rebuilt the tilemap in RAM; push it to VRAM so
               the title text doesn't stay painted on the playing field (the
               in-game loop only ever uploads the HUD row and changed cells) */
            WaitForVBlank();
            dmaCopyVram((u8 *)blockmap, 0x0000, 0x800);
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

    /* After every game the player ends up on the high score table:
       qualifying score -> name entry -> table (new row blinking),
       otherwise        -> game over / final score -> table. */
    if (qualifiesForHighScore())
    {
        nameEntryScreen();
        gameState = ST_HISCORES;
        return;
    }

    newRank = NO_RANK;
    drawWalls();
    writeText(11, 12, "GAME OVER");
    writeText(10, 15, "SCORE");
    writeNum(16, 15, score, SCORE_DIGITS);
    writeText(10, 18, "PRESS START");
    WaitForVBlank();
    dmaCopyVram((u8 *)blockmap, 0x0000, 0x800);

    waitReleased(KEY_START);
    for (;;)
    {
        pad0 = padsCurrent(0);
        WaitForVBlank();
        if (pad0 & KEY_START)
            break;
    }
    gameState = ST_HISCORES;
}

//---------------------------------------------------------------------------------
int main(void)
{
    setBrightness(0);
    WaitForVBlank();

    randSeed = 0xACE1;
    newRank = NO_RANK;
    loadHighScores();
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
    /* The SNES starts drawing one line *into* the background: with a vertical
       scroll of 0, tilemap pixel row 0 is never shown and everything sits 1px
       too high -- which clipped the top stroke off the HUD text on tile row 0.
       A scroll of -1 puts tilemap row N on screen line N, matching the pixel
       coordinates the sprites and tile lookups already use. */
    bgSetScroll(0, 0, 0xFFFF);

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
