/* wmem_strbuf.c
 * Wireshark Memory Manager String Buffer
 * Copyright 2012, Evan Huus <eapache@gmail.com>
 *
 * Wireshark - Network traffic analyzer
 * By Gerald Combs <gerald@wireshark.org>
 * Copyright 1998 Gerald Combs
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <glib.h>

#include "wmem_core.h"
#include "wmem_strbuf.h"

#define DEFAULT_MINIMUM_LEN 16

/* Holds a wmem-allocated string-buffer.
 *  len is the length of the string (not counting the null-terminator) and
 *      should be the same as strlen(str) unless the string contains embedded
 *      nulls.
 *  alloc_len is the length of the raw buffer pointed to by str, regardless of
 *      what string is actually being stored (i.e. the buffer contents)
 *  max_len is the maximum permitted alloc_len (NOT the maximum permitted len,
 *      which must be one shorter than alloc_len to permit null-termination).
 *      When max_len is 0 (the default), no maximum is enforced.
 */
struct _wmem_strbuf_t {
    wmem_allocator_t *allocator;

    gchar *str;

    gsize len;
    gsize alloc_len;
    gsize max_len;
};

/* _ROOM accounts for the null-terminator, _RAW_ROOM does not.
 * Some functions need one, some functions need the other. */
#define WMEM_STRBUF_ROOM(S) ((S)->alloc_len - (S)->len - 1)
#define WMEM_STRBUF_RAW_ROOM(S) ((S)->alloc_len - (S)->len)

wmem_strbuf_t *
wmem_strbuf_sized_new(wmem_allocator_t *allocator,
                      gsize alloc_len, gsize max_len)
{
    wmem_strbuf_t *strbuf;

    g_assert((max_len == 0) || (alloc_len <= max_len));

    strbuf = wmem_new(allocator, wmem_strbuf_t);

    strbuf->allocator = allocator;
    strbuf->len       = 0;
    strbuf->alloc_len = alloc_len ? alloc_len : DEFAULT_MINIMUM_LEN;
    strbuf->max_len   = max_len;

    strbuf->str    = (gchar *)wmem_alloc(strbuf->allocator, strbuf->alloc_len);
    strbuf->str[0] = '\0';

    return strbuf;
}

wmem_strbuf_t *
wmem_strbuf_new(wmem_allocator_t *allocator, const gchar *str)
{
    wmem_strbuf_t *strbuf;
    gsize          len, alloc_len;

    len       = str ? strlen(str) : 0;
    alloc_len = DEFAULT_MINIMUM_LEN;

    /* +1 for the null-terminator */
    while (alloc_len < (len + 1)) {
        alloc_len *= 2;
    }

    strbuf = wmem_strbuf_sized_new(allocator, alloc_len, 0);

    if (str && len > 0) {
        g_strlcpy(strbuf->str, str, alloc_len);
        strbuf->len = len;
    }

    return strbuf;
}

/* grows the allocated size of the wmem_strbuf_t. If max_len is set, then
 * not guaranteed to grow by the full amount to_add */
static inline void
wmem_strbuf_grow(wmem_strbuf_t *strbuf, const gsize to_add)
{
    gsize  new_alloc_len, new_len;

    /* short-circuit for efficiency if we have room already; greatly speeds up
     * repeated calls to wmem_strbuf_append_c and others which grow a little bit
     * at a time.
     */
    if (WMEM_STRBUF_ROOM(strbuf) >= to_add) {
        return;
    }

    new_alloc_len = strbuf->alloc_len;
    new_len = strbuf->len + to_add;

    /* +1 for the null-terminator */
    while (new_alloc_len < (new_len + 1)) {
        new_alloc_len *= 2;
    }

    /* max length only enforced if not 0 */
    if (strbuf->max_len && new_alloc_len > strbuf->max_len) {
        new_alloc_len = strbuf->max_len;
    }

    if (new_alloc_len == strbuf->alloc_len) {
        return;
    }

    strbuf->str = (gchar *)wmem_realloc(strbuf->allocator, strbuf->str, new_alloc_len);

    strbuf->alloc_len = new_alloc_len;
}

void
wmem_strbuf_append(wmem_strbuf_t *strbuf, const gchar *str)
{
    gsize append_len;

    if (!str || str[0] == '\0') {
        return;
    }

    append_len = strlen(str);

    wmem_strbuf_grow(strbuf, append_len);

    g_strlcpy(&strbuf->str[strbuf->len], str, strbuf->max_len ? WMEM_STRBUF_RAW_ROOM(strbuf) : append_len+1);

    strbuf->len = MIN(strbuf->len + append_len, strbuf->alloc_len - 1);
}

