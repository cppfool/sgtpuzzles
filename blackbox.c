/*
 * blackbox.c: implementation of 'Black Box'.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <ctype.h>
#include <math.h>

#include "puzzles.h"

#define PREFERRED_TILE_SIZE 32
#define FLASH_FRAME 0.2F

/* Terminology, for ease of reading various macros scattered about the place.
 *
 * The 'arena' is the inner area where the balls are placed. This is
 *   indexed from (0,0) to (w-1,h-1) but its offset in the grid is (1,1).
 *
 * The 'range' (firing range) is the bit around the edge where
 *   the lasers are fired from. This is indexed from 0 --> (2*(w+h) - 1),
 *   starting at the top left ((1,0) on the grid) and moving clockwise.
 *
 * The 'grid' is just the big array containing arena and range;
 *   locations (0,0), (0,w+1), (h+1,w+1) and (h+1,0) are unused.
 */

enum {
    COL_BACKGROUND, COL_COVER, COL_LOCK,
    COL_TEXT, COL_FLASHTEXT,
    COL_HIGHLIGHT, COL_LOWLIGHT, COL_GRID,
    COL_BALL, COL_WRONG, COL_BUTTON,
    COL_LASER, COL_DIMLASER,
    NCOLOURS
};

struct game_params {
    int w, h;
    int minballs, maxballs;
};

static game_params *default_params(void)
{
    game_params *ret = snew(game_params);

    ret->w = ret->h = 8;
    ret->minballs = ret->maxballs = 5;

    return ret;
}

static const game_params blackbox_presets[] = {
    { 5, 5, 3, 3 },
    { 8, 8, 5, 5 },
    { 8, 8, 3, 6 },
    { 10, 10, 5, 5 },
    { 10, 10, 4, 10 }
};

static int game_fetch_preset(int i, char **name, game_params **params)
{
    char str[80];
    game_params *ret;

    if (i < 0 || i >= lenof(blackbox_presets))
        return FALSE;

    ret = snew(game_params);
    *ret = blackbox_presets[i];

    if (ret->minballs == ret->maxballs)
        sprintf(str, "%dx%d, %d balls",
                ret->w, ret->h, ret->minballs);
    else
        sprintf(str, "%dx%d, %d-%d balls",
                ret->w, ret->h, ret->minballs, ret->maxballs);

    *name = dupstr(str);
    *params = ret;
    return TRUE;
}

static void free_params(game_params *params)
{
    sfree(params);
}

static game_params *dup_params(game_params *params)
{
    game_params *ret = snew(game_params);
    *ret = *params;		       /* structure copy */
    return ret;
}

static void decode_params(game_params *params, char const *string)
{
    char const *p = string;
    game_params *defs = default_params();

    *params = *defs; free_params(defs);

    while (*p) {
        switch (*p++) {
        case 'w':
            params->w = atoi(p);
            while (*p && isdigit((unsigned char)*p)) p++;
            break;

        case 'h':
            params->h = atoi(p);
            while (*p && isdigit((unsigned char)*p)) p++;
            break;

        case 'm':
            params->minballs = atoi(p);
            while (*p && isdigit((unsigned char)*p)) p++;
            break;

        case 'M':
            params->maxballs = atoi(p);
            while (*p && isdigit((unsigned char)*p)) p++;
            break;

        default:
            ;
        }
    }
}

static char *encode_params(game_params *params, int full)
{
    char str[256];

    sprintf(str, "w%dh%dm%dM%d",
            params->w, params->h, params->minballs, params->maxballs);
    return dupstr(str);
}

static config_item *game_configure(game_params *params)
{
    config_item *ret;
    char buf[80];

    ret = snewn(4, config_item);

    ret[0].name = "Width";
    ret[0].type = C_STRING;
    sprintf(buf, "%d", params->w);
    ret[0].sval = dupstr(buf);
    ret[0].ival = 0;

    ret[1].name = "Height";
    ret[1].type = C_STRING;
    sprintf(buf, "%d", params->h);
    ret[1].sval = dupstr(buf);
    ret[1].ival = 0;

    ret[2].name = "No. of balls";
    ret[2].type = C_STRING;
    if (params->minballs == params->maxballs)
        sprintf(buf, "%d", params->minballs);
    else
        sprintf(buf, "%d-%d", params->minballs, params->maxballs);
    ret[2].sval = dupstr(buf);
    ret[2].ival = 0;

    ret[3].name = NULL;
    ret[3].type = C_END;
    ret[3].sval = NULL;
    ret[3].ival = 0;

    return ret;
}

static game_params *custom_params(config_item *cfg)
{
    game_params *ret = snew(game_params);

    ret->w = atoi(cfg[0].sval);
    ret->h = atoi(cfg[1].sval);

    /* Allow 'a-b' for a range, otherwise assume a single number. */
    if (sscanf(cfg[2].sval, "%d-%d", &ret->minballs, &ret->maxballs) < 2)
        ret->minballs = ret->maxballs = atoi(cfg[2].sval);

    return ret;
}

static char *validate_params(game_params *params, int full)
{
    if (params->w < 2 || params->h < 2)
        return "Grid must be at least 2 wide and 2 high";
    /* next one is just for ease of coding stuff into 'char'
     * types, and could be worked around if required. */
    if (params->w > 255 || params->h > 255)
        return "Grid must be < 255 in each direction";
    if (params->minballs > params->maxballs)
        return "Min. balls must be <= max. balls";
    if (params->minballs >= params->w * params->h)
        return "Too many balls for grid";
    return NULL;
}

