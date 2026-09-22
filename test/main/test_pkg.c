/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file test_pkg.c
 * @brief Unit tests for the `pkg` pure helpers (pkg_commands.c).
 *
 * Only the I/O-free helpers are covered here; the pkg verbs' file operations
 * are hardware-verified (see tools/pkg_test.py).
 */

#include "unity.h"
#include "command.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

void test_pkg_app_name_from_appinfo_ok(void)
{
    char out[32];

    TEST_ASSERT_TRUE(pkg_app_name_from_appinfo("BOUNCE.APPINFO", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("BOUNCE", out);

    /* Case is uppercased. */
    TEST_ASSERT_TRUE(pkg_app_name_from_appinfo("Snake.appinfo", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("SNAKE", out);

    /* Underscore/dash are allowed app-name characters. */
    TEST_ASSERT_TRUE(pkg_app_name_from_appinfo("tcmd-1_x.APPINFO", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("TCMD-1_X", out);
}

void test_pkg_app_name_from_appinfo_bad(void)
{
    char out[32];

    /* Not an APPINFO name. */
    TEST_ASSERT_FALSE(pkg_app_name_from_appinfo("BOUNCE.BAT", out, sizeof(out)));
    TEST_ASSERT_FALSE(pkg_app_name_from_appinfo("APPINFO", out, sizeof(out)));
    TEST_ASSERT_FALSE(pkg_app_name_from_appinfo(".APPINFO", out, sizeof(out)));
    /* Disallowed characters in the base (space, dot, slash). */
    TEST_ASSERT_FALSE(pkg_app_name_from_appinfo("A B.APPINFO", out, sizeof(out)));
    TEST_ASSERT_FALSE(pkg_app_name_from_appinfo("A.B.APPINFO", out, sizeof(out)));
    TEST_ASSERT_FALSE(pkg_app_name_from_appinfo("A/B.APPINFO", out, sizeof(out)));
    /* Buffer too small. */
    TEST_ASSERT_FALSE(pkg_app_name_from_appinfo("BOUNCE.APPINFO", out, 4));
    /* NULL-safe. */
    TEST_ASSERT_FALSE(pkg_app_name_from_appinfo(NULL, out, sizeof(out)));
    TEST_ASSERT_FALSE(pkg_app_name_from_appinfo("BOUNCE.APPINFO", NULL, 0));
}

/* ========================================================================
 * Manifest signatures (ECDSA P-256, pkg_commands.c).
 *
 * Fixed test vector (private 0xC0FFEE on SECP256R1, test-only, generated
 * once on the host): message "FOO.BAT=1234ABCD\nBAR.BAT=5678EF90\n".
 * ======================================================================== */

static const char TEST_PUB_HEX[] =
    "d360332fad9bc83afaff4a740de8a516bf1b8fb3fde360ff1d03979c1f943ee2"
    "e8a66007fd276b0271265c6db092c4a0c5eb8c45fdc436502c8a095f5d5745f2";
static const char TEST_SIG_HEX[] =
    "456851efa7a01cc814f28477ada460d11791fd42bc6e82eec7e22282951e37ff"
    "55c914dbf1cee5caf68653ef10b2e67a8079108b29baf3385590b9d0f03e7982";
static const char TEST_MSG[] = "FOO.BAT=1234ABCD\nBAR.BAT=5678EF90\n";
static const char TEST_MSG_SHA[] =
    "88f9cb3de4f9b1d65ba0cedeb60afba16fb1886b75498f0d1dfe70dfaf7c5c3c";
static const char TEST_PUB_FP[] =
    "dea1538ce0fdf5cc96fe1defb5fe7ed87d8123efe0cadbced896fe817eddd57d";

static int test_hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void test_unhex(const char *hex, uint8_t *out, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++) {
        out[i] = (uint8_t)((test_hexval(hex[i * 2]) << 4) | test_hexval(hex[i * 2 + 1]));
    }
}

void test_pkg_sign_line(void)
{
    uint8_t sig[PKG_SIGN_SIG_BYTES];
    char line[256];

    TEST_ASSERT_TRUE(pkg_is_sign_line("SIGN=00"));
    TEST_ASSERT_TRUE(pkg_is_sign_line("  sign=00"));
    TEST_ASSERT_TRUE(pkg_is_sign_line("SIGN =00") == false); /* key must be exact */
    TEST_ASSERT_FALSE(pkg_is_sign_line("FOO.BAT=1234ABCD"));
    TEST_ASSERT_FALSE(pkg_is_sign_line("# comment"));
    TEST_ASSERT_FALSE(pkg_is_sign_line(""));
    TEST_ASSERT_FALSE(pkg_is_sign_line(NULL));

    snprintf(line, sizeof(line), "SIGN=%s", TEST_SIG_HEX);
    TEST_ASSERT_TRUE(pkg_sign_parse(line, sig));
    TEST_ASSERT_EQUAL_UINT8(0x45, sig[0]);
    TEST_ASSERT_EQUAL_UINT8(0x82, sig[63]);

    /* Malformed: short, long, non-hex, trailing garbage, NULL. */
    TEST_ASSERT_FALSE(pkg_sign_parse("SIGN=00", sig));
    TEST_ASSERT_FALSE(pkg_sign_parse("SIGN=ZZ", sig));
    {
        char bad[256];
        snprintf(bad, sizeof(bad), "SIGN=%s00", TEST_SIG_HEX);
        TEST_ASSERT_FALSE(pkg_sign_parse(bad, sig));
    }
    {
        char bad[256];
        snprintf(bad, sizeof(bad), "SIGN=%s TRAIL=x", TEST_SIG_HEX);
        TEST_ASSERT_FALSE(pkg_sign_parse(bad, sig));
    }
    TEST_ASSERT_FALSE(pkg_sign_parse("FOO=00", sig));
    TEST_ASSERT_FALSE(pkg_sign_parse(NULL, sig));
    TEST_ASSERT_FALSE(pkg_sign_parse(line, NULL));
}

void test_pkg_sign_canonical(void)
{
    char out[256];
    size_t len = 0;

    /* Comments, blanks, and SIGN lines drop out; one LF each survives. */
    TEST_ASSERT_EQUAL_INT(0, pkg_sign_canonical(
        "; header\nFOO.BAT=1\n\n# c\nSIGN=00\nBAR.BAT=2\r\n", out, sizeof(out), &len));
    TEST_ASSERT_EQUAL_STRING("FOO.BAT=1\nBAR.BAT=2\n", out);
    TEST_ASSERT_EQUAL_INT(20, (int)len);

    /* Overflow and NULL-safety. */
    TEST_ASSERT_EQUAL_INT(1, pkg_sign_canonical("FOO.BAT=1\n", out, 4, NULL));
    TEST_ASSERT_EQUAL_INT(1, pkg_sign_canonical("x", NULL, 0, NULL));
    TEST_ASSERT_EQUAL_INT(0, pkg_sign_canonical(NULL, out, sizeof(out), &len));
    TEST_ASSERT_EQUAL_STRING("", out);
}

void test_pkg_sign_hash(void)
{
    uint8_t hash[PKG_SIGN_HASH_BYTES];
    char hex[PKG_SIGN_HASH_BYTES * 2 + 1];

    TEST_ASSERT_EQUAL_INT(0, pkg_sign_hash((const uint8_t *)TEST_MSG,
                                          strlen(TEST_MSG), hash));
    for (int i = 0; i < PKG_SIGN_HASH_BYTES; i++) {
        int hi = test_hexval(TEST_MSG_SHA[i * 2]);
        int lo = test_hexval(TEST_MSG_SHA[i * 2 + 1]);
        TEST_ASSERT_EQUAL_UINT8((uint8_t)((hi << 4) | lo), hash[i]);
        snprintf(hex + i * 2, 3, "%02x", hash[i]);
    }
    TEST_ASSERT_EQUAL_STRING(TEST_MSG_SHA, hex);
    TEST_ASSERT_EQUAL_INT(1, pkg_sign_hash(NULL, 0, hash));
    TEST_ASSERT_EQUAL_INT(1, pkg_sign_hash((const uint8_t *)"x", 1, NULL));
}

void test_pkg_sign_verify(void)
{
    uint8_t pub[PKG_SIGN_PUB_BYTES];
    uint8_t sig[PKG_SIGN_SIG_BYTES];
    uint8_t bad[PKG_SIGN_SIG_BYTES];

    test_unhex(TEST_PUB_HEX, pub, sizeof(pub));
    test_unhex(TEST_SIG_HEX, sig, sizeof(sig));

    /* The fixed vector verifies. */
    TEST_ASSERT_EQUAL_INT(0, pkg_sign_verify((const uint8_t *)TEST_MSG,
                                            strlen(TEST_MSG), sig, pub));
    /* A flipped bit fails; so does a wrong key or message. */
    memcpy(bad, sig, sizeof(bad));
    bad[0] ^= 0x01;
    TEST_ASSERT_EQUAL_INT(1, pkg_sign_verify((const uint8_t *)TEST_MSG,
                                            strlen(TEST_MSG), bad, pub));
    memcpy(bad, pub, sizeof(bad));
    bad[63] ^= 0x01;
    TEST_ASSERT_EQUAL_INT(1, pkg_sign_verify((const uint8_t *)TEST_MSG,
                                            strlen(TEST_MSG), sig, bad));
    TEST_ASSERT_EQUAL_INT(1, pkg_sign_verify((const uint8_t *)"other",
                                            5, sig, pub));
    /* NULL-safe. */
    TEST_ASSERT_EQUAL_INT(1, pkg_sign_verify(NULL, 0, sig, pub));
    TEST_ASSERT_EQUAL_INT(1, pkg_sign_verify((const uint8_t *)TEST_MSG,
                                            strlen(TEST_MSG), NULL, pub));
    TEST_ASSERT_EQUAL_INT(1, pkg_sign_verify((const uint8_t *)TEST_MSG,
                                            strlen(TEST_MSG), sig, NULL));

    /* Fingerprint is SHA-256 of the raw key. */
    {
        char fp[PKG_SIGN_HASH_BYTES * 2 + 1];
        pkg_sign_fingerprint(pub, fp);
        TEST_ASSERT_EQUAL_STRING(TEST_PUB_FP, fp);
    }
    pkg_sign_fingerprint(NULL, NULL);
}

void test_pkg_sign_check_manifest(void)
{
    char manifest[512];
    char fp[PKG_SIGN_HASH_BYTES * 2 + 1];
    uint8_t pub[PKG_SIGN_PUB_BYTES];

    test_unhex(TEST_PUB_HEX, pub, sizeof(pub));
    snprintf(manifest, sizeof(manifest), "; demo\n%s\nSIGN=%s\n",
             "FOO.BAT=1234ABCD\nBAR.BAT=5678EF90", TEST_SIG_HEX);
    fp[0] = '\0';
    TEST_ASSERT_EQUAL_INT(PKG_SIGN_OK, pkg_sign_check_with_key(manifest, pub, fp));
    TEST_ASSERT_EQUAL_STRING(TEST_PUB_FP, fp);

    /* Unsigned: CRC-only contract. */
    TEST_ASSERT_EQUAL_INT(PKG_SIGN_NONE,
                          pkg_sign_check_with_key("FOO.BAT=1\n", pub, NULL));
    /* Tampered payload breaks the signature. */
    snprintf(manifest, sizeof(manifest), "FOO.BAT=99999999\nBAR.BAT=5678EF90\nSIGN=%s\n",
             TEST_SIG_HEX);
    TEST_ASSERT_EQUAL_INT(PKG_SIGN_BAD, pkg_sign_check_with_key(manifest, pub, NULL));
    /* Malformed and doubled SIGN lines are BAD, not skippable. */
    TEST_ASSERT_EQUAL_INT(PKG_SIGN_BAD, pkg_sign_check_with_key("SIGN=00\n", pub, NULL));
    snprintf(manifest, sizeof(manifest), "SIGN=%s\nSIGN=%s\n", TEST_SIG_HEX, TEST_SIG_HEX);
    TEST_ASSERT_EQUAL_INT(PKG_SIGN_BAD, pkg_sign_check_with_key(manifest, pub, NULL));
    /* NULL text/key is an error, never OK. */
    TEST_ASSERT_EQUAL_INT(PKG_SIGN_ERROR, pkg_sign_check_with_key(NULL, pub, NULL));
    TEST_ASSERT_EQUAL_INT(PKG_SIGN_ERROR, pkg_sign_check_with_key(manifest, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(PKG_SIGN_ERROR, pkg_sign_check_manifest(NULL, NULL));
    memset(pub, 0, sizeof(pub));
}

void test_pkg_sign_stock_manifest(void)
{
    /* Exact stock shape from apps/push_pkgs.py (unsigned): must be NONE. */
    char manifest[256];
    uint8_t pub[PKG_SIGN_PUB_BYTES];

    test_unhex(TEST_PUB_HEX, pub, sizeof(pub));
    snprintf(manifest, sizeof(manifest),
             "; PKGTEST bundle manifest (pkg install PKGTEST)\nPKGTEST.BAT=DAB5799D\n");
    TEST_ASSERT_FALSE(pkg_is_sign_line("; PKGTEST bundle manifest (pkg install PKGTEST)"));
    TEST_ASSERT_FALSE(pkg_is_sign_line("PKGTEST.BAT=DAB5799D"));
    TEST_ASSERT_EQUAL_INT(PKG_SIGN_NONE, pkg_sign_check_with_key(manifest, pub, NULL));
    memset(pub, 0, sizeof(pub));
}
