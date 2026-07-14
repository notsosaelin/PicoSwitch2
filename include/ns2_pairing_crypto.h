/*
 * Shared Nintendo Switch 2 controller pairing crypto (AES-128 ECB + the LTK derivation scheme
 * used by command 0x15's sub-commands). Extracted 2026-07-13 from switch_pro2.c (where it was
 * private/static) so switch_gc.c can also perform REAL pairing crypto instead of placeholder
 * bytes -- ndeadly's own reference docs confirm "the Switch 2 console requires successful
 * pairing... [which] can be performed via both Bluetooth and USB connections," so a wired-USB
 * personality is not exempt from this requirement. Zero pico-sdk/TinyUSB dependency,
 * host-compilable and host-tested (see tools/test_ns2_pairing_crypto.c).
 *
 * Protocol (verified against ndeadly's capture, unchanged from switch_pro2.c's own citation):
 *   LTK        = reverse(A1) XOR reverse(B1)      (A1 from 0x15/04, B1 a per-controller-type
 *                                                   public constant)
 *   B2(onwire) = AES128_ECB(LTK, reverse(A2))     (A2 from 0x15/02 challenge)
 */
#ifndef NS2_PAIRING_CRYPTO_H
#define NS2_PAIRING_CRYPTO_H

#include <stdint.h>

// AES-128 ECB single-block encrypt.
void ns2_aes128_encrypt(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]);

// Reverse 16 bytes (the wire byte-order flip used throughout this pairing scheme).
void ns2_pairing_rev16(const uint8_t in[16], uint8_t out[16]);

// LTK = reverse(a1_wire) XOR reverse(device_key_b1). Caller owns the ltk_out storage -- each
// personality/controller instance keeps its own session-scoped LTK, never shared.
void ns2_pairing_derive_ltk(const uint8_t a1_wire[16], const uint8_t device_key_b1[16],
                            uint8_t ltk_out[16]);

// B2(wire) = AES128_ECB(ltk, reverse(a2_wire)).
void ns2_pairing_challenge(const uint8_t ltk[16], const uint8_t a2_wire[16],
                           uint8_t b2_wire_out[16]);

#endif  // NS2_PAIRING_CRYPTO_H
