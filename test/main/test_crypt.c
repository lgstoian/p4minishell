/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file test_crypt.c
 * @brief Unit tests for the pure crypt core (command/crypt_commands.c).
 *
 * Covers key derivation determinism/salting, whole-buffer envelope
 * round-trips (incl. empty input), wrong-password and tamper rejection,
 * magic validation, and short-buffer refusals. No SD or transcript needed;
 * mbedTLS runs on the target like everywhere else.
 */

#include "unity.h"
#include "command.h"

#include <string.h>

void test_crypt_derive_deterministic(void)
{
    uint8_t salt[16] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
    uint8_t key_a[32];
    uint8_t key_b[32];
    uint8_t salt2[16] = {15, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};

    TEST_ASSERT_EQUAL_INT(0, crypt_derive_key("secret", salt, key_a));
    TEST_ASSERT_EQUAL_INT(0, crypt_derive_key("secret", salt, key_b));
    TEST_ASSERT_EQUAL_MEMORY(key_a, key_b, sizeof(key_a));
    /* A different salt derives a different key. */
    TEST_ASSERT_EQUAL_INT(0, crypt_derive_key("secret", salt2, key_b));
    TEST_ASSERT_TRUE(memcmp(key_a, key_b, sizeof(key_a)) != 0);
    /* Bad arguments are refused. */
    TEST_ASSERT_TRUE(crypt_derive_key("", salt, key_a) != 0);
    TEST_ASSERT_TRUE(crypt_derive_key(NULL, salt, key_a) != 0);
    TEST_ASSERT_TRUE(crypt_derive_key("secret", NULL, key_a) != 0);
    TEST_ASSERT_TRUE(crypt_derive_key("secret", salt, NULL) != 0);
}

void test_crypt_mem_roundtrip(void)
{
    const uint8_t plain[] = "the quick brown fox jumps over the lazy dog";
    uint8_t enc[128];
    uint8_t dec[128];
    size_t enc_len = 0;
    size_t dec_len = 0;

    TEST_ASSERT_EQUAL_INT(0, crypt_encrypt_mem(plain, sizeof(plain) - 1, "pw",
                                              enc, sizeof(enc), &enc_len));
    /* Envelope overhead: 8 magic + 16 salt + 12 nonce + 16 tag = 52. */
    TEST_ASSERT_EQUAL_UINT(sizeof(plain) - 1 + 52, enc_len);
    TEST_ASSERT_EQUAL_INT(0, crypt_decrypt_mem(enc, enc_len, "pw",
                                              dec, sizeof(dec), &dec_len));
    TEST_ASSERT_EQUAL_UINT(sizeof(plain) - 1, dec_len);
    TEST_ASSERT_EQUAL_MEMORY(plain, dec, sizeof(plain) - 1);
    /* Empty plaintext round-trips too. */
    TEST_ASSERT_EQUAL_INT(0, crypt_encrypt_mem(plain, 0, "pw", enc, sizeof(enc), &enc_len));
    TEST_ASSERT_EQUAL_UINT(52, enc_len);
    TEST_ASSERT_EQUAL_INT(0, crypt_decrypt_mem(enc, enc_len, "pw", dec, sizeof(dec), &dec_len));
    TEST_ASSERT_EQUAL_UINT(0, dec_len);
}

void test_crypt_mem_rejects(void)
{
    const uint8_t plain[] = "payload";
    uint8_t enc[128];
    uint8_t dec[128];
    size_t enc_len = 0;
    size_t dec_len = 0;

    TEST_ASSERT_EQUAL_INT(0, crypt_encrypt_mem(plain, sizeof(plain) - 1, "right",
                                              enc, sizeof(enc), &enc_len));
    /* Wrong password, flipped bit, bad magic, truncation: all refused
     * identically (the caller never learns which check failed). */
    TEST_ASSERT_TRUE(crypt_decrypt_mem(enc, enc_len, "wrong", dec, sizeof(dec), &dec_len) != 0);
    enc[enc_len - 1] ^= 0x01;
    TEST_ASSERT_TRUE(crypt_decrypt_mem(enc, enc_len, "right", dec, sizeof(dec), &dec_len) != 0);
    enc[enc_len - 1] ^= 0x01;
    enc[0] = 'X';
    TEST_ASSERT_TRUE(crypt_decrypt_mem(enc, enc_len, "right", dec, sizeof(dec), &dec_len) != 0);
    TEST_ASSERT_TRUE(crypt_decrypt_mem(enc, 10, "right", dec, sizeof(dec), &dec_len) != 0);
    /* Short buffers are refused, not overflowed. */
    TEST_ASSERT_TRUE(crypt_encrypt_mem(plain, sizeof(plain) - 1, "pw",
                                       enc, 10, &enc_len) != 0);
    TEST_ASSERT_TRUE(crypt_decrypt_mem(NULL, 0, "pw", dec, sizeof(dec), &dec_len) != 0);
}
