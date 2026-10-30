#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "posts.h"

Post posts[MAX_POSTS];
int post_count = 0;

/* ------------------------------------------------------------------ */
/* minimal string-aware JSON scanner                                  */
/*                                                                     */
/* Both APIs return brace-delimited objects, so we do not need a full   */
/* parser - but we do need to know which braces live inside string      */
/* literals. Tag lists routinely contain '{' and '}', and cutting an    */
/* object short at the first one silently drops every field after it.   */
/* ------------------------------------------------------------------ */

static const char *skip_ws(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
        p++;
    return p;
}

/* Copy the string literal starting at p (which must point at '"') into
   out, unescaping the handful of escapes these APIs emit. Returns a
   pointer just past the closing quote. Pass out == NULL to skip. */
static const char *jstr(const char *p, char *out, int outsz)
{
    if (*p != '"') {
        if (out && outsz > 0)
            out[0] = 0;
        return p;
    }
    p++;
    int n = 0;
    while (*p && *p != '"') {
        unsigned char c = (unsigned char)*p;
        if (c == '\\' && p[1]) {
            p++;
            if (*p == 'n') c = '\n';
            else if (*p == 't') c = '\t';
            else if (*p == 'r') c = '\r';
            else c = (unsigned char)*p;
        }
        if (out && n < outsz - 1)
            out[n++] = (char)c;
        p++;
    }
    if (out && outsz > 0)
        out[n] = 0;
    if (*p == '"')
        p++;
    return p;
}

/* Advance past one JSON value of any type. */
static const char *jskip(const char *p)
{
    p = skip_ws(p);
    if (*p == '"')
        return jstr(p, NULL, 0);
    if (*p == '{' || *p == '[') {
        int depth = 0;
        while (*p) {
            if (*p == '"') {
                p = jstr(p, NULL, 0);
                continue;
            }
            if (*p == '{' || *p == '[')
                depth++;
            else if (*p == '}' || *p == ']') {
                depth--;
                if (depth == 0)
                    return p + 1;
            }
            p++;
        }
        return p;
    }
    while (*p && *p != ',' && *p != '}' && *p != ']' && *p != ' ')
        p++;
    return p;
}

/* Copy one JSON value into out as text, whether or not it is quoted.
   These APIs are not consistent: safebooru returns "directory":3921 as a
   bare number while konachan omits the field entirely. Accepting only
   quoted strings would leave the cursor parked on a number, and the next
   key check would then abort the whole object. */
static const char *jval(const char *p, char *out, int outsz)
{
    p = skip_ws(p);
    if (*p == '"')
        return jstr(p, out, outsz);
    if (!out || outsz <= 0)
        return jskip(p);
    int n = 0;
    while (*p && *p != ',' && *p != '}' && *p != ']' && *p != ' ' &&
           *p != '\t' && *p != '\n' && *p != '\r') {
        if (n < outsz - 1)
            out[n++] = *p;
        p++;
    }
    out[n] = 0;
    return p;
}

/* JSON boolean. Both providers send it bare, but accept a quoted form
   too rather than silently treating "true" as false. */
static const char *jbool(const char *p, bool *out)
{
    char tmp[8];
    p = jval(p, tmp, sizeof(tmp));
    *out = (tmp[0] == 't' || tmp[0] == 'T' || tmp[0] == '1');
    return p;
}

/* id field: quoted on some providers, bare on others */
static const char *juint(const char *p, unsigned int *out)
{
    char tmp[24];
    p = jval(p, tmp, sizeof(tmp));
    *out = (unsigned int)strtoul(tmp, NULL, 10);
    return p;
}

static const char *parse_url_object(const char *ob, char *out, int outsz, bool *has_value)
{
    if (out && outsz > 0)
        out[0] = 0;
    if (has_value)
        *has_value = true;
    if (*ob != '{')
        return ob;
    ob++;

    const char *p = ob;
    for (;;) {
        p = skip_ws(p);
        if (*p == '}')
            return p + 1;
        if (!*p)
            return p;
        if (*p == ',') {
            p++;
            continue;
        }
        if (*p != '"')
            return p;

        char key[48];
        p = jstr(p, key, sizeof(key));
        p = skip_ws(p);
        if (*p != ':')
            return p;
        p++;
        p = skip_ws(p);

        if (!strcmp(key, "url"))
            p = jval(p, out, outsz);
        else if (!strcmp(key, "has")) {
            bool has = true;
            p = jbool(p, &has);
            if (has_value)
                *has_value = has;
        } else {
            p = jskip(p);
        }
    }
}