/*
 * We store: width | height | ball1x | ball1y | [ ball2x | ball2y | [...] ]
 * all stored as unsigned chars; validate_params has already
 * checked this won't overflow an 8-bit char.
 * Then we obfuscate it.
 */

static char *new_game_desc(game_params *params, random_state *rs,
			   char **aux, int interactive)
{
    int nballs = params->minballs, i;
    char *grid, *ret;
    unsigned char *bmp;

    if (params->maxballs > params->minballs)
        nballs += random_upto(rs, params->maxballs-params->minballs);

    grid = snewn(params->w*params->h, char);
    memset(grid, 0, params->w * params->h * sizeof(char));

    bmp = snewn(nballs*2 + 2, unsigned char);
    memset(bmp, 0, (nballs*2 + 2) * sizeof(unsigned char));

    bmp[0] = params->w;
    bmp[1] = params->h;

    for (i = 0; i < nballs; i++) {
        int x, y;
newball:
        x = random_upto(rs, params->w);
        y = random_upto(rs, params->h);
        if (grid[y*params->h + x]) goto newball;
        grid[y*params->h + x] = 1;
        bmp[(i+1)*2 + 0] = x;
        bmp[(i+1)*2 + 1] = y;
    }
    sfree(grid);

    obfuscate_bitmap(bmp, (nballs*2 + 2) * 8, FALSE);
    ret = bin2hex(bmp, nballs*2 + 2);
    sfree(bmp);

    return ret;
}

static char *validate_desc(game_params *params, char *desc)
{
    int nballs, dlen = strlen(desc), i;
    unsigned char *bmp;
    char *ret;

    /* the bitmap is 2+(nballs*2) long; the hex version is double that. */
    nballs = ((dlen/2)-2)/2;

    if (dlen < 4 || dlen % 4 ||
        nballs < params->minballs || nballs > params->maxballs)
        return "Game description is wrong length";

    bmp = hex2bin(desc, nballs*2 + 2);
    obfuscate_bitmap(bmp, (nballs*2 + 2) * 8, TRUE);
    ret = "Game description is corrupted";
    /* check general grid size */
    if (bmp[0] != params->w || bmp[1] != params->h)
        goto done;
    /* check each ball will fit on that grid */
    for (i = 0; i < nballs; i++) {
        int x = bmp[(i+1)*2 + 0], y = bmp[(i+1)*2 + 1];
        if (x < 0 || y < 0 || x > params->w || y > params->h)
            goto done;
    }
    ret = NULL;

done:
    sfree(bmp);
    return ret;
}

#define BALL_CORRECT    0x01
#define BALL_GUESS      0x02
#define BALL_LOCK       0x04

#define LASER_FLAGMASK  0xf800
#define LASER_OMITTED   0x0800
#define LASER_REFLECT   0x1000
#define LASER_HIT       0x2000
#define LASER_WRONG     0x4000
#define LASER_FLASHED   0x8000
#define LASER_EMPTY     (~0)

struct game_state {
    int w, h, minballs, maxballs, nballs, nlasers;
    unsigned int *grid; /* (w+2)x(h+2), to allow for laser firing range */
    unsigned int *exits; /* one per laser */
    int done;           /* user has finished placing his own balls. */
    int laserno;        /* number of next laser to be fired. */
    int nguesses, reveal, nright, nwrong, nmissed;
};

#define GRID(s,x,y) ((s)->grid[(y)*((s)->h+2) + (x)])

/* specify numbers because they must match array indexes. */
enum { DIR_UP = 0, DIR_RIGHT = 1, DIR_DOWN = 2, DIR_LEFT = 3 };

struct _off { int x, y; };

static const struct _off offsets[] = {
    {  0, -1 }, /* up */
    {  1,  0 }, /* right */
    {  0,  1 }, /* down */
    { -1,  0 }  /* left */
};

#ifdef DEBUGGING
static const char *dirstrs[] = {
    "UP", "RIGHT", "DOWN", "LEFT"
};
#endif

static int range2grid(game_state *state, int rangeno, int *x, int *y, int *direction)
{
    if (rangeno < 0)
        return 0;

    if (rangeno < state->w) {
        /* top row; from (1,0) to (w,0) */
        *x = rangeno + 1;
        *y = 0;
        *direction = DIR_DOWN;
        return 1;
    }
    rangeno -= state->w;
    if (rangeno < state->h) {
        /* RHS; from (w+1, 1) to (w+1, h) */
        *x = state->w+1;
        *y = rangeno + 1;
        *direction = DIR_LEFT;
        return 1;
    }
    rangeno -= state->h;
    if (rangeno < state->w) {
        /* bottom row; from (1, h+1) to (w, h+1); counts backwards */
        *x = (state->w - rangeno);
        *y = state->h+1;
        *direction = DIR_UP;
        return 1;
    }
    rangeno -= state->w;
    if (rangeno < state->h) {
        /* LHS; from (0, 1) to (0, h); counts backwards */
        *x = 0;
        *y = (state->h - rangeno);
        *direction = DIR_RIGHT;
        return 1;
    }
    return 0;
}

