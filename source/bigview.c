#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include <3ds.h>
#include <citro3d.h>
#include <tex3ds.h>

#include "net.h"
#include "posts.h"
#include "imgtex.h"
#include "scaledec.h"
#include "bigview.h"
#include "stb_image.h"

#define BIG_MAX_W 400
#define BIG_MAX_H 240
#define BIG_RETRY_FRAMES 300 /* ~5s at 60fps */

static struct
{
    int post;      /* currently loaded, and which post it belongs to */
    int want;      /* what the cursor is asking for */
    int fetching;  /* post the in-flight transfer is actually for; want
                      can move underneath it when a request is coalesced */
    bool ready;
    bool failed;
    u32 retry_at;  /* frame stamp; failures are retried, not sticky */
    dl_t *dl;
    C3D_Tex tex;
    Tex3DS_SubTexture sub;
} s_big = { -1, -1, -1, false, false, 0, NULL, {}, {} };

static u32 s_frame = 1;
static char s_err[192] = "";
const char *g_big_err = s_err;

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
            q[0] = p[0]; q[1] = p[1]; q[2] = p[2]; q[3] = p[3];
        }
    }
    return true;
}

void bigview_init(void)
{
    /* stb_image handles PNG/JPEG; no turbojpeg init needed */
}

void bigview_exit(void)
{
    bigview_reset();
}

void bigview_reset(void)
{
    if (s_big.dl) {
        dl_abort(s_big.dl);
        s_big.dl = NULL;
    }
    if (s_big.ready)
        C3D_TexDelete(&s_big.tex);
    memset(&s_big, 0, sizeof(s_big));
    s_big.post = -1;
    s_big.want = -1;
    s_big.fetching = -1;
    s_err[0] = 0;
}

void bigview_abort(void)
{
    if (s_big.dl) {
        dl_abort(s_big.dl);
        s_big.dl = NULL;
        s_big.failed = false;
    }
}

void bigview_request(int post)
{
    if (s_big.want == post)
        return;
    if (s_big.dl) {
        s_big.want = post;
        s_big.failed = false;
        return;
    }
    if (s_big.ready) {
        C3D_TexDelete(&s_big.tex);
        s_big.ready = false;
    }
    s_big.failed = false;
    s_big.want = post;
}

bool bigview_get(int post, C3D_Tex **tex, const Tex3DS_SubTexture **sub)
{
    if (!s_big.ready || s_big.post != post)
        return false;
    *tex = &s_big.tex;
    *sub = &s_big.sub;
    return true;
}

static void build_url(int post, char *out, int outsz)
{
    Post *p = &posts[post];

    const char *cand = NULL;

    if (p->sample_ok && p->sample[0])
        cand = p->sample;
    else if (p->file[0])
        cand = p->file;
    else if (p->preview[0])
        cand = p->preview;
    else if (p->image[0])
        cand = p->image;

    if (!cand || !cand[0]) {
        out[0] = 0;
        return;
    }
    if (!strncmp(cand, "//", 2))
        snprintf(out, outsz, "https:%s", cand);
    else if (cand[0] == '/')
        snprintf(out, outsz, "https://%s%s", net_host(), cand);
    else
        snprintf(out, outsz, "%s", cand);
}

/* Decode any common booru image type; this is what fixes e621 support. */
static bool decode_big(u8 *data, u32 sz, C3D_Tex *tex, Tex3DS_SubTexture *sub)
{
    int w = 0, h = 0, ch = 0;
    unsigned char *rgba = stbi_load_from_memory(data, (int)sz, &w, &h, &ch, 4);
    if (!rgba)
        return false;

    int dw = 0, dh = 0;
    if (!scale_fit(w, h, BIG_MAX_W, BIG_MAX_H, &dw, &dh)) {
        stbi_image_free(rgba);
        return false;
    }

    u8 *resized = (u8 *)malloc((size_t)dw * dh * 4);
    if (!resized) {
        stbi_image_free(rgba);
        return false;
    }

    bool ok = resize_rgba_nearest(rgba, w, h, dw, dh, resized) &&
              imgtex_make(tex, sub, resized, dw, dh);

    free(resized);
    stbi_image_free(rgba);
    return ok;
}

bool bigview_busy(void)
{
    return s_big.dl != NULL;
}

bool bigview_failed(void)
{
    return s_big.failed;
}

u32 bigview_bytes(void)
{
    return s_big.dl ? dl_size(s_big.dl) : 0;
}

void bigview_pump(void)
{
    s_frame++;

    if (post_count <= 0 || s_big.want < 0 || s_big.want >= post_count)
        return;

    if (s_big.failed && s_big.want != s_big.post &&
        s_frame - s_big.retry_at >= BIG_RETRY_FRAMES) {
        s_big.failed = false;
        s_err[0] = 0;
    }

    if (!s_big.dl && !s_big.failed && s_big.want != s_big.post) {
        if (s_big.ready) {
            C3D_TexDelete(&s_big.tex);
            s_big.ready = false;
        }
        char url[512];
        build_url(s_big.want, url, sizeof(url));
        if (!url[0]) {
            s_big.failed = true;
            s_big.retry_at = s_frame;
            return;
        }
        s_big.dl = dl_start(url, false);
        if (!s_big.dl) {
            s_big.failed = true;
            s_big.retry_at = s_frame;
        } else {
            s_big.fetching = s_big.want;
        }
        return;
    }

    if (s_big.dl) {
        int r = dl_pump(s_big.dl);
        if (r == DL_DONE) {
            long code = dl_code(s_big.dl);
            u8 *buf = (u8 *)dl_buf(s_big.dl);
            u32 sz = dl_size(s_big.dl);
            bool ok = sz >= 16 && decode_big(buf, sz, &s_big.tex, &s_big.sub);
            if (ok) {
                s_err[0] = 0;
            } else {
                snprintf(s_err, sizeof(s_err), "%s (http %ld%s)",
                         dl_err(s_big.dl), code,
                         sz < 16 ? ", empty" : ", decode failed");
            }
            dl_abort(s_big.dl);
            s_big.dl = NULL;
            s_big.post = ok ? s_big.fetching : -1;
            s_big.ready = ok;
            s_big.failed = !ok;
            s_big.retry_at = s_frame;
        } else if (r == DL_ERR) {
            snprintf(s_err, sizeof(s_err), "%s", dl_err(s_big.dl));
            dl_abort(s_big.dl);
            s_big.dl = NULL;
            s_big.failed = true;
            s_big.retry_at = s_frame;
        }
    }
}
