/*
 * Source-Like Bhop  -  PSP homebrew (PSPDEV / psp-cmake)
 * Source-style movement (accel/airaccel/friction), bhop courses,
 * hitscan guns, and bots. Written from scratch - NOT Valve code.
 *
 * CONTROLS
 *  Analog stick : move / strafe
 *  D-pad        : look (L/R = turn, U/D = pitch)
 *  L or Cross   : jump (hold = autohop if enabled)
 *  R            : fire
 *  Triangle     : reload
 *  Square       : switch weapon
 *  Circle       : restart / respawn
 *  Select       : next map
 *  Start        : toggle autohop
 */
#include <pspkernel.h>
#include <pspdebug.h>
#include <pspdisplay.h>
#include <pspctrl.h>
#include <pspgu.h>
#include <pspgum.h>
#include <psprtc.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

PSP_MODULE_INFO("SourceLike", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);

#define BUF_W 512
#define SCR_W 480
#define SCR_H 272
#define FRAME_SIZE 0x88000

typedef unsigned int u32;
static unsigned int __attribute__((aligned(16))) list[0x20000];

/* ---- Source-ish movement constants (Source units) ---- */
#define ACCEL      10.0f
#define AIRACCEL   10.0f
#define FRICTION    4.0f
#define STOPSPEED 100.0f
#define MAXSPEED  250.0f
#define GRAVITY   800.0f
#define JUMPVEL   301.0f
#define AIRCAP     30.0f
#define STEPH      18.0f
#define HW         16.0f
#define PH         72.0f
#define EYE        64.0f

#define RGB(r,g,b) (0xFF000000u | ((u32)(b) << 16) | ((u32)(g) << 8) | (u32)(r))

typedef struct { float x, y, z; } Vec3;
typedef struct { u32 c; float x, y, z; } Vtx;
typedef struct { u32 c; short x, y, z; short pad; } Vtx2;
typedef struct { Vec3 mn, mx; u32 col; } Box;

/* ------------------------------------------------------------------ */
/* Exit callback                                                        */
/* ------------------------------------------------------------------ */
static int exit_cb(int a, int b, void *c) { sceKernelExitGame(); return 0; }
static int cb_thread(SceSize args, void *argp) {
    int id = sceKernelCreateCallback("exit", exit_cb, NULL);
    sceKernelRegisterExitCallback(id);
    sceKernelSleepThreadCB();
    return 0;
}
static void setup_callbacks(void) {
    int t = sceKernelCreateThread("cb", cb_thread, 0x11, 0xFA0, 0, 0);
    if (t >= 0) sceKernelStartThread(t, 0, 0);
}

/* ------------------------------------------------------------------ */
/* World                                                                */
/* ------------------------------------------------------------------ */
#define MAXBOX 96
static Box boxes[MAXBOX];
static int nbox = 0;

static void add_box(float cx, float y, float cz, float w, float h, float d, u32 col) {
    if (nbox >= MAXBOX) return;
    Box *b = &boxes[nbox++];
    b->mn = (Vec3){cx - w / 2, y, cz - d / 2};
    b->mx = (Vec3){cx + w / 2, y + h, cz + d / 2};
    b->col = col;
}

typedef struct { const char *name; int bots; int race; } MapInfo;
static MapInfo maps[3] = {
    {"bhop_easy", 0, 1},
    {"bhop_hard", 0, 1},
    {"aim_arena", 4, 0},
};
static int cur_map = 0;
static float fin_x0, fin_x1, fin_z0, fin_z1, fin_top;
static float best_time[3] = {0, 0, 0};