/* Parse the object at ob into post. Returns a pointer past its closing
   brace, or ob itself if there is no object there. */
static const char *parse_object(const char *ob, Post *post)
{
    memset(post, 0, sizeof(*post));
    post->sample_ok = true; /* only an explicit false demotes it */
    if (*ob != '{')
        return ob;
    ob++;

    const char *p = ob;
    for (;;) {
        p = skip_ws(p);
        if (*p == '}')
            return p + 1;
        if (!*p)
            return p;
        if (*p == ',') {
            p++;
            continue;
        }
        if (*p != '"')
            return p; /* malformed: stop before we loop forever */

        char key[48];
        p = jstr(p, key, sizeof(key));
        p = skip_ws(p);
        if (*p != ':')
            return p;
        p++;          /* step over the ':' onto the value */
        p = skip_ws(p);

        if (!strcmp(key, "id")) p = juint(p, &post->id);
        else if (!strcmp(key, "sample")) {
            bool has = true;
            p = parse_url_object(p, post->sample, sizeof(post->sample), &has);
            post->sample_ok = has;
        } else if (!strcmp(key, "has_sample")) p = jbool(p, &post->sample_ok);
        else if (!strcmp(key, "directory")) p = jval(p, post->directory, sizeof(post->directory));
        else if (!strcmp(key, "image")) p = jval(p, post->image, sizeof(post->image));
        else if (!strcmp(key, "preview")) p = parse_url_object(p, post->preview, sizeof(post->preview), NULL);
        else if (!strcmp(key, "preview_url")) p = jval(p, post->preview, sizeof(post->preview));
        else if (!strcmp(key, "sample_url")) p = jval(p, post->sample, sizeof(post->sample));
        else if (!strcmp(key, "file")) p = parse_url_object(p, post->file, sizeof(post->file), NULL);
        else if (!strcmp(key, "file_url")) p = jval(p, post->file, sizeof(post->file));
        else if (!strcmp(key, "posts")) {
            /* e621 search replies wrap the array in a top-level object.
               Parse the post array in place and stop here; the array is
               handled by parse_posts() before we return to the caller. */
            p = jskip(p);
        } else p = jskip(p);
    }
}

static const char *parse_posts_array(const char *p, int *count)
{
    p = skip_ws(p);
    if (*p != '[')
        return p;
    p++;

    for (;;) {
        p = skip_ws(p);
        if (*p == ']')
            return p + 1;
        if (*p != '{') {
            p = jskip(p);
            continue;
        }

        Post tmp;
        const char *end = parse_object(p, &tmp);
        if (end <= p) {
            p++;
            continue;
        }

        if (tmp.file[0] || tmp.image[0] || tmp.id) {
            if (*count < MAX_POSTS)
                posts[(*count)++] = tmp;
            else
                break;
        }

        p = end;
        p = skip_ws(p);
        if (*p == ',') {
            p++;
            continue;
        }
        if (*p == ']')
            return p + 1;
    }

    return p;
}

int parse_posts(char *json)
{
    post_count = 0;
    const char *p = json;

    if (!p)
        return 0;

    /* e621 search responses are an envelope object with a "posts" array. */
    char *posts_key = strstr(json, "\"posts\"");
    if (posts_key) {
        const char *arr = posts_key + strlen("\"posts\"");
        while (*arr && *arr != '[')
            arr++;
        if (*arr == '[') {
            parse_posts_array(arr, &post_count);
            return post_count;
        }
    }

    /* Also accept bare JSON arrays or single post object responses. */
    p = skip_ws(p);
    if (*p == '[') {
        parse_posts_array(p, &post_count);
        return post_count;
    }

    if (*p == '{') {
        Post tmp;
        const char *end = parse_object(p, &tmp);
        if (end > p && (tmp.file[0] || tmp.image[0] || tmp.id)) {
            posts[post_count++] = tmp;
        }
    }

    return post_count;
}