static int grid2range(game_state *state, int x, int y, int *rangeno)
{
    int ret, x1 = state->w+1, y1 = state->h+1;

    if (x > 0 && x < x1 && y > 0 && y < y1) return 0; /* in arena */
    if (x < 0 || x > y1 || y < 0 || y > y1) return 0; /* outside grid */

    if ((x == 0 || x == x1) && (y == 0 || y == y1))
        return 0; /* one of 4 corners */

    if (y == 0) {               /* top line */
        ret = x - 1;
    } else if (x == x1) {       /* RHS */
        ret = y - 1 + state->w;
    } else if (y == y1) {       /* Bottom [and counts backwards] */
        ret = (state->w - x) + state->w + state->h;
    } else {                    /* LHS [and counts backwards ] */
        ret = (state->h-y) + state->w + state->w + state->h;
    }
    *rangeno = ret;
    debug(("grid2range: (%d,%d) rangeno = %d\n", x, y, ret));
    return 1;
}

static game_state *new_game(midend_data *me, game_params *params, char *desc)
{
    game_state *state = snew(game_state);
    int dlen = strlen(desc), i;
    unsigned char *bmp;

    state->minballs = params->minballs;
    state->maxballs = params->maxballs;
    state->nballs = ((dlen/2)-2)/2;

    bmp = hex2bin(desc, state->nballs*2 + 2);
    obfuscate_bitmap(bmp, (state->nballs*2 + 2) * 8, TRUE);

    state->w = bmp[0]; state->h = bmp[1];
    state->nlasers = 2 * (state->w + state->h);

    state->grid = snewn((state->w+2)*(state->h+2), unsigned int);
    memset(state->grid, 0, (state->w+2)*(state->h+2) * sizeof(unsigned int));

    state->exits = snewn(state->nlasers, unsigned int);
    memset(state->exits, LASER_EMPTY, state->nlasers * sizeof(unsigned int));

    for (i = 0; i < state->nballs; i++) {
        GRID(state, bmp[(i+1)*2 + 0]+1, bmp[(i+1)*2 + 1]+1) = BALL_CORRECT;
    }
    sfree(bmp);

    state->done = state->nguesses = state->reveal =
        state->nright = state->nwrong = state->nmissed = 0;
    state->laserno = 1;

    return state;
}

#define XFER(x) ret->x = state->x

static game_state *dup_game(game_state *state)
{
    game_state *ret = snew(game_state);

    XFER(w); XFER(h);
    XFER(minballs); XFER(maxballs);
    XFER(nballs); XFER(nlasers);

    ret->grid = snewn((ret->w+2)*(ret->h+2), unsigned int);
    memcpy(ret->grid, state->grid, (ret->w+2)*(ret->h+2) * sizeof(unsigned int));
    ret->exits = snewn(ret->nlasers, unsigned int);
    memcpy(ret->exits, state->exits, ret->nlasers * sizeof(unsigned int));

    XFER(done);
    XFER(laserno);
    XFER(nguesses);
    XFER(reveal);
    XFER(nright); XFER(nwrong); XFER(nmissed);

    return ret;
}

#undef XFER

static void free_game(game_state *state)
{
    sfree(state->exits);
    sfree(state->grid);
    sfree(state);
}

static char *solve_game(game_state *state, game_state *currstate,
			char *aux, char **error)
{
    return dupstr("S");
}

static char *game_text_format(game_state *state)
{
    return NULL;
}

struct game_ui {
    int flash_laserno;
};

static game_ui *new_ui(game_state *state)
{
    game_ui *ui = snew(struct game_ui);
    ui->flash_laserno = LASER_EMPTY;
    return ui;
}

static void free_ui(game_ui *ui)
{
    sfree(ui);
}

static char *encode_ui(game_ui *ui)
{
    return NULL;
}

static void decode_ui(game_ui *ui, char *encoding)
{
}

static void game_changed_state(game_ui *ui, game_state *oldstate,
                               game_state *newstate)
{
}

#define OFFSET(gx,gy,o) do {                                    \
    int off = (o); while (off < 0) { off += 4; }  off %= 4;     \
    (gx) += offsets[off].x;                                     \
    (gy) += offsets[off].y;                                     \
} while(0)

enum { LOOK_LEFT, LOOK_FORWARD, LOOK_RIGHT };

/* Given a position and a direction, check whether we can see a ball in front
 * of us, or to our front-left or front-right. */
static int isball(game_state *state, int gx, int gy, int direction, int lookwhere)
{
    debug(("isball, (%d, %d), dir %s, lookwhere %s\n", gx, gy, dirstrs[direction],
           lookwhere == LOOK_LEFT ? "LEFT" :
           lookwhere == LOOK_FORWARD ? "FORWARD" : "RIGHT"));
    OFFSET(gx,gy,direction);
    if (lookwhere == LOOK_LEFT)
        OFFSET(gx,gy,direction-1);
    else if (lookwhere == LOOK_RIGHT)
        OFFSET(gx,gy,direction+1);
    else if (lookwhere != LOOK_FORWARD)
        assert(!"unknown lookwhere");

    debug(("isball, new (%d, %d)\n", gx, gy));

    /* if we're off the grid (into the firing range) there's never a ball. */
    if (gx < 1 || gy < 1 || gx > state->h || gy > state->w)
        return 0;

    if (GRID(state, gx,gy) & BALL_CORRECT)
        return 1;

    return 0;
}