static void gen_course(int count, float w, float gap0, float gapstep, float zig) {
    add_box(0, -16, 0, 160, 16, 160, RGB(110, 110, 120));
    float cursor = -80;
    for (int i = 0; i < count; i++) {
        float gap = gap0 + i * gapstep;
        float zc = cursor - gap - w / 2;
        float top = (float)((i % 3) * 8);
        int m = i % 4;
        float x = (m == 1) ? zig : (m == 3) ? -zig : 0;
        u32 col = (i & 1) ? RGB(70, 140, 220) : RGB(220, 140, 70);
        add_box(x, -16, zc, w, 16 + top, w, col);
        cursor = zc - w / 2;
    }
    float zc = cursor - 60 - 80;
    add_box(0, -16, zc, 160, 16, 160, RGB(240, 200, 40));
    fin_x0 = -80; fin_x1 = 80; fin_z0 = zc - 80; fin_z1 = zc + 80; fin_top = 0;
}

static void gen_arena(void) {
    u32 wall = RGB(120, 100, 90), cov = RGB(150, 150, 160);
    add_box(0, -16, 0, 1600, 16, 1600, RGB(90, 95, 90));
    add_box(0, 0, -808, 1632, 220, 16, wall);
    add_box(0, 0, 808, 1632, 220, 16, wall);
    add_box(-808, 0, 0, 16, 220, 1600, wall);
    add_box(808, 0, 0, 16, 220, 1600, wall);
    add_box(-200, 0, -150, 96, 64, 96, cov);
    add_box(250, 0, -100, 128, 96, 32, cov);
    add_box(0, 0, 200, 32, 160, 32, cov);
    add_box(300, 0, 300, 96, 64, 96, cov);
    add_box(-250, 0, 300, 50, 16, 200, cov);
    add_box(-310, 0, 300, 70, 32, 200, cov);
    add_box(-450, 0, 300, 200, 48, 200, cov);
}

static Vec3 bot_spawns[4] = {{500, 0, 500}, {-500, 0, -500}, {500, 0, -500}, {0, 0, -600}};

/* ------------------------------------------------------------------ */
/* Math helpers                                                         */
/* ------------------------------------------------------------------ */
static float frand(void) { return (float)rand() / (float)RAND_MAX; }
static float srand1(void) { return frand() * 2.0f - 1.0f; }

static int slab(float o, float d, float mn, float mx, float *t0, float *t1) {
    if (fabsf(d) < 1e-8f) return (o >= mn && o <= mx);
    float a = (mn - o) / d, b = (mx - o) / d;
    if (a > b) { float t = a; a = b; b = t; }
    if (a > *t0) *t0 = a;
    if (b < *t1) *t1 = b;
    return *t0 <= *t1;
}
static float ray_aabb(Vec3 o, Vec3 d, Vec3 mn, Vec3 mx) {
    float t0 = 0, t1 = 1e9f;
    if (!slab(o.x, d.x, mn.x, mx.x, &t0, &t1)) return -1;
    if (!slab(o.y, d.y, mn.y, mx.y, &t0, &t1)) return -1;
    if (!slab(o.z, d.z, mn.z, mx.z, &t0, &t1)) return -1;
    return t0;
}
static float ray_world(Vec3 o, Vec3 d, float maxt) {
    float best = maxt;
    for (int i = 0; i < nbox; i++) {
        float t = ray_aabb(o, d, boxes[i].mn, boxes[i].mx);
        if (t >= 0 && t < best) best = t;
    }
    return best;
}

