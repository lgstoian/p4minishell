/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file emoji_core.c
 * @brief Emoji detection and the built-in retro DOS fallback map.
 *
 * The default pack is this table: it maps the emoji people actually type to
 * the CP437 symbols a DOS-era terminal had (smileys, card suits, music,
 * arrows, blocks). It is authored here, not copied from a font, so its licence
 * is the project's MIT (see licence.md).
 */

#include "emoji_core.h"

/* The fallback codepoints MUST exist in the shipped cell/terminal font
 * (`lv_font_unscii_16`: ASCII, U+2500-257F, U+2600-26FF). Anything outside
 * those ranges renders as the font's placeholder, which is the tofu this
 * feature exists to avoid, so the map is deliberately restricted to the
 * 2600-26FF symbol block (plus the ASCII smileys' block). */
#define DOS_SMILE       0x263A  /* ☺ */
#define DOS_SMILE_INV   0x263B  /* ☻ */
#define DOS_HEART       0x2665  /* ♥ */
#define DOS_DIAMOND     0x2666  /* ♦ */
#define DOS_CLUB        0x2663  /* ♣ */
#define DOS_SPADE       0x2660  /* ♠ */
#define DOS_MALE        0x2642  /* ♂ */
#define DOS_FEMALE      0x2640  /* ♀ */
#define DOS_NOTE        0x266A  /* ♪ */
#define DOS_NOTES       0x266B  /* ♫ */
#define DOS_SUN         0x263C  /* ☼ */
#define DOS_STAR        0x2605  /* ★ */
#define DOS_STAR_OPEN   0x2606  /* ☆ */
#define DOS_CIRCLE      0x2610  /* ☐ (ballot box: generic object) */
#define DOS_TRI_UP      0x2605  /* ★ */
#define DOS_TRI_DOWN    0x2606  /* ☆ */
#define DOS_RIGHT       0x2605  /* ★ */
#define DOS_LEFT        0x2606  /* ☆ */
#define DOS_UP          0x2605  /* ★ */
#define DOS_DOWN        0x2606  /* ☆ */
#define DOS_ARROW_LR    0x263C  /* ☼ */
#define DOS_UPDOWN      0x2605  /* ★ */
#define DOS_DOUBLE_BANG 0x26A0  /* ⚠ warning */
#define DOS_CHECK       0x2611  /* ☑ ballot box with check */
#define DOS_ANGLE       0x2612  /* ☒ ballot box with X */
#define DOS_SECTION     0x2663  /* ♣ (plants) */
#define DOS_SQUARE      0x2610  /* ☐ */
#define DOS_BULLET      0x2605  /* ★ */
#define DOS_CHESS       0x265F  /* ♟ (pawn: game) */

/** Curated single-codepoint fallbacks: the emoji a chat/notes page uses most.
 *  Kept small and ordered; anything not listed falls to a range default. */