static void fire_laser(game_state *state, int x, int y, int direction)
{
    int xstart = x, ystart = y, unused, lno;

    assert(grid2range(state, x, y, &lno));

    /* deal with strange initial reflection rules (that stop
     * you turning down the laser range) */

    /* I've just chosen to prioritise instant-hit over instant-reflection;
     * I can't find anywhere that gives me a definite algorithm for this. */
    if (isball(state, x, y, direction, LOOK_FORWARD)) {
        debug(("Instant hit at (%d, %d)\n", x, y));
        GRID(state, x, y) = LASER_HIT;
        state->exits[lno] = LASER_HIT;
        return;
    }

    if (isball(state, x, y, direction, LOOK_LEFT) ||
        isball(state, x, y, direction, LOOK_RIGHT)) {
        debug(("Instant reflection at (%d, %d)\n", x, y));
        GRID(state, x, y) = LASER_REFLECT;
        state->exits[lno] = LASER_REFLECT;
        return;
    }
    /* move us onto the grid. */
    OFFSET(x, y, direction);

    while (1) {
        debug(("fire_laser: looping at (%d, %d) pointing %s\n",
               x, y, dirstrs[direction]));
        if (grid2range(state, x, y, &unused)) {
            int newno = state->laserno++, exitno;
            debug(("Back on range; (%d, %d) --> (%d, %d)\n",
                   xstart, ystart, x, y));
            /* We're back out of the grid; the move is complete. */
            if (xstart == x && ystart == y) {
                GRID(state, x, y) = LASER_REFLECT;
                state->exits[lno] = LASER_REFLECT;
            } else {
                /* it wasn't a reflection */
                GRID(state, xstart, ystart) = newno;
                GRID(state, x, y) = newno;

                assert(grid2range(state, x, y, &exitno));
                state->exits[lno] = exitno;
                state->exits[exitno] = lno;
            }
            return;
        }
        /* paranoia. This obviously should never happen */
        assert(!(GRID(state, x, y) & BALL_CORRECT));

        if (isball(state, x, y, direction, LOOK_FORWARD)) {
            /* we're facing a ball; send back a reflection. */
            GRID(state, xstart, ystart) = LASER_HIT;
            state->exits[lno] = LASER_HIT;
            debug(("Ball ahead of (%d, %d); HIT at (%d, %d), new grid 0x%x\n",
                   x, y, xstart, ystart, GRID(state, xstart, ystart)));
            return;
        }

        if (isball(state, x, y, direction, LOOK_LEFT)) {
            /* ball to our left; rotate clockwise and look again. */
            debug(("Ball to left; turning clockwise.\n"));
            direction += 1; direction %= 4;
            continue;
        }
        if (isball(state, x, y, direction, LOOK_RIGHT)) {
            /* ball to our right; rotate anti-clockwise and look again. */
            debug(("Ball to rightl turning anti-clockwise.\n"));
            direction += 3; direction %= 4;
            continue;
        }
        /* ... otherwise, we have no balls ahead of us so just move one step. */
        debug(("No balls; moving forwards.\n"));
        OFFSET(x, y, direction);
    }
}

/* Checks that the guessed balls in the state match up with the real balls
 * for all possible lasers (i.e. not just the ones that the player might
 * have already guessed). This is required because any layout with >4 balls
 * might have multiple valid solutions. Returns non-zero for a 'correct'
 * (i.e. consistent) layout. */
static int check_guesses(game_state *state)
{
    game_state *solution, *guesses;
    int i, x, y, dir, unused;
    int ret = 0;

    /* duplicate the state (to solution) */
    solution = dup_game(state);

    /* clear out the lasers of solution */
    for (i = 0; i < solution->nlasers; i++) {
        assert(range2grid(solution, i, &x, &y, &unused));
        GRID(solution, x, y) = 0;
        solution->exits[i] = LASER_EMPTY;
    }

    /* duplicate solution to guess. */
    guesses = dup_game(solution);

    /* clear out BALL_CORRECT on guess, make BALL_GUESS BALL_CORRECT. */
    for (x = 1; x <= state->w; x++) {
        for (y = 1; y <= state->h; y++) {
            GRID(guesses, x, y) &= ~BALL_CORRECT;
            if (GRID(guesses, x, y) & BALL_GUESS)
                GRID(guesses, x, y) |= BALL_CORRECT;
        }
    }

    /* for each laser (on both game_states), fire it if it hasn't been fired.
     * If one has been fired (or received a hit) and another hasn't, we know
     * the ball layouts didn't match and can short-circuit return. */
    for (i = 0; i < solution->nlasers; i++) {
        assert(range2grid(solution, i, &x, &y, &dir));
        if (solution->exits[i] == LASER_EMPTY)
            fire_laser(solution, x, y, dir);
        if (guesses->exits[i] == LASER_EMPTY)
            fire_laser(guesses, x, y, dir);
    }

    /* check each game_state's laser against the other; if any differ, return 0 */
    ret = 1;
    for (i = 0; i < solution->nlasers; i++) {
        assert(range2grid(solution, i, &x, &y, &unused));

        if (solution->exits[i] != guesses->exits[i]) {
            /* If the original state didn't have this shot fired,
             * and it would be wrong between the guess and the solution,
             * add it. */
            if (state->exits[i] == LASER_EMPTY) {
                state->exits[i] = solution->exits[i];
                if (state->exits[i] == LASER_REFLECT ||
                    state->exits[i] == LASER_HIT)
                    GRID(state, x, y) = state->exits[i];
                else {
                    /* add a new shot, incrementing state's laser count. */
                    int ex, ey, newno = state->laserno++;
                    assert(range2grid(state, state->exits[i], &ex, &ey, &unused));
                    GRID(state, x, y) = newno;
                    GRID(state, ex, ey) = newno;
                }
		state->exits[i] |= LASER_OMITTED;
            } else {
		state->exits[i] |= LASER_WRONG;
	    }
            ret = 0;
        }
    }
    if (ret == 0) goto done;

    /* fix up original state so the 'correct' balls end up matching the guesses,
     * as we've just proved that they were equivalent. */
    for (x = 1; x <= state->w; x++) {
        for (y = 1; y <= state->h; y++) {
            if (GRID(state, x, y) & BALL_GUESS)
                GRID(state, x, y) |= BALL_CORRECT;
            else
                GRID(state, x, y) &= ~BALL_CORRECT;
        }
    }

done:
    /* fill in nright and nwrong. */
    state->nright = state->nwrong = state->nmissed = 0;
    for (x = 1; x <= state->w; x++) {
        for (y = 1; y <= state->h; y++) {
            int bs = GRID(state, x, y) & (BALL_GUESS | BALL_CORRECT);
            if (bs == (BALL_GUESS | BALL_CORRECT))
                state->nright++;
            else if (bs == BALL_GUESS)
                state->nwrong++;
            else if (bs == BALL_CORRECT)
                state->nmissed++;
        }
    }
    free_game(solution);
    free_game(guesses);
    return ret;
}