/* Axis-separated AABB movement with step-up. Returns grounded flag. */
static int move_body(Vec3 *p, Vec3 *v, float hw, float h, float dt, int grounded) {
    /* X */
    p->x += v->x * dt;
    for (int i = 0; i < nbox; i++) {
        Box *b = &boxes[i];
        if (p->x + hw > b->mn.x && p->x - hw < b->mx.x && p->z + hw > b->mn.z &&
            p->z - hw < b->mx.z && p->y + h > b->mn.y && p->y < b->mx.y) {
            float rise = b->mx.y - p->y;
            if (grounded && rise > 0 && rise <= STEPH) { p->y = b->mx.y; continue; }
            p->x = (v->x > 0) ? b->mn.x - hw : b->mx.x + hw;
            v->x = 0;
        }
    }
    /* Z */
    p->z += v->z * dt;
    for (int i = 0; i < nbox; i++) {
        Box *b = &boxes[i];
        if (p->x + hw > b->mn.x && p->x - hw < b->mx.x && p->z + hw > b->mn.z &&
            p->z - hw < b->mx.z && p->y + h > b->mn.y && p->y < b->mx.y) {
            float rise = b->mx.y - p->y;
            if (grounded && rise > 0 && rise <= STEPH) { p->y = b->mx.y; continue; }
            p->z = (v->z > 0) ? b->mn.z - hw : b->mx.z + hw;
            v->z = 0;
        }
    }
    /* Y */
    int ground = 0;
    p->y += v->y * dt;
    for (int i = 0; i < nbox; i++) {
        Box *b = &boxes[i];
        if (p->x + hw > b->mn.x && p->x - hw < b->mx.x && p->z + hw > b->mn.z &&
            p->z - hw < b->mx.z && p->y + h > b->mn.y && p->y < b->mx.y) {
            if (v->y <= 0) { p->y = b->mx.y; ground = 1; }
            else p->y = b->mn.y - h;
            v->y = 0;
        }
    }
    return ground;
}

static void accelerate(Vec3 *v, Vec3 wd, float wishspeed, float cap, float a, float dt) {
    float ws = wishspeed > cap ? cap : wishspeed;
    float cur = v->x * wd.x + v->z * wd.z;
    float add = ws - cur;
    if (add <= 0) return;
    float as = a * wishspeed * dt;
    if (as > add) as = add;
    v->x += as * wd.x;
    v->z += as * wd.z;
}

/* ------------------------------------------------------------------ */
/* Weapons / player / bots                                              */
/* ------------------------------------------------------------------ */
typedef struct {
    const char *name; float dmg, rate; int mag, reserve, pellets;
    float spread, reload; int autofire; float kick;
} Weapon;
static Weapon W[3] = {
    {"PISTOL",  24, 0.20f, 12, 48, 1, 0.004f, 1.4f, 0, 0.025f},
    {"RIFLE",   16, 0.09f, 30, 90, 1, 0.012f, 2.0f, 1, 0.015f},
    {"SHOTGUN",  9, 0.85f,  6, 24, 8, 0.060f, 2.0f, 0, 0.080f},
};

static struct {
    Vec3 p, v; float yaw, pitch; int ground; float hp;
    int wpn, mag[3], res[3]; float cd, reload; int reloading;
    float hit, hurt, kick, flash, msg;
    int kills, autohop;
    int race_state; float race_time;
} P;

#define MAXBOT 4
typedef struct { Vec3 p, v; float hp, cd, resp, strafe_t; int alive, dir; } Bot;
static Bot bots[MAXBOT];
static int nbots = 0;

static void spawn_player(void) {
    memset(&P.v, 0, sizeof(P.v));
    P.p = (Vec3){0, 0.1f, cur_map == 2 ? 650.0f : 0.0f};
    P.yaw = 0; P.pitch = 0; P.ground = 0; P.hp = 100;
    P.wpn = 0; P.cd = 0; P.reload = 0; P.reloading = 0;
    for (int i = 0; i < 3; i++) { P.mag[i] = W[i].mag; P.res[i] = W[i].reserve; }
    P.race_state = 0; P.race_time = 0;
}

static void bot_respawn(Bot *b) {
    b->p = bot_spawns[rand() % 4];
    b->p.y = 0.1f;
    b->v = (Vec3){0, 0, 0};
    b->hp = 100; b->alive = 1; b->cd = 1.0f; b->dir = 1; b->strafe_t = 1.0f;
}

static void load_map(int m) {
    cur_map = m;
    nbox = 0;
    if (m == 0) gen_course(24, 64, 40, 3.5f, 40);
    else if (m == 1) gen_course(30, 40, 70, 2.5f, 64);
    else gen_arena();
    nbots = maps[m].bots;
    for (int i = 0; i < nbots; i++) { bot_respawn(&bots[i]); bots[i].p = bot_spawns[i]; bots[i].p.y = 0.1f; }
    P.kills = 0;
    spawn_player();
}