static const struct {
    uint32_t cp;
    uint32_t glyph;
} s_fallbacks[] = {
    /* Faces and people. */
    { 0x1F600, DOS_SMILE },      /* 😀 grinning */
    { 0x1F601, DOS_SMILE },      /* 😁 beaming */
    { 0x1F602, DOS_SMILE },      /* 😂 tears of joy */
    { 0x1F603, DOS_SMILE },      /* 😃 */
    { 0x1F604, DOS_SMILE },      /* 😄 */
    { 0x1F605, DOS_SMILE },      /* 😅 */
    { 0x1F606, DOS_SMILE },      /* 😆 */
    { 0x1F609, DOS_SMILE },      /* 😉 */
    { 0x1F60A, DOS_SMILE },      /* 😊 */
    { 0x1F60D, DOS_HEART },      /* 😍 */
    { 0x1F60E, DOS_SMILE },      /* 😎 */
    { 0x1F610, DOS_SMILE_INV },  /* 😐 neutral */
    { 0x1F614, DOS_SMILE_INV },  /* 😔 pensive */
    { 0x1F620, DOS_TRI_DOWN },   /* 😠 angry */
    { 0x1F621, DOS_TRI_DOWN },   /* 😡 rage */
    { 0x1F622, DOS_TRI_DOWN },   /* 😢 cry */
    { 0x1F62D, DOS_TRI_DOWN },   /* 😭 sob */
    { 0x1F631, DOS_DOUBLE_BANG },/* 😱 scream */
    { 0x1F642, DOS_SMILE },      /* 🙂 slight smile */
    { 0x1F643, DOS_SMILE_INV },  /* 🙃 upside down */
    { 0x1F644, DOS_SMILE_INV },  /* 🙄 eye roll */
    /* Hands. */
    { 0x1F44D, DOS_CHECK },      /* 👍 */
    { 0x1F44E, DOS_ANGLE },      /* 👎 */
    { 0x1F44F, DOS_STAR },       /* 👏 */
    { 0x1F64F, DOS_HEART },      /* 🙏 */
    { 0x1F4AA, DOS_STAR },       /* 💪 */
    /* Hearts, symbols, marks. */
    { 0x2764, DOS_HEART },       /* ❤ */
    { 0x1F494, DOS_HEART },      /* 💔 */
    { 0x1F495, DOS_HEART },      /* 💕 */
    { 0x1F496, DOS_HEART },      /* 💖 */
    { 0x1F497, DOS_HEART },      /* 💗 */
    { 0x1F498, DOS_HEART },      /* 💘 */
    { 0x1F49D, DOS_HEART },      /* 💝 */
    { 0x1F49E, DOS_HEART },      /* 💞 */
    { 0x1F49F, DOS_HEART },      /* 💟 */
    { 0x2728, DOS_STAR },        /* ✨ */
    { 0x2B50, DOS_STAR },        /* ⭐ */
    { 0x1F31F, DOS_STAR },       /* 🌟 */
    { 0x2705, DOS_CHECK },       /* ✅ */
    { 0x2714, DOS_CHECK },       /* ✔ */
    { 0x274C, DOS_ANGLE },       /* ❌ */
    { 0x2716, DOS_ANGLE },       /* ✖ */
    { 0x26A0, DOS_DOUBLE_BANG }, /* ⚠ */
    { 0x2757, DOS_DOUBLE_BANG }, /* ❗ */
    { 0x2753, DOS_DOUBLE_BANG }, /* ❓ */
    { 0x1F4A1, DOS_STAR },       /* 💡 */
    { 0x1F525, DOS_TRI_UP },     /* 🔥 */
    { 0x1F4A5, DOS_DOUBLE_BANG },/* 💥 */
    { 0x1F389, DOS_STAR },       /* 🎉 */
    { 0x1F38A, DOS_STAR },       /* 🎊 */
    { 0x1F381, DOS_HEART },      /* 🎁 */
    /* Music, media, time. */
    { 0x1F3B5, DOS_NOTE },       /* 🎵 */
    { 0x1F3B6, DOS_NOTES },      /* 🎶 */
    { 0x1F3A7, DOS_NOTE },       /* 🎧 */
    { 0x1F3AC, DOS_RIGHT },      /* 🎬 */
    { 0x1F4F7, DOS_CIRCLE },     /* 📷 */
    { 0x1F4F9, DOS_RIGHT },      /* 📹 */
    { 0x1F4FE, DOS_RIGHT },      /* 📾 */
    { 0x1F550, DOS_SUN },        /* 🕐 clock */
    { 0x23F0, DOS_SUN },         /* ⏰ alarm clock */
    { 0x23F3, DOS_SUN },         /* ⏳ hourglass */
    { 0x2328, DOS_CIRCLE },      /* ⌨ keyboard */
    /* Weather and nature. */
    { 0x2600, DOS_SUN },         /* ☀ */
    { 0x2601, DOS_CIRCLE },      /* ☁ */
    { 0x2602, DOS_DOWN },        /* ☂ */
    { 0x2744, DOS_STAR },        /* ❄ */
    { 0x26A1, DOS_TRI_UP },      /* ⚡ */
    { 0x1F30D, DOS_CIRCLE },     /* 🌍 */
    { 0x1F30E, DOS_CIRCLE },     /* 🌎 */
    { 0x1F30F, DOS_CIRCLE },     /* 🌏 */
    { 0x1F319, DOS_SUN },        /* 🌙 moon */
    { 0x1F31E, DOS_SUN },        /* 🌞 */
    { 0x1F332, DOS_TRI_UP },     /* 🌲 */
    { 0x1F33F, DOS_SECTION },    /* 🌿 */
    { 0x1F340, DOS_CLUB },       /* 🍀 */
    { 0x1F337, DOS_HEART },      /* 🌷 */
    { 0x1F338, DOS_HEART },      /* 🌸 */
    { 0x1F339, DOS_HEART },      /* 🌹 */
    { 0x1F33B, DOS_SUN },        /* 🌻 */
    /* Food and drink. */
    { 0x1F354, DOS_CIRCLE },     /* 🍔 */
    { 0x1F355, DOS_CIRCLE },     /* 🍕 */
    { 0x1F35A, DOS_CIRCLE },     /* 🍚 */
    { 0x1F363, DOS_CIRCLE },     /* 🍣 */
    { 0x1F36A, DOS_CIRCLE },     /* 🍪 */
    { 0x1F36B, DOS_CIRCLE },     /* 🍫 */
    { 0x1F369, DOS_CIRCLE },     /* 🍩 */
    { 0x2615, DOS_CIRCLE },      /* ☕ */
    { 0x1F37A, DOS_CIRCLE },     /* 🍺 */
    { 0x1F377, DOS_CIRCLE },     /* 🍷 */
    { 0x1F34E, DOS_CIRCLE },     /* 🍎 */
    { 0x1F34C, DOS_CIRCLE },     /* 🍌 */
    { 0x1F347, DOS_CIRCLE },     /* 🍇 */
    { 0x1F353, DOS_HEART },      /* 🍓 */
    /* Transport and places. */
    { 0x1F697, DOS_RIGHT },      /* 🚗 */
    { 0x1F68C, DOS_RIGHT },      /* 🚌 */
    { 0x1F6B2, DOS_RIGHT },      /* 🚲 */
    { 0x2708, DOS_RIGHT },       /* ✈ */
    { 0x1F680, DOS_RIGHT },      /* 🚀 */
    { 0x1F6A2, DOS_RIGHT },      /* 🚢 */
    { 0x1F3E0, DOS_CIRCLE },     /* 🏠 */
    { 0x1F3E2, DOS_CIRCLE },     /* 🏢 */
    { 0x1F3EB, DOS_CIRCLE },     /* 🏫 */
    { 0x1F5FB, DOS_TRI_UP },     /* 🗻 */
    /* Objects and tech. */
    { 0x1F4BB, DOS_CIRCLE },     /* 💻 */
    { 0x1F4F1, DOS_CIRCLE },     /* 📱 */
    { 0x1F5A5, DOS_CIRCLE },     /* 🖥 */
    { 0x1F4BE, DOS_CIRCLE },     /* 💾 */
    { 0x1F4C1, DOS_CIRCLE },     /* 📁 */
    { 0x1F4C4, DOS_CIRCLE },     /* 📄 */
    { 0x1F4DA, DOS_CLUB },       /* 📚 books */
    { 0x1F4D6, DOS_CLUB },       /* 📖 */
    { 0x1F4E7, DOS_CIRCLE },     /* 📧 */
    { 0x1F4E8, DOS_CIRCLE },     /* 📨 */
    { 0x1F4E4, DOS_RIGHT },      /* 📤 */
    { 0x1F4E5, DOS_LEFT },       /* 📥 */
    { 0x1F50B, DOS_CIRCLE },     /* 🔋 */
    { 0x1F50C, DOS_CIRCLE },     /* 🔌 */
    { 0x1F511, DOS_HEART },      /* 🔑 */
    { 0x1F512, DOS_CIRCLE },     /* 🔒 */
    { 0x1F513, DOS_CIRCLE },     /* 🔓 */
    { 0x1F527, DOS_ANGLE },      /* 🔧 */
    { 0x1F528, DOS_ANGLE },      /* 🔨 */
    { 0x1F6E0, DOS_ANGLE },      /* 🛠 */
    { 0x1F52C, DOS_CIRCLE },     /* 🔬 */
    { 0x1F52D, DOS_CIRCLE },     /* 🔭 */
    /* Marks used in lists/status text. */
    { 0x1F4CC, DOS_STAR },       /* 📌 */
    { 0x1F4CD, DOS_STAR },       /* 📍 */
    { 0x1F4C5, DOS_CIRCLE },     /* 📅 */
    { 0x1F4C6, DOS_CIRCLE },     /* 📆 */
    { 0x1F5D3, DOS_CIRCLE },     /* 🗓 */
    { 0x1F4CA, DOS_CIRCLE },     /* 📊 */
    { 0x1F4C8, DOS_TRI_UP },     /* 📈 */
    { 0x1F4C9, DOS_TRI_DOWN },   /* 📉 */
    { 0x1F3AF, DOS_CIRCLE },     /* 🎯 */
    { 0x1F3AE, DOS_CHESS },      /* 🎮 */
    { 0x1F3B2, DOS_CIRCLE },     /* 🎲 */
};