#define TILE_SIZE (ds->tilesize)

#define TODRAW(x) ((TILE_SIZE * (x)) + (TILE_SIZE / 2))
#define FROMDRAW(x) (((x) - (TILE_SIZE / 2)) / TILE_SIZE)

struct game_drawstate {
    int tilesize, crad, rrad, w, h; /* w and h to make macros work... */
    unsigned int *grid;          /* as the game_state grid */
    int started, canreveal, reveal;
    int flash_laserno;
};

static char *interpret_move(game_state *state, game_ui *ui, game_drawstate *ds,
			    int x, int y, int button)
{
    int gx = -1, gy = -1, rangeno = -1;
    enum { NONE, TOGGLE_BALL, TOGGLE_LOCK, FIRE, REVEAL,
           TOGGLE_COLUMN_LOCK, TOGGLE_ROW_LOCK} action = NONE;
    char buf[80], *nullret = NULL;

    if (button == LEFT_BUTTON || button == RIGHT_BUTTON) {
        gx = FROMDRAW(x);
        gy = FROMDRAW(y);
        if (gx == 0 && gy == 0 && button == LEFT_BUTTON)
            action = REVEAL;
        if (gx >= 1 && gx <= state->w && gy >= 1 && gy <= state->h) {
            if (button == LEFT_BUTTON) {
                if (!(GRID(state, gx,gy) & BALL_LOCK))
                    action = TOGGLE_BALL;
            } else
                action = TOGGLE_LOCK;
        }
        if (grid2range(state, gx, gy, &rangeno)) {
            if (button == LEFT_BUTTON)
                action = FIRE;
            else if (gy == 0 || gy > state->h)
                action = TOGGLE_COLUMN_LOCK; /* and use gx */
            else
                action = TOGGLE_ROW_LOCK;    /* and use gy */
        }
    } else if (button == LEFT_RELEASE) {
        ui->flash_laserno = LASER_EMPTY;
        return "";
    }

    switch (action) {
    case TOGGLE_BALL:
        sprintf(buf, "T%d,%d", gx, gy);
        break;

    case TOGGLE_LOCK:
        sprintf(buf, "LB%d,%d", gx, gy);
        break;

    case TOGGLE_COLUMN_LOCK:
        sprintf(buf, "LC%d", gx);
        break;

    case TOGGLE_ROW_LOCK:
        sprintf(buf, "LR%d", gy);
        break;

    case FIRE:
	if (state->reveal && state->exits[rangeno] == LASER_EMPTY)
	    return nullret;
        ui->flash_laserno = rangeno;
        nullret = "";
        if (state->exits[rangeno] != LASER_EMPTY)
            return "";
        sprintf(buf, "F%d", rangeno);
        break;

    case REVEAL:
        if (!ds->canreveal) return nullret;
        sprintf(buf, "R");
        break;

    default:
        return nullret;
    }
    if (state->reveal) return nullret;
    return dupstr(buf);
}

static game_state *execute_move(game_state *from, char *move)
{
    game_state *ret = dup_game(from);
    int gx = -1, gy = -1, rangeno = -1, direction;

    if (!strcmp(move, "S")) {
        ret->reveal = 1;
        return ret;
    }

    if (from->reveal) goto badmove;
    if (strlen(move) < 1) goto badmove;

    switch (move[0]) {
    case 'T':
        sscanf(move+1, "%d,%d", &gx, &gy);
        if (gx < 1 || gy < 1 || gx > ret->w || gy > ret->h)
            goto badmove;
        if (GRID(ret, gx, gy) & BALL_GUESS) {
            ret->nguesses--;
            GRID(ret, gx, gy) &= ~BALL_GUESS;
        } else {
            ret->nguesses++;
            GRID(ret, gx, gy) |= BALL_GUESS;
        }
        break;

    case 'F':
        sscanf(move+1, "%d", &rangeno);
        if (ret->exits[rangeno] != LASER_EMPTY)
            goto badmove;
        if (!range2grid(ret, rangeno, &gx, &gy, &direction))
            goto badmove;
        fire_laser(ret, gx, gy, direction);
        break;

    case 'R':
        if (ret->nguesses < ret->minballs ||
            ret->nguesses > ret->maxballs)
            goto badmove;
        check_guesses(ret);
        ret->reveal = 1;
        break;

    case 'L':
        {
            int lcount = 0;
            if (strlen(move) < 2) goto badmove;
            switch (move[1]) {
            case 'B':
                sscanf(move+2, "%d,%d", &gx, &gy);
                if (gx < 1 || gy < 1 || gx > ret->w || gy > ret->h)
                    goto badmove;
                GRID(ret, gx, gy) ^= BALL_LOCK;
                break;

#define COUNTLOCK do { if (GRID(ret, gx, gy) & BALL_LOCK) lcount++; } while (0)
#define SETLOCKIF(c) do {                                       \
    if (lcount > (c)) GRID(ret, gx, gy) &= ~BALL_LOCK;          \
    else              GRID(ret, gx, gy) |= BALL_LOCK;           \
} while(0)

            case 'C':
                sscanf(move+2, "%d", &gx);
                if (gx < 1 || gx > ret->w) goto badmove;
                for (gy = 1; gy <= ret->h; gy++) { COUNTLOCK; }
                for (gy = 1; gy <= ret->h; gy++) { SETLOCKIF(ret->h/2); }
                break;

            case 'R':
                sscanf(move+2, "%d", &gy);
                if (gy < 1 || gy > ret->h) goto badmove;
                for (gx = 1; gx <= ret->w; gx++) { COUNTLOCK; }
                for (gx = 1; gx <= ret->w; gx++) { SETLOCKIF(ret->w/2); }
                break;

#undef COUNTLOCK
#undef SETLOCKIF

            default:
                goto badmove;
            }
        }
        break;

    default:
        goto badmove;
    }

    return ret;