static void player_damage(float d) {
    P.hp -= d;
    P.hurt = 0.35f;
    if (P.hp <= 0) { spawn_player(); P.msg = 1.5f; }
}

static Vec3 view_fwd(void) {
    float cp = cosf(P.pitch);
    return (Vec3){-sinf(P.yaw) * cp, sinf(P.pitch), -cosf(P.yaw) * cp};
}

static void fire_weapon(void) {
    Weapon *w = &W[P.wpn];
    P.mag[P.wpn]--;
    P.cd = w->rate;
    P.kick = 1.0f; P.flash = 0.05f;
    P.pitch += w->kick;

    Vec3 eye = {P.p.x, P.p.y + EYE, P.p.z};
    Vec3 f = view_fwd();
    Vec3 r = {cosf(P.yaw), 0, -sinf(P.yaw)};
    Vec3 u = {r.y * f.z - r.z * f.y, r.z * f.x - r.x * f.z, r.x * f.y - r.y * f.x};

    for (int k = 0; k < w->pellets; k++) {
        float sx = srand1() * w->spread, sy = srand1() * w->spread;
        Vec3 d = {f.x + r.x * sx + u.x * sy, f.y + r.y * sx + u.y * sy, f.z + r.z * sx + u.z * sy};
        float l = sqrtf(d.x * d.x + d.y * d.y + d.z * d.z);
        d.x /= l; d.y /= l; d.z /= l;

        float tw = ray_world(eye, d, 8000.0f);
        int hit = -1; int head = 0; float bt = tw;
        for (int i = 0; i < nbots; i++) {
            if (!bots[i].alive) continue;
            Vec3 bp = bots[i].p;
            float tb = ray_aabb(eye, d, (Vec3){bp.x - 16, bp.y, bp.z - 16}, (Vec3){bp.x + 16, bp.y + 58, bp.z + 16});
            float th = ray_aabb(eye, d, (Vec3){bp.x - 8, bp.y + 58, bp.z - 8}, (Vec3){bp.x + 8, bp.y + 72, bp.z + 8});
            if (tb >= 0 && tb < bt) { bt = tb; hit = i; head = 0; }
            if (th >= 0 && th < bt) { bt = th; hit = i; head = 1; }
        }
        if (hit >= 0) {
            bots[hit].hp -= w->dmg * (head ? 2.0f : 1.0f);
            P.hit = 0.15f;
            if (bots[hit].hp <= 0) {
                bots[hit].alive = 0; bots[hit].resp = 3.0f; P.kills++;
                P.res[P.wpn] += w->mag / 2;
            }
        }
    }
}