bool emoji_is_codepoint(uint32_t cp)
{
    return (cp >= 0x1F000 && cp <= 0x1FAFF) ||
           (cp >= 0x1F1E6 && cp <= 0x1F1FF) ||
           (cp >= 0x2600 && cp <= 0x27BF) ||
           (cp >= 0x2B00 && cp <= 0x2BFF) ||
           cp == 0x2328 || cp == 0x23F0 || cp == 0x23F3 ||
           cp == 0x2708 || cp == 0x2728 ||
           cp == 0x203C || cp == 0x2049;
}

bool emoji_is_modifier(uint32_t cp)
{
    return cp == 0x200D ||                 /* zero-width joiner */
           cp == 0xFE0E || cp == 0xFE0F || /* variation selectors */
           cp == 0x20E3 ||                 /* combining keycap */
           (cp >= 0x1F3FB && cp <= 0x1F3FF); /* skin tones */
}

uint32_t emoji_dos_glyph(uint32_t cp)
{
    for (size_t i = 0; i < sizeof(s_fallbacks) / sizeof(s_fallbacks[0]); i++) {
        if (s_fallbacks[i].cp == cp) {
            return s_fallbacks[i].glyph;
        }
    }
    /* Range defaults keep unknown emoji readable (never tofu) and deterministic
     * so two renders of the same page are identical. */
    if (cp >= 0x1F600 && cp <= 0x1F64F) return DOS_SMILE;   /* faces */
    if (cp >= 0x1F900 && cp <= 0x1F9FF) return DOS_CIRCLE;  /* supplemental */
    if (cp >= 0x1F300 && cp <= 0x1F5FF) return DOS_CIRCLE;  /* symbols */
    if (cp >= 0x1F680 && cp <= 0x1F6FF) return DOS_RIGHT;   /* transport */
    if (cp >= 0x1F1E6 && cp <= 0x1F1FF) return DOS_SQUARE;  /* flags */
    if (cp >= 0x1F000 && cp <= 0x1FAFF) return DOS_CIRCLE;  /* the rest */
    if (cp >= 0x2600 && cp <= 0x27BF) return DOS_STAR;      /* dingbats */
    if (cp >= 0x2B00 && cp <= 0x2BFF) return DOS_STAR;
    return 0;
}