badmove:
    free_game(ret);
    return NULL;
}

/* ----------------------------------------------------------------------
 * Drawing routines.
 */

static void game_compute_size(game_params *params, int tilesize,
			      int *x, int *y)
{
    /* Border is ts/2, to make things easier.
     * Thus we have (width) + 2 (firing range*2) + 1 (border*2) tiles
     * across, and similarly height + 2 + 1 tiles down. */
    *x = (params->w + 3) * tilesize;
    *y = (params->h + 3) * tilesize;
}

static void game_set_size(game_drawstate *ds, game_params *params,
			  int tilesize)
{
    ds->tilesize = tilesize;
    ds->crad = (tilesize-1)/2;
    ds->rrad = (3*tilesize)/8;
}

static float *game_colours(frontend *fe, game_state *state, int *ncolours)
{
    float *ret = snewn(3 * NCOLOURS, float);
    int i;

    game_mkhighlight(fe, ret, COL_BACKGROUND, COL_HIGHLIGHT, COL_LOWLIGHT);

    ret[COL_BALL * 3 + 0] = 0.0F;
    ret[COL_BALL * 3 + 1] = 0.0F;
    ret[COL_BALL * 3 + 2] = 0.0F;

    ret[COL_WRONG * 3 + 0] = 1.0F;
    ret[COL_WRONG * 3 + 1] = 0.0F;
    ret[COL_WRONG * 3 + 2] = 0.0F;

    ret[COL_BUTTON * 3 + 0] = 0.0F;
    ret[COL_BUTTON * 3 + 1] = 1.0F;
    ret[COL_BUTTON * 3 + 2] = 0.0F;

    ret[COL_LASER * 3 + 0] = 1.0F;
    ret[COL_LASER * 3 + 1] = 0.0F;
    ret[COL_LASER * 3 + 2] = 0.0F;

    ret[COL_DIMLASER * 3 + 0] = 0.5F;
    ret[COL_DIMLASER * 3 + 1] = 0.0F;
    ret[COL_DIMLASER * 3 + 2] = 0.0F;

    for (i = 0; i < 3; i++) {
        ret[COL_GRID * 3 + i] = ret[COL_BACKGROUND * 3 + i] * 0.9F;
        ret[COL_LOCK * 3 + i] = ret[COL_BACKGROUND * 3 + i] * 0.7F;
        ret[COL_COVER * 3 + i] = ret[COL_BACKGROUND * 3 + i] * 0.5F;
        ret[COL_TEXT * 3 + i] = 0.0F;
    }

    ret[COL_FLASHTEXT * 3 + 0] = 0.0F;
    ret[COL_FLASHTEXT * 3 + 1] = 1.0F;
    ret[COL_FLASHTEXT * 3 + 2] = 0.0F;

    *ncolours = NCOLOURS;
    return ret;
}

static game_drawstate *game_new_drawstate(game_state *state)
{
    struct game_drawstate *ds = snew(struct game_drawstate);

    ds->tilesize = 0;
    ds->w = state->w; ds->h = state->h;
    ds->grid = snewn((state->w+2)*(state->h+2), unsigned int);
    memset(ds->grid, 0, (state->w+2)*(state->h+2)*sizeof(unsigned int));
    ds->started = 0;
    ds->flash_laserno = LASER_EMPTY;

    return ds;
}

static void game_free_drawstate(game_drawstate *ds)
{
    sfree(ds->grid);
    sfree(ds);
}