void
wmem_strbuf_append_len(wmem_strbuf_t *strbuf, const gchar *str, gsize append_len)
{

    if (!append_len || !str || str[0] == '\0') {
        return;
    }

    wmem_strbuf_grow(strbuf, append_len);

    if (strbuf->max_len) {
        append_len = MIN(append_len, WMEM_STRBUF_ROOM(strbuf));
    }

    memcpy(&strbuf->str[strbuf->len], str, append_len);
    strbuf->len += append_len;
    strbuf->str[strbuf->len] = '\0';
}

static inline
int _strbuf_vsnprintf(wmem_strbuf_t *strbuf, const char *format, va_list ap, gboolean reset)
{
    int want_len;
    char *buffer = &strbuf->str[strbuf->len];
    size_t buffer_size = WMEM_STRBUF_RAW_ROOM(strbuf);

    want_len = vsnprintf(buffer, buffer_size, format, ap);
    if (want_len < 0) {
        /* Error. */
        g_warning("%s: vsnprintf (%d): %s", G_STRFUNC, want_len, g_strerror(errno));
        return -1;
    }
    if ((size_t)want_len < buffer_size) {
        /* Success. */
        strbuf->len += want_len;
        return 0;
    }

    /* No space in buffer, output was truncated. */
    if (reset) {
        strbuf->str[strbuf->len] = '\0'; /* Reset. */
    }
    else {
        strbuf->len += buffer_size - 1; /* Append. */
        g_assert(strbuf->len == strbuf->alloc_len - 1);
    }

    return want_len; /* Length (not including terminating null) that would be written
                        if there was enough space in buffer. */
}

void
wmem_strbuf_append_vprintf(wmem_strbuf_t *strbuf, const gchar *fmt, va_list ap)
{
    int want_len;
    va_list ap2;

    G_VA_COPY(ap2, ap);
    /* Try to write buffer, check if output fits. */
    want_len = _strbuf_vsnprintf(strbuf, fmt, ap2, TRUE); /* Remove output if truncated. */
    va_end(ap2);
    if (want_len <= 0)
        return;

    /* Resize buffer and try again. This could hit the 'max_len' ceiling. */
    wmem_strbuf_grow(strbuf, want_len);
    _strbuf_vsnprintf(strbuf, fmt, ap, FALSE); /* Keep output if truncated. */
}

void
wmem_strbuf_append_printf(wmem_strbuf_t *strbuf, const gchar *format, ...)
{
    va_list ap;

    va_start(ap, format);
    wmem_strbuf_append_vprintf(strbuf, format, ap);
    va_end(ap);
}

void
wmem_strbuf_append_c(wmem_strbuf_t *strbuf, const gchar c)
{
    wmem_strbuf_grow(strbuf, 1);

    if (!strbuf->max_len || WMEM_STRBUF_ROOM(strbuf) >= 1) {
        strbuf->str[strbuf->len] = c;
        strbuf->len++;
        strbuf->str[strbuf->len] = '\0';
    }
}

void
wmem_strbuf_append_unichar(wmem_strbuf_t *strbuf, const gunichar c)
{
    gchar buf[6];
    gsize charlen;

    charlen = g_unichar_to_utf8(c, buf);

    wmem_strbuf_grow(strbuf, charlen);

    if (!strbuf->max_len || WMEM_STRBUF_ROOM(strbuf) >= charlen) {
        memcpy(&strbuf->str[strbuf->len], buf, charlen);
        strbuf->len += charlen;
        strbuf->str[strbuf->len] = '\0';
    }
}

void
wmem_strbuf_truncate(wmem_strbuf_t *strbuf, const gsize len)
{
    if (len >= strbuf->len) {
        return;
    }

    strbuf->str[len] = '\0';
    strbuf->len = len;
}

const gchar *
wmem_strbuf_get_str(wmem_strbuf_t *strbuf)
{
    return strbuf->str;
}

gsize
wmem_strbuf_get_len(wmem_strbuf_t *strbuf)
{
    return strbuf->len;
}

/* Truncates the allocated memory down to the minimal amount, frees the header
 * structure, and returns a non-const pointer to the raw string. The
 * wmem_strbuf_t structure cannot be used after this is called.
 */
char *
wmem_strbuf_finalize(wmem_strbuf_t *strbuf)
{
    char *ret;

    ret = (char *)wmem_realloc(strbuf->allocator, strbuf->str, strbuf->len+1);

    wmem_free(strbuf->allocator, strbuf);

    return ret;
}

/*
 * Editor modelines  -  https://www.wireshark.org/tools/modelines.html
 *
 * Local variables:
 * c-basic-offset: 4
 * tab-width: 8
 * indent-tabs-mode: nil
 * End:
 *
 * vi: set shiftwidth=4 tabstop=8 expandtab:
 * :indentSize=4:tabSize=8:noTabs=true:
 */
