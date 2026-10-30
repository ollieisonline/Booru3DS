#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <limits.h>

#include <3ds.h>
#include <citro3d.h>
#include <tex3ds.h>

#include "app.h"
#include "net.h"
#include "posts.h"
#include "imgtex.h"
#include "scaledec.h"
#include "thumbs.h"
#include "stb_image.h"

#define THUMB_MAX_DIM  112
#define THUMB_MAX_EDGE 128
#define MAX_THUMBS     12
#define THUMB_CONCURRENT    1
#define THUMB_RETRY_FRAMES  300
#define THUMB_MAX_TRIES     3

typedef enum { T_EMPTY, T_QUEUE, T_ACTIVE, T_READY, T_FAIL } TState;

typedef struct
{
    int post;
    TState st;
    u32 stamp;
    u8 tries;
    C3D_Tex tex;
    Tex3DS_SubTexture sub;
    dl_t *dl;
} Slot;

static Slot s_slots[MAX_THUMBS];
static u32 s_frame = 1;
static int s_page = -1;

static bool is_jpeg_url(const char *s)
{
    if (!s || !s[0])
        return false;
    const char *dot = strrchr(s, '.');
    if (!dot)
        return false;
    const char *ext = dot + 1;
    if (!ext[0])
        return false;

    char a = ext[0];
    char b = ext[1];
    if (a >= 'A' && a <= 'Z') a += 'a' - 'A';
    if (b >= 'A' && b <= 'Z') b += 'a' - 'A';

    return (a == 'j' && b == 'p' && ext[2] == 'g' && ext[3] == 0) ||
           (a == 'j' && b == 'p' && ext[2] == 'e' && ext[3] == 'g' && ext[4] == 0);
}

static bool resize_rgba_nearest(const u8 *src, int sw, int sh,
                                int dw, int dh, u8 *dst)
{
    if (!src || sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0 || !dst)
        return false;

    for (int y = 0; y < dh; y++) {
        int sy = (y * sh) / dh;
        if (sy >= sh) sy = sh - 1;
        for (int x = 0; x < dw; x++) {
            int sx = (x * sw) / dw;
            if (sx >= sw) sx = sw - 1;
            const u8 *p = src + (sy * sw + sx) * 4;
            u8 *q = dst + (y * dw + x) * 4;
            q[0] = p[0];
            q[1] = p[1];
            q[2] = p[2];
            q[3] = p[3];
        }
    }
    return true;
}

void thumbs_init(void)
{
    /* stb_image handles PNG and JPEG */
}

void thumbs_exit(void)
{
    thumbs_reset();
}

void thumbs_reset(void)
{
    for (int i = 0; i < MAX_THUMBS; i++) {
        Slot *s = &s_slots[i];
        if (s->st == T_ACTIVE && s->dl)
            dl_abort(s->dl);
        if (s->st == T_READY)
            C3D_TexDelete(&s->tex);
        memset(s, 0, sizeof(*s));
    }
    s_page = -1;
}

bool thumb_get(int post, C3D_Tex **tex, const Tex3DS_SubTexture **sub)
{
    for (int i = 0; i < MAX_THUMBS; i++) {
        Slot *s = &s_slots[i];
        if (s->st == T_READY && s->post == post) {
            s->stamp = s_frame;
            *tex = &s->tex;
            *sub = &s->sub;
            return true;
        }
    }
    return false;
}

static const char *pick_image_candidate(const Post *p)
{
    if (p->sample_ok && p->sample[0] && is_jpeg_url(p->sample))
        return p->sample;
    if (p->file[0] && is_jpeg_url(p->file))
        return p->file;
    if (p->preview[0] && is_jpeg_url(p->preview))
        return p->preview;

    if (p->sample_ok && p->sample[0])
        return p->sample;
    if (p->file[0])
        return p->file;
    if (p->preview[0])
        return p->preview;
    if (p->image[0])
        return p->image;

    return NULL;
}

static void build_url(int post, char *out, int outsz)
{
    const Post *p = &posts[post];
    const char *cand = pick_image_candidate(p);

    if (!cand || !cand[0]) {
        snprintf(out, outsz,
                 "https://%s/thumbnails/%s/thumbnail_%s",
                 net_host(), p->directory, p->image);
        return;
    }

    if (!strncmp(cand, "//", 2))
        snprintf(out, outsz, "https:%s", cand);
    else if (cand[0] == '/')
        snprintf(out, outsz, "https://%s%s", net_host(), cand);
    else
        snprintf(out, outsz, "%s", cand);
}

static bool decode_thumb(u8 *data, u32 sz, C3D_Tex *tex, Tex3DS_SubTexture *sub)
{
    int w = 0, h = 0, ch = 0;
    unsigned char *rgba = stbi_load_from_memory(data, (int)sz, &w, &h, &ch, 4);
    if (!rgba)
        return false;

    int dw = 0, dh = 0;
    if (!scale_fit(w, h, THUMB_MAX_DIM, THUMB_MAX_DIM, &dw, &dh) ||
        dw > THUMB_MAX_EDGE || dh > THUMB_MAX_EDGE) {
        stbi_image_free(rgba);
        return false;
    }

    u8 *resized = (u8 *)malloc((size_t)dw * dh * 4);
    if (!resized) {
        stbi_image_free(rgba);
        return false;
    }

    bool ok = resize_rgba_nearest(rgba, w, h, dw, dh, resized) &&
              imgtex_make565(tex, sub, resized, dw, dh);

    free(resized);
    stbi_image_free(rgba);
    return ok;
}