static void draw_arena_tile(frontend *fe, game_state *gs, game_drawstate *ds,
                            int ax, int ay, int force, int isflash)
{
    int gx = ax+1, gy = ay+1;
    int gs_tile = GRID(gs, gx, gy), ds_tile = GRID(ds, gx, gy);
    int dx = TODRAW(gx), dy = TODRAW(gy);

    if (gs_tile != ds_tile || gs->reveal != ds->reveal || force) {
        int bcol, bg;

        bg = (gs_tile & BALL_LOCK) ? COL_LOCK :
            gs->reveal ? COL_BACKGROUND : COL_COVER;

        draw_rect(fe, dx, dy, TILE_SIZE, TILE_SIZE, bg);
        draw_rect_outline(fe, dx, dy, TILE_SIZE, TILE_SIZE, COL_GRID);

        if (gs->reveal) {
            /* Guessed balls are always black; if they're incorrect they'll
             * have a red cross added later.
             * Missing balls are red. */
            if (gs_tile & BALL_GUESS) {
                bcol = isflash ? bg : COL_BALL;
            } else if (gs_tile & BALL_CORRECT) {
                bcol = isflash ? bg : COL_WRONG;
            } else {
                bcol = bg;
            }
        } else {
            /* guesses are black/black, all else background. */
            if (gs_tile & BALL_GUESS) {
                bcol = COL_BALL;
            } else {
                bcol = bg;
            }
        }

        draw_circle(fe, dx + TILE_SIZE/2, dy + TILE_SIZE/2, ds->crad-1,
                    bcol, bcol);

        if (gs->reveal &&
            (gs_tile & BALL_GUESS) &&
            !(gs_tile & BALL_CORRECT)) {
            int x1 = dx + 3, y1 = dy + 3;
            int x2 = dx + TILE_SIZE - 3, y2 = dy + TILE_SIZE-3;
	    int coords[8];

            /* Incorrect guess; draw a red cross over the ball. */
	    coords[0] = x1-1;
	    coords[1] = y1+1;
	    coords[2] = x1+1;
	    coords[3] = y1-1;
	    coords[4] = x2+1;
	    coords[5] = y2-1;
	    coords[6] = x2-1;
	    coords[7] = y2+1;
	    draw_polygon(fe, coords, 4, COL_WRONG, COL_WRONG);
	    coords[0] = x2+1;
	    coords[1] = y1+1;
	    coords[2] = x2-1;
	    coords[3] = y1-1;
	    coords[4] = x1-1;
	    coords[5] = y2-1;
	    coords[6] = x1+1;
	    coords[7] = y2+1;
	    draw_polygon(fe, coords, 4, COL_WRONG, COL_WRONG);
        }
        draw_update(fe, dx, dy, TILE_SIZE, TILE_SIZE);
    }
    GRID(ds,gx,gy) = gs_tile;
}

static void draw_laser_tile(frontend *fe, game_state *gs, game_drawstate *ds,
                            game_ui *ui, int lno, int force)
{
    int gx, gy, dx, dy, unused;
    int wrong, omitted, reflect, hit, laserval, flash = 0;
    unsigned int gs_tile, ds_tile, exitno;

    assert(range2grid(gs, lno, &gx, &gy, &unused));
    gs_tile = GRID(gs, gx, gy);
    ds_tile = GRID(ds, gx, gy);
    dx = TODRAW(gx);
    dy = TODRAW(gy);

    wrong = gs->exits[lno] & LASER_WRONG;
    omitted = gs->exits[lno] & LASER_OMITTED;
    exitno = gs->exits[lno] & ~LASER_FLAGMASK;

    reflect = gs_tile & LASER_REFLECT;
    hit = gs_tile & LASER_HIT;
    laserval = gs_tile & ~LASER_FLAGMASK;

    if (lno == ui->flash_laserno)
        gs_tile |= LASER_FLASHED;
    else if (!(gs->exits[lno] & (LASER_HIT | LASER_REFLECT))) {
        if (exitno == ui->flash_laserno)
            gs_tile |= LASER_FLASHED;
    }
    if (gs_tile & LASER_FLASHED) flash = 1;

    gs_tile |= wrong | omitted;

    if (gs_tile != ds_tile || force) {
        draw_rect(fe, dx, dy, TILE_SIZE, TILE_SIZE, COL_BACKGROUND);
        draw_rect_outline(fe, dx, dy, TILE_SIZE, TILE_SIZE, COL_GRID);

        if (gs_tile &~ (LASER_WRONG | LASER_OMITTED)) {
            char str[10];
            int tcol = flash ? COL_FLASHTEXT : omitted ? COL_WRONG : COL_TEXT;

            if (reflect || hit)
                sprintf(str, "%s", reflect ? "R" : "H");
            else
                sprintf(str, "%d", laserval);

            if (wrong) {
                draw_circle(fe, dx + TILE_SIZE/2, dy + TILE_SIZE/2,
                            ds->rrad,
                            COL_WRONG, COL_WRONG);
                draw_circle(fe, dx + TILE_SIZE/2, dy + TILE_SIZE/2,
                            ds->rrad - TILE_SIZE/16,
                            COL_BACKGROUND, COL_WRONG);
            }

            draw_text(fe, dx + TILE_SIZE/2, dy + TILE_SIZE/2,
                      FONT_VARIABLE, TILE_SIZE/2, ALIGN_VCENTRE | ALIGN_HCENTRE,
                      tcol, str);
        }
        draw_update(fe, dx, dy, TILE_SIZE, TILE_SIZE);
    }
    GRID(ds, gx, gy) = gs_tile;
}