size_t emoji_utf8_encode(uint32_t cp, char out[5])
{
    if (cp < 0x80) {
        out[0] = (char)cp;
        out[1] = '\0';
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        out[2] = '\0';
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        out[3] = '\0';
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    out[4] = '\0';
    return 4;
}

size_t emoji_utf8_decode(const char *text, uint32_t *cp_out)
{
    const unsigned char *p = (const unsigned char *)text;
    size_t len;
    uint32_t cp;

    if (text == NULL || cp_out == NULL || p[0] == '\0') {
        if (cp_out) *cp_out = 0;
        return 0;
    }
    if (p[0] < 0x80) {
        *cp_out = p[0];
        return 1;
    }
    if ((p[0] & 0xE0) == 0xC0) {
        len = 2;
        cp = p[0] & 0x1F;
    } else if ((p[0] & 0xF0) == 0xE0) {
        len = 3;
        cp = p[0] & 0x0F;
    } else if ((p[0] & 0xF8) == 0xF0) {
        len = 4;
        cp = p[0] & 0x07;
    } else {
        *cp_out = 0xFFFD;
        return 1;
    }
    for (size_t i = 1; i < len; i++) {
        if ((p[i] & 0xC0) != 0x80) {
            *cp_out = 0xFFFD;
            return 1;
        }
        cp = (cp << 6) | (p[i] & 0x3F);
    }
    *cp_out = cp;
    return len;
}

size_t emoji_fallback_utf8(uint32_t cp, char out[5])
{
    uint32_t glyph = emoji_dos_glyph(cp);

    if (glyph == 0) {
        out[0] = '\0';
        return 0;
    }
    return emoji_utf8_encode(glyph, out);
}