static void free_slot(Slot *s)
{
    if (s->st == T_READY)
        C3D_TexDelete(&s->tex);
    else if (s->st == T_ACTIVE && s->dl)
        dl_abort(s->dl);
    memset(s, 0, sizeof(*s));
}

static Slot *alloc_slot(void)
{
    for (int i = 0; i < MAX_THUMBS; i++)
        if (s_slots[i].st == T_EMPTY || s_slots[i].st == T_FAIL)
            return &s_slots[i];

    Slot *victim = NULL;
    for (int i = 0; i < MAX_THUMBS; i++) {
        Slot *s = &s_slots[i];
        if (s->st == T_ACTIVE)
            continue;
        if (!victim || s->stamp < victim->stamp)
            victim = s;
    }
    if (victim)
        free_slot(victim);
    return victim;
}

static Slot *find_slot(int post)
{
    for (int i = 0; i < MAX_THUMBS; i++)
        if (s_slots[i].post == post && s_slots[i].st != T_EMPTY)
            return &s_slots[i];
    return NULL;
}

static int count_active(void)
{
    int n = 0;
    for (int i = 0; i < MAX_THUMBS; i++)
        if (s_slots[i].st == T_ACTIVE)
            n++;
    return n;
}

static Slot *pick_queued(int cursor)
{
    Slot *best = NULL;
    int best_dist = INT_MAX;
    for (int i = 0; i < MAX_THUMBS; i++) {
        Slot *s = &s_slots[i];
        if (s->st != T_QUEUE)
            continue;
        int dist = s->post - cursor;
        if (dist < 0) dist = -dist;
        if (dist < best_dist) {
            best_dist = dist;
            best = s;
        }
    }
    return best;
}

void thumbs_suspend(void)
{
    for (int i = 0; i < MAX_THUMBS; i++) {
        Slot *s = &s_slots[i];
        if (s->st == T_ACTIVE && s->dl) {
            dl_abort(s->dl);
            s->dl = NULL;
            s->st = T_QUEUE;
        }
    }
}

void thumbs_update(int cursor)
{
    s_frame++;

    if (post_count <= 0)
        return;

    int cur_page = (cursor / PAGE_SIZE) * PAGE_SIZE;
    int page_end = cur_page + PAGE_SIZE;
    if (page_end > post_count)
        page_end = post_count;

    if (cur_page != s_page) {
        for (int i = 0; i < MAX_THUMBS; i++) {
            Slot *s = &s_slots[i];
            if (s->st == T_ACTIVE && s->dl) {
                dl_abort(s->dl);
                s->dl = NULL;
                s->st = T_EMPTY;
            }
        }
        s_page = cur_page;
    }

    for (int i = 0; i < MAX_THUMBS; i++) {
        Slot *s = &s_slots[i];
        if (s->st == T_FAIL && s->tries < THUMB_MAX_TRIES &&
            s_frame - s->stamp > THUMB_RETRY_FRAMES)
            s->st = T_QUEUE;
    }

    for (int i = cur_page; i < page_end; i++) {
        if (find_slot(i))
            continue;
        Slot *s = alloc_slot();
        if (!s)
            break;
        memset(s, 0, sizeof(*s));
        s->post = i;
        s->st = T_QUEUE;
        s->stamp = s_frame;
    }

    while (count_active() < THUMB_CONCURRENT) {
        Slot *nextq = pick_queued(cursor);
        if (!nextq)
            break;
        char url[512];
        build_url(nextq->post, url, sizeof(url));
        nextq->dl = dl_start(url, false);
        nextq->st = nextq->dl ? T_ACTIVE : T_FAIL;
        if (!nextq->dl) {
            nextq->stamp = s_frame;
            nextq->tries++;
        }
    }

    bool decoded = false;
    for (int i = 0; i < MAX_THUMBS; i++) {
        Slot *s = &s_slots[i];
        if (s->st != T_ACTIVE || !s->dl)
            continue;

        int r = dl_pump(s->dl);
        if (r == DL_ERR) {
            dl_abort(s->dl);
            s->dl = NULL;
            s->st = T_FAIL;
            s->stamp = s_frame;
            s->tries++;
            continue;
        }

        if (r != DL_DONE || decoded)
            continue;

        decoded = true;
        u8 *buf = (u8 *)dl_buf(s->dl);
        u32 sz = dl_size(s->dl);

        bool ok = sz >= 16 && decode_thumb(buf, sz, &s->tex, &s->sub);

        dl_abort(s->dl);
        s->dl = NULL;
        s->st = ok ? T_READY : T_FAIL;

        if (!ok) {
            s->stamp = s_frame;
            s->tries++;
        }
    }
}