static void game_redraw(frontend *fe, game_drawstate *ds, game_state *oldstate,
			game_state *state, int dir, game_ui *ui,
			float animtime, float flashtime)
{
    int i, x, y, ts = TILE_SIZE, isflash = 0, force = 0;

    if (flashtime > 0) {
        int frame = (int)(flashtime / FLASH_FRAME);
        isflash = (frame % 2) == 0;
        force = 1;
        debug(("game_redraw: flashtime = %f", flashtime));
    }

    if (!ds->started) {
        int x0 = TODRAW(0)-1, y0 = TODRAW(0)-1;
        int x1 = TODRAW(state->w+2), y1 = TODRAW(state->h+2);

        draw_rect(fe, 0, 0,
                  TILE_SIZE * (state->w+3), TILE_SIZE * (state->h+3),
                  COL_BACKGROUND);

        /* clockwise around the outline starting at pt behind (1,1). */
        draw_line(fe, x0+ts, y0+ts, x0+ts, y0,    COL_HIGHLIGHT);
        draw_line(fe, x0+ts, y0,    x1-ts, y0,    COL_HIGHLIGHT);
        draw_line(fe, x1-ts, y0,    x1-ts, y0+ts, COL_LOWLIGHT);
        draw_line(fe, x1-ts, y0+ts, x1,    y0+ts, COL_HIGHLIGHT);
        draw_line(fe, x1,    y0+ts, x1,    y1-ts, COL_LOWLIGHT);
        draw_line(fe, x1,    y1-ts, x1-ts, y1-ts, COL_LOWLIGHT);
        draw_line(fe, x1-ts, y1-ts, x1-ts, y1,    COL_LOWLIGHT);
        draw_line(fe, x1-ts, y1,    x0+ts, y1,    COL_LOWLIGHT);
        draw_line(fe, x0+ts, y1,    x0+ts, y1-ts, COL_HIGHLIGHT);
        draw_line(fe, x0+ts, y1-ts, x0,    y1-ts, COL_LOWLIGHT);
        draw_line(fe, x0,    y1-ts, x0,    y0+ts, COL_HIGHLIGHT);
        draw_line(fe, x0,    y0+ts, x0+ts, y0+ts, COL_HIGHLIGHT);
        /* phew... */

        draw_update(fe, 0, 0,
                    TILE_SIZE * (state->w+3), TILE_SIZE * (state->h+3));
        force = 1;
        ds->started = 1;
    }

    /* draw the arena */
    for (x = 0; x < state->w; x++) {
        for (y = 0; y < state->h; y++) {
            draw_arena_tile(fe, state, ds, x, y, force, isflash);
        }
    }

    /* draw the lasers */
    for (i = 0; i < 2*(state->w+state->h); i++) {
        draw_laser_tile(fe, state, ds, ui, i, force);
    }

    /* draw the 'finish' button */
    if (state->nguesses >= state->minballs &&
        state->nguesses <= state->maxballs &&
        !state->reveal) {
        clip(fe, TODRAW(0), TODRAW(0), TILE_SIZE-1, TILE_SIZE-1);
        draw_circle(fe, TODRAW(0) + ds->crad, TODRAW(0) + ds->crad, ds->crad,
                    COL_BUTTON, COL_BALL);
	unclip(fe);
        ds->canreveal = 1;
    } else {
        draw_rect(fe, TODRAW(0), TODRAW(0),
		  TILE_SIZE-1, TILE_SIZE-1, COL_BACKGROUND);
        ds->canreveal = 0;
    }
    draw_update(fe, TODRAW(0), TODRAW(0), TILE_SIZE, TILE_SIZE);
    ds->reveal = state->reveal;
    ds->flash_laserno = ui->flash_laserno;

    {
        char buf[256];

        if (ds->reveal) {
            if (state->nwrong == 0 &&
                state->nmissed == 0 &&
                state->nright >= state->minballs)
                sprintf(buf, "CORRECT!");
            else
                sprintf(buf, "%d wrong and %d missed balls.",
                        state->nwrong, state->nmissed);
        } else {
            if (state->nguesses > state->maxballs)
                sprintf(buf, "%d too many balls marked.",
                        state->nguesses - state->maxballs);
            else if (state->nguesses <= state->maxballs &&
                     state->nguesses >= state->minballs)
                sprintf(buf, "Click button to verify guesses.");
            else if (state->maxballs == state->minballs)
                sprintf(buf, "Balls marked: %d / %d",
                        state->nguesses, state->minballs);
            else
                sprintf(buf, "Balls marked: %d / %d-%d.",
                        state->nguesses, state->minballs, state->maxballs);
        }
        status_bar(fe, buf);
    }
}

static float game_anim_length(game_state *oldstate, game_state *newstate,
			      int dir, game_ui *ui)
{
    return 0.0F;
}

static float game_flash_length(game_state *oldstate, game_state *newstate,
			       int dir, game_ui *ui)
{
    if (!oldstate->reveal && newstate->reveal)
        return 4.0F * FLASH_FRAME;
    else
        return 0.0F;
}

static int game_wants_statusbar(void)
{
    return TRUE;
}

static int game_timing_state(game_state *state, game_ui *ui)
{
    return TRUE;
}

#ifdef COMBINED
#define thegame blackbox
#endif

const struct game thegame = {
    "Black Box", "games.blackbox",
    default_params,
    game_fetch_preset,
    decode_params,
    encode_params,
    free_params,
    dup_params,
    TRUE, game_configure, custom_params,
    validate_params,
    new_game_desc,
    validate_desc,
    new_game,
    dup_game,
    free_game,
    TRUE, solve_game,
    FALSE, game_text_format,
    new_ui,
    free_ui,
    encode_ui,
    decode_ui,
    game_changed_state,
    interpret_move,
    execute_move,
    PREFERRED_TILE_SIZE, game_compute_size, game_set_size,
    game_colours,
    game_new_drawstate,
    game_free_drawstate,
    game_redraw,
    game_anim_length,
    game_flash_length,
    game_wants_statusbar,
    FALSE, game_timing_state,
    0,				       /* mouse_priorities */
};

/* vim: set shiftwidth=4 tabstop=8: */