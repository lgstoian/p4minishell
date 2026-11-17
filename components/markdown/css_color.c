/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file css_color.c
 * @brief The one CSS colour parser (see css_color.h).
 */

#include "css_color.h"

#include "ansi.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool colour_component(const char **p, uint32_t *out)
{
    char *end = NULL;
    long value;

    while (**p == ' ' || **p == ',') {
        (*p)++;
    }
    value = strtol(*p, &end, 10);
    if (end == *p) {
        return false;
    }
    *p = end;
    if (**p == '%') {
        (*p)++;
        value = (value * 255) / 100;
    }
    if (value < 0) value = 0;
    if (value > 255) value = 255;
    *out = (uint32_t)value;
    return true;
}

bool css_color_parse(const char *text, css_rgba_t *out)
{
    char buf[64];
    size_t len;

    if (text == NULL || out == NULL) {
        return false;
    }
    while (*text == ' ' || *text == '\t') {
        text++;
    }
    len = strlen(text);
    while (len > 0 && (text[len - 1] == ' ' || text[len - 1] == '\t')) {
        len--;
    }
    if (len == 0 || len >= sizeof(buf)) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        char c = text[i];

        buf[i] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
    }
    buf[len] = '\0';

    if (strcmp(buf, "transparent") == 0) {
        *out = (css_rgba_t){ 0, 0, 0, 0 };
        return true;
    }
    if (buf[0] == '#') {
        size_t n = strlen(buf + 1);
        int v[8];
        int count = (n == 3 || n == 4) ? (int)n : (n == 6 || n == 8) ? (int)n : 0;

        if (count == 0) {
            return false;
        }
        for (int i = 0; i < count; i++) {
            v[i] = hex_digit(buf[1 + i]);
            if (v[i] < 0) {
                return false;
            }
        }
        if (count <= 4) {
            out->r = (uint8_t)(v[0] * 17);
            out->g = (uint8_t)(v[1] * 17);
            out->b = (uint8_t)(v[2] * 17);
            out->a = count == 4 ? (uint8_t)(v[3] * 17) : 255;
        } else {
            out->r = (uint8_t)(v[0] * 16 + v[1]);
            out->g = (uint8_t)(v[2] * 16 + v[3]);
            out->b = (uint8_t)(v[4] * 16 + v[5]);
            out->a = count == 8 ? (uint8_t)(v[6] * 16 + v[7]) : 255;
        }
        return true;
    }
    if (strncmp(buf, "rgb(", 4) == 0 || strncmp(buf, "rgba(", 5) == 0) {
        const char *p = strchr(buf, '(');
        uint32_t r = 0, g = 0, b = 0, a = 255;

        if (p == NULL) {
            return false;
        }
        p++;
        if (!colour_component(&p, &r) || !colour_component(&p, &g) ||
            !colour_component(&p, &b)) {
            return false;
        }
        if (strncmp(buf, "rgba(", 5) == 0) {
            char *end = NULL;
            double ca;

            while (*p == ' ' || *p == ',') p++;
            ca = strtod(p, &end);
            if (end != p) {
                if (*end == '%') {
                    ca /= 100.0;
                }
                if (ca < 0.0) ca = 0.0;
                if (ca > 1.0) ca = 1.0;
                a = (uint32_t)(ca * 255.0 + 0.5);
            }
        }
        out->r = (uint8_t)r;
        out->g = (uint8_t)g;
        out->b = (uint8_t)b;
        out->a = (uint8_t)a;
        return true;
    }
    {
        static const struct {
            const char *name;
            uint8_t r, g, b;
        } named[] = {
            { "black", 0, 0, 0 },       { "white", 255, 255, 255 },
            { "red", 255, 0, 0 },       { "green", 0, 128, 0 },
            { "blue", 0, 0, 255 },      { "gray", 128, 128, 128 },
            { "grey", 128, 128, 128 },  { "yellow", 255, 255, 0 },
            { "cyan", 0, 255, 255 },    { "aqua", 0, 255, 255 },
            { "magenta", 255, 0, 255 }, { "fuchsia", 255, 0, 255 },
            { "orange", 255, 165, 0 },  { "purple", 128, 0, 128 },
            { "navy", 0, 0, 128 },      { "teal", 0, 128, 128 },
            { "silver", 192, 192, 192 },{ "maroon", 128, 0, 0 },
            { "olive", 128, 128, 0 },   { "lime", 0, 255, 0 },
            { "brown", 165, 42, 42 },
        };

        for (size_t i = 0; i < sizeof(named) / sizeof(named[0]); i++) {
            if (strcmp(buf, named[i].name) == 0) {
                *out = (css_rgba_t){ named[i].r, named[i].g, named[i].b, 255 };
                return true;
            }
        }
    }
    return false;
}

uint8_t css_color_to_dos(const css_rgba_t *color)
{
    if (color == NULL || color->a == 0) {
        return 16;
    }
    return ansi_cga_nearest(((uint32_t)color->r << 16) |
                            ((uint32_t)color->g << 8) | color->b);
}

css_rgba_t css_color_from_dos(uint8_t dos)
{
    uint32_t rgb = ansi_cga_color(dos);
    css_rgba_t c;

    c.r = (uint8_t)((rgb >> 16) & 0xFF);
    c.g = (uint8_t)((rgb >> 8) & 0xFF);
    c.b = (uint8_t)(rgb & 0xFF);
    c.a = 255;
    return c;
}

css_rgba_t css_color_blend(css_rgba_t fg, css_rgba_t bg, uint8_t alpha)
{
    css_rgba_t out;

    out.r = (uint8_t)(((uint32_t)fg.r * alpha + (uint32_t)bg.r * (255u - alpha)) / 255u);
    out.g = (uint8_t)(((uint32_t)fg.g * alpha + (uint32_t)bg.g * (255u - alpha)) / 255u);
    out.b = (uint8_t)(((uint32_t)fg.b * alpha + (uint32_t)bg.b * (255u - alpha)) / 255u);
    out.a = 255;
    return out;
}
