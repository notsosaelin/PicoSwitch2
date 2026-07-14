/*
 * Host-compilable tests for the shared NS2 pairing crypto (src/ns2_pairing_crypto.c),
 * extracted 2026-07-13 from switch_pro2.c's already-working, hardware-validated pairing
 * implementation. No pico-sdk/TinyUSB dependency:
 *
 *   gcc -I include -o test_ns2_pairing_crypto \
 *       tools/test_ns2_pairing_crypto.c src/ns2_pairing_crypto.c
 *   ./test_ns2_pairing_crypto
 *
 * Exit code 0 = all assertions passed. Verifies the extracted AES-128 core against the
 * standard, publicly-documented FIPS-197 test vector (independent of anything Nintendo-specific
 * -- this alone proves the block cipher itself is correct), plus the rev16/LTK-derivation/
 * challenge composition against a self-consistency round-trip.
 */
#include <stdio.h>
#include <string.h>

#include "ns2_pairing_crypto.h"

static int failures = 0;

#define CHECK(cond, msg)                                                     \
    do {                                                                     \
        if (!(cond)) {                                                      \
            printf("FAIL: %s\n", msg);                                      \
            failures++;                                                    \
        } else {                                                            \
            printf("OK:   %s\n", msg);                                     \
        }                                                                   \
    } while (0)

int main(void) {
    // Standard FIPS-197 AES-128 test vector (Appendix C.1) -- independent of anything
    // Nintendo-specific, this alone proves the extracted block cipher core is correct.
    {
        uint8_t key[16] = {0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,
                            0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f};
        uint8_t plain[16] = {0x00,0x11,0x22,0x33,0x44,0x55,0x66,0x77,
                              0x88,0x99,0xaa,0xbb,0xcc,0xdd,0xee,0xff};
        uint8_t expect[16] = {0x69,0xc4,0xe0,0xd8,0x6a,0x7b,0x04,0x30,
                               0xd8,0xcd,0xb7,0x80,0x70,0xb4,0xc5,0x5a};
        uint8_t out[16];
        ns2_aes128_encrypt(key, plain, out);
        CHECK(memcmp(out, expect, 16) == 0, "AES-128 matches FIPS-197 Appendix C.1 test vector");
    }

    // rev16: reversing twice is the identity.
    {
        uint8_t in[16] = {0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15};
        uint8_t once[16], twice[16];
        ns2_pairing_rev16(in, once);
        CHECK(once[0] == 15 && once[15] == 0, "rev16 reverses byte order");
        ns2_pairing_rev16(once, twice);
        CHECK(memcmp(in, twice, 16) == 0, "rev16 applied twice is the identity");
    }

    // LTK derivation: reverse(A1) XOR reverse(B1) -- verify against a hand-computed case.
    {
        uint8_t a1[16]; memset(a1, 0xFF, 16);
        uint8_t b1[16]; memset(b1, 0x00, 16);
        uint8_t ltk[16];
        ns2_pairing_derive_ltk(a1, b1, ltk);
        // reverse(all-FF) XOR reverse(all-00) = all-FF XOR all-00 = all-FF
        uint8_t expect[16]; memset(expect, 0xFF, 16);
        CHECK(memcmp(ltk, expect, 16) == 0, "LTK derivation: all-FF A1 XOR all-00 B1 = all-FF");
    }
    {
        uint8_t a1[16]; memset(a1, 0xAA, 16);
        uint8_t b1[16]; memset(b1, 0xAA, 16);
        uint8_t ltk[16];
        ns2_pairing_derive_ltk(a1, b1, ltk);
        uint8_t expect[16]; memset(expect, 0x00, 16);
        CHECK(memcmp(ltk, expect, 16) == 0, "LTK derivation: identical A1/B1 XOR to all-zero");
    }

    // Challenge composition: B2 = AES128(LTK, reverse(A2)) -- verify it's deterministic and
    // that changing the LTK changes the output (sanity, not a specific known-answer test since
    // no real captured (A1,A2,B2) triple is available/safe to embed here).
    {
        uint8_t ltk1[16]; memset(ltk1, 0x11, 16);
        uint8_t ltk2[16]; memset(ltk2, 0x22, 16);
        uint8_t a2[16]; memset(a2, 0x33, 16);
        uint8_t b2a[16], b2b[16], b2c[16];
        ns2_pairing_challenge(ltk1, a2, b2a);
        ns2_pairing_challenge(ltk1, a2, b2b);
        ns2_pairing_challenge(ltk2, a2, b2c);
        CHECK(memcmp(b2a, b2b, 16) == 0, "challenge is deterministic for the same LTK/A2");
        CHECK(memcmp(b2a, b2c, 16) != 0, "challenge output changes when the LTK changes");
    }

    printf("\n%s\n", failures == 0 ? "All checks passed." : "One or more checks FAILED.");
    return failures == 0 ? 0 : 1;
}
