/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_fixture_io.h"
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

/* Read tokens without recursion or a fixed token buffer. A separator or comment
 * is replaced by NUL after saving the next cursor. */
static char *next_token(char **cursor) {
    char *s = *cursor;
    for (;;) {
        while (*s == ',' || isspace((unsigned char)*s)) ++s;
        if (*s != '#') break;
        while (*s && *s != '\n' && *s != '\r') ++s;
    }
    if (!*s) { *cursor = s; return NULL; }
    char *token = s;
    while (*s && *s != ',' && *s != '#' && !isspace((unsigned char)*s)) ++s;
    if (*s == '#') {
        char *comment = s;
        while (*s && *s != '\n' && *s != '\r') ++s;
        *comment = '\0';
    } else if (*s) {
        *s++ = '\0';
    }
    *cursor = s;
    return token;
}

bool esy_fixture_read_u16_csv(const char *path, uint16_t *out,
    size_t max, size_t *out_count) {
    if (!path || !out || !out_count) return false;
    *out_count = 0u;
    FILE *fp = fopen(path, "rb");
    if (!fp) return false;
    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return false; }
    long length = ftell(fp);
    if (length < 0 || (uintmax_t)length >= SIZE_MAX ||
        fseek(fp, 0, SEEK_SET) != 0) { fclose(fp); return false; }
    char *buffer = malloc((size_t)length + 1u);
    if (!buffer) { fclose(fp); return false; }
    size_t count = fread(buffer, 1u, (size_t)length, fp);
    bool ok = !ferror(fp) && count == (size_t)length;
    fclose(fp);
    buffer[count] = '\0';
    char *cursor = buffer;
    char *token;
    while (ok && *out_count < max && (token = next_token(&cursor)) != NULL) {
        char *end;
        errno = 0;
        long value = strtol(token, &end, 10);
        if (end == token || *end != '\0') continue;
        if (value < 0) value = 0;
        if (value > UINT16_MAX) value = UINT16_MAX;
        out[(*out_count)++] = (uint16_t)value;
    }
    free(buffer);
    return ok;
}