static void update_bots(float dt) {
    Vec3 pe = {P.p.x, P.p.y + EYE, P.p.z};
    for (int i = 0; i < nbots; i++) {
        Bot *b = &bots[i];
        if (!b->alive) { b->resp -= dt; if (b->resp <= 0) bot_respawn(b); continue; }
        float dx = P.p.x - b->p.x, dz = P.p.z - b->p.z;
        float dist = sqrtf(dx * dx + dz * dz) + 0.001f;
        float nx = dx / dist, nz = dz / dist;

        b->strafe_t -= dt;
        if (b->strafe_t <= 0) { b->dir = (rand() & 1) ? 1 : -1; b->strafe_t = 0.8f + frand(); }
        float fwd = dist > 350 ? 1.0f : (dist < 200 ? -1.0f : 0.0f);
        b->v.x = (nx * fwd + -nz * b->dir * 0.8f) * 130.0f;
        b->v.z = (nz * fwd + nx * b->dir * 0.8f) * 130.0f;
        b->v.y -= GRAVITY * dt;
        move_body(&b->p, &b->v, HW, PH, dt, 1);

        b->cd -= dt;
        if (b->cd <= 0 && dist < 1200) {
            Vec3 be = {b->p.x, b->p.y + EYE, b->p.z};
            Vec3 d = {pe.x - be.x, pe.y - be.y, pe.z - be.z};
            float l = sqrtf(d.x * d.x + d.y * d.y + d.z * d.z);
            d.x /= l; d.y /= l; d.z /= l;
            if (ray_world(be, d, l) >= l) {
                b->cd = 0.9f + frand() * 0.6f;
                float chance = 0.6f - dist / 2500.0f;
                if (frand() < chance) player_damage(8.0f + frand() * 6.0f);
            } else b->cd = 0.2f;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Rendering                                                            */
/* ------------------------------------------------------------------ */
static const int FI[6][4] = {{0,2,6,4},{1,3,7,5},{0,1,5,4},{2,3,7,6},{0,1,3,2},{4,5,7,6}};
static const float FS[6] = {0.75f, 0.75f, 0.5f, 1.0f, 0.85f, 0.65f};
static const int TI[6] = {0, 1, 2, 0, 2, 3};

static u32 shade(u32 c, float f) {
    u32 r = (u32)((c & 255) * f), g = (u32)(((c >> 8) & 255) * f), b = (u32)(((c >> 16) & 255) * f);
    return 0xFF000000u | (b << 16) | (g << 8) | r;
}
static int push_box(Vtx *v, Vec3 a, Vec3 b, u32 col) {
    float X[8], Y[8], Z[8];
    for (int i = 0; i < 8; i++) {
        X[i] = (i & 1) ? b.x : a.x;
        Y[i] = (i & 2) ? b.y : a.y;
        Z[i] = (i & 4) ? b.z : a.z;
    }
    int n = 0;
    for (int f = 0; f < 6; f++) {
        u32 c = shade(col, FS[f]);
        for (int k = 0; k < 6; k++) {
            int idx = FI[f][TI[k]];
            v[n].c = c; v[n].x = X[idx]; v[n].y = Y[idx]; v[n].z = Z[idx];
            n++;
        }
    }
    return n;
}

static void setup_gu(void) {
    sceGuInit();
    sceGuStart(GU_DIRECT, list);
    sceGuDrawBuffer(GU_PSM_8888, (void *)0, BUF_W);
    sceGuDispBuffer(SCR_W, SCR_H, (void *)FRAME_SIZE, BUF_W);
    sceGuDepthBuffer((void *)(FRAME_SIZE * 2), BUF_W);
    sceGuOffset(2048 - (SCR_W / 2), 2048 - (SCR_H / 2));
    sceGuViewport(2048, 2048, SCR_W, SCR_H);
    sceGuDepthRange(65535, 0);
    sceGuScissor(0, 0, SCR_W, SCR_H);
    sceGuEnable(GU_SCISSOR_TEST);
    sceGuDepthFunc(GU_GEQUAL);
    sceGuEnable(GU_DEPTH_TEST);
    sceGuDisable(GU_CULL_FACE);
    sceGuShadeModel(GU_SMOOTH);
    sceGuEnable(GU_CLIP_PLANES);
    sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
    sceGuFinish();
    sceGuSync(0, 0);
    sceDisplayWaitVblankStart();
    sceGuDisplay(GU_TRUE);
}

static int rect(Vtx2 *v, int n, int x0, int y0, int x1, int y1, u32 c) {
    v[n].c = c; v[n].x = x0; v[n].y = y0; v[n].z = 0;
    v[n + 1].c = c; v[n + 1].x = x1; v[n + 1].y = y1; v[n + 1].z = 0;
    return n + 2;
}

static void draw_frame(void) {
    sceGuStart(GU_DIRECT, list);
    sceGuClearColor(RGB(120, 170, 220));
    sceGuClearDepth(0);
    sceGuClear(GU_COLOR_BUFFER_BIT | GU_DEPTH_BUFFER_BIT);

    sceGumMatrixMode(GU_PROJECTION);
    sceGumLoadIdentity();
    sceGumPerspective(75.0f, 480.0f / 272.0f, 1.0f, 8000.0f);

    sceGumMatrixMode(GU_VIEW);
    sceGumLoadIdentity();
    sceGumRotateX(-P.pitch);
    sceGumRotateY(-P.yaw);
    ScePspFVector3 t = {-P.p.x, -(P.p.y + EYE), -P.p.z};
    sceGumTranslate(&t);

    sceGumMatrixMode(GU_MODEL);
    sceGumLoadIdentity();

    /* world + bots */
    int maxv = 36 * (nbox + 2 * nbots + 2);
    Vtx *v = (Vtx *)sceGuGetMemory(maxv * sizeof(Vtx));
    int n = 0;
    for (int i = 0; i < nbox; i++) n += push_box(v + n, boxes[i].mn, boxes[i].mx, boxes[i].col);
    for (int i = 0; i < nbots; i++) {
        if (!bots[i].alive) continue;
        Vec3 bp = bots[i].p;
        n += push_box(v + n, (Vec3){bp.x - 16, bp.y, bp.z - 16}, (Vec3){bp.x + 16, bp.y + 58, bp.z + 16}, RGB(200, 50, 50));
        n += push_box(v + n, (Vec3){bp.x - 8, bp.y + 58, bp.z - 8}, (Vec3){bp.x + 8, bp.y + 72, bp.z + 8}, RGB(230, 190, 150));
    }
    sceGumDrawArray(GU_TRIANGLES, GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_3D, n, 0, v);

    /* viewmodel */
    sceGumMatrixMode(GU_VIEW);
    sceGumLoadIdentity();
    sceGuClear(GU_DEPTH_BUFFER_BIT);
    Vtx *g = (Vtx *)sceGuGetMemory(72 * sizeof(Vtx));
    float k = P.kick * 3.0f;
    int gn = push_box(g, (Vec3){5, -10, -34 + k}, (Vec3){11, -4, -12 + k}, RGB(60, 60, 65));
    gn += push_box(g + gn, (Vec3){7, -4, -34 + k}, (Vec3){9, -2.5f, -14 + k}, RGB(90, 90, 95));
    sceGumDrawArray(GU_TRIANGLES, GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_3D, gn, 0, g);

    /* HUD */
    sceGuDisable(GU_DEPTH_TEST);
    sceGuEnable(GU_BLEND);
    Vtx2 *h = (Vtx2 *)sceGuGetMemory(32 * sizeof(Vtx2));
    int hn = 0;
    int cx = SCR_W / 2, cy = SCR_H / 2;
    u32 cc = P.hit > 0 ? RGB(255, 60, 60) : RGB(255, 255, 255);
    int gap = P.hit > 0 ? 6 : 4;
    hn = rect(h, hn, cx - gap - 6, cy - 1, cx - gap, cy + 1, cc);
    hn = rect(h, hn, cx + gap, cy - 1, cx + gap + 6, cy + 1, cc);
    hn = rect(h, hn, cx - 1, cy - gap - 6, cx + 1, cy - gap, cc);
    hn = rect(h, hn, cx - 1, cy + gap, cx + 1, cy + gap + 6, cc);
    int hpw = (int)(P.hp * 1.2f); if (hpw < 0) hpw = 0;
    hn = rect(h, hn, 10, SCR_H - 22, 10 + 120, SCR_H - 14, 0x80000000u);
    hn = rect(h, hn, 10, SCR_H - 22, 10 + hpw, SCR_H - 14, RGB(60, 220, 60));
    if (P.flash > 0) hn = rect(h, hn, cx - 20, SCR_H - 90, cx + 20, SCR_H - 60, 0xC0A0FFFFu);
    if (P.hurt > 0) {
        u32 a = (u32)(P.hurt / 0.35f * 120.0f);
        hn = rect(h, hn, 0, 0, SCR_W, SCR_H, (a << 24) | 0x000000FFu);
    }
    sceGuDrawArray(GU_SPRITES, GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D, hn, 0, h);
    sceGuDisable(GU_BLEND);
    sceGuEnable(GU_DEPTH_TEST);

    sceGuFinish();
    sceGuSync(0, 0);
}

/* ------------------------------------------------------------------ */
/* Main                                                                 */
/* ------------------------------------------------------------------ */
int main(void) {
    setup_callbacks();
    pspDebugScreenInit();
    pspDebugScreenEnableBackColor(0);
    setup_gu();

    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);

    u64 res = sceRtcGetTickResolution();
    u64 last; sceRtcGetCurrentTick(&last);
    unsigned int prev = 0;
    int draw_off = 0;

    srand((unsigned int)last);
    P.autohop = 1;
    load_map(0);

    while (1) {
        u64 now; sceRtcGetCurrentTick(&now);
        float dt = (float)(now - last) / (float)res;
        last = now;
        if (dt > 1.0f / 30.0f) dt = 1.0f / 30.0f;
        if (dt < 0.0005f) dt = 0.0005f;

        SceCtrlData pad;
        sceCtrlPeekBufferPositive(&pad, 1);
        unsigned int b = pad.Buttons;
        unsigned int pressed = b & ~prev;

        /* menu-ish buttons */
        if (pressed & PSP_CTRL_SELECT) load_map((cur_map + 1) % 3);
        if (pressed & PSP_CTRL_START) P.autohop = !P.autohop;
        if (pressed & PSP_CTRL_CIRCLE) spawn_player();
        if (pressed & PSP_CTRL_SQUARE) { P.wpn = (P.wpn + 1) % 3; P.reloading = 0; P.cd = 0.3f; }
        if ((pressed & PSP_CTRL_TRIANGLE) && !P.reloading &&
            P.mag[P.wpn] < W[P.wpn].mag && P.res[P.wpn] > 0) {
            P.reloading = 1; P.reload = W[P.wpn].reload;
        }

        /* look */
        if (b & PSP_CTRL_LEFT)  P.yaw += 2.2f * dt;
        if (b & PSP_CTRL_RIGHT) P.yaw -= 2.2f * dt;
        if (b & PSP_CTRL_UP)    P.pitch += 1.6f * dt;
        if (b & PSP_CTRL_DOWN)  P.pitch -= 1.6f * dt;
        if (P.pitch > 1.5f) P.pitch = 1.5f;
        if (P.pitch < -1.5f) P.pitch = -1.5f;

        /* wish direction */
        float fm = -((float)pad.Ly - 128.0f) / 128.0f;
        float sm = ((float)pad.Lx - 128.0f) / 128.0f;
        if (fabsf(fm) < 0.2f) fm = 0;
        if (fabsf(sm) < 0.2f) sm = 0;
        float sy = sinf(P.yaw), cy = cosf(P.yaw);
        Vec3 f = {-sy, 0, -cy}, r = {cy, 0, -sy};
        Vec3 wv = {f.x * fm + r.x * sm, 0, f.z * fm + r.z * sm};
        float wl = sqrtf(wv.x * wv.x + wv.z * wv.z);
        Vec3 wd = {0, 0, 0}; float ws = 0;
        if (wl > 0.001f) {
            wd.x = wv.x / wl; wd.z = wv.z / wl;
            ws = MAXSPEED * (wl > 1 ? 1 : wl);
        }

        /* jump */
        unsigned int jm = PSP_CTRL_LTRIGGER | PSP_CTRL_CROSS;
        int want = P.autohop ? ((b & jm) != 0) : (((b & jm) != 0) && !(prev & jm));
        if (want && P.ground) { P.v.y = JUMPVEL; P.ground = 0; }

        /* friction + accel (Source order) */
        if (P.ground) {
            float sp = sqrtf(P.v.x * P.v.x + P.v.z * P.v.z);
            if (sp > 0.1f) {
                float ctl = sp < STOPSPEED ? STOPSPEED : sp;
                float ns = sp - ctl * FRICTION * dt;
                if (ns < 0) ns = 0;
                P.v.x *= ns / sp; P.v.z *= ns / sp;
            }
            accelerate(&P.v, wd, ws, 1e9f, ACCEL, dt);
        } else {
            accelerate(&P.v, wd, ws, AIRCAP, AIRACCEL, dt);
        }
        P.v.y -= GRAVITY * dt;
        P.ground = move_body(&P.p, &P.v, HW, PH, dt, P.ground);

        /* fell off */
        if (P.p.y < -600) { spawn_player(); }

        /* race logic */
        if (maps[cur_map].race) {
            if (P.race_state == 0 && P.p.z < -82) P.race_state = 1;
            if (P.race_state == 1) {
                P.race_time += dt;
                if (P.ground && P.p.x > fin_x0 && P.p.x < fin_x1 && P.p.z > fin_z0 &&
                    P.p.z < fin_z1 && P.p.y >= fin_top - 1) {
                    P.race_state = 2;
                    if (best_time[cur_map] == 0 || P.race_time < best_time[cur_map])
                        best_time[cur_map] = P.race_time;
                }
            }
        }

        /* weapons */
        if (P.cd > 0) P.cd -= dt;
        if (P.hit > 0) P.hit -= dt;
        if (P.hurt > 0) P.hurt -= dt;
        if (P.flash > 0) P.flash -= dt;
        if (P.msg > 0) P.msg -= dt;
        if (P.kick > 0) { P.kick -= dt * 8.0f; if (P.kick < 0) P.kick = 0; }
        if (P.reloading) {
            P.reload -= dt;
            if (P.reload <= 0) {
                int need = W[P.wpn].mag - P.mag[P.wpn];
                int take = need < P.res[P.wpn] ? need : P.res[P.wpn];
                P.mag[P.wpn] += take; P.res[P.wpn] -= take;
                P.reloading = 0;
            }
        } else if (P.cd <= 0) {
            int trig = (b & PSP_CTRL_RTRIGGER) != 0;
            int trig_new = trig && !(prev & PSP_CTRL_RTRIGGER);
            if ((W[P.wpn].autofire ? trig : trig_new) || (trig && W[P.wpn].autofire)) {
                if (P.mag[P.wpn] > 0) fire_weapon();
                else if (P.res[P.wpn] > 0) { P.reloading = 1; P.reload = W[P.wpn].reload; }
            }
        }

        update_bots(dt);
        prev = b;

        /* render */
        draw_frame();
        pspDebugScreenSetOffset(draw_off);
        pspDebugScreenSetTextColor(0xFFFFFFFF);
        pspDebugScreenSetXY(1, 1);
        if (maps[cur_map].race) {
            pspDebugScreenPrintf("%s   TIME %.2f   BEST %.2f%s", maps[cur_map].name, P.race_time,
                                 best_time[cur_map], P.race_state == 2 ? "  FINISHED!" : "");
        } else {
            pspDebugScreenPrintf("%s   KILLS %d", maps[cur_map].name, P.kills);
        }
        pspDebugScreenSetXY(1, 3);
        pspDebugScreenPrintf("SPD %3d  AUTOHOP %s", (int)sqrtf(P.v.x * P.v.x + P.v.z * P.v.z),
                             P.autohop ? "ON" : "OFF");
        pspDebugScreenSetXY(1, 31);
        pspDebugScreenPrintf("HP %3d", (int)P.hp);
        pspDebugScreenSetXY(48, 31);
        pspDebugScreenPrintf("%s %2d/%3d%s", W[P.wpn].name, P.mag[P.wpn], P.res[P.wpn],
                             P.reloading ? " RELOAD" : "");
        if (P.msg > 0) { pspDebugScreenSetXY(28, 14); pspDebugScreenPrintf("YOU DIED"); }

        sceDisplayWaitVblankStart();
        sceGuSwapBuffers();
        draw_off = draw_off ? 0 : FRAME_SIZE;
    }
    return 0;
}
