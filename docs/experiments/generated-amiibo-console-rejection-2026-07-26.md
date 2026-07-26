# Generated amiibo rejected by Switch 2 (crypto is validated)

Date: 2026-07-26
Status: ✅ Conclusive negative result
Related: [`../switch2/amiibo-identity-and-generation.md`](../switch2/amiibo-identity-and-generation.md),
[`../switch2/nfc-implementation.md`](../switch2/nfc-implementation.md)

## Question

Does a real Switch 2 accept a key-free, raw-layout, identity-only amiibo image (correct NTAG215
structure and identity block at `0x54`, but zeroed tag/data HMACs, zeroed encrypted settings, and
no originality signature) served through the PicoSwitch2 virtual reader?

## Competing hypotheses

- **H1 (accept):** the console keys on the plaintext identity block and structural validity; prior
  virtual-reader reads succeeded, and normal 540-byte dumps do not carry `READ_SIG`.
- **H2 (reject):** the console validates the UID-bound HMAC/signature cryptography, so a zero-HMAC
  image fails regardless of a correct identity block.

## Method

- Generated a raw-layout image with [`../../tools/generate_test_amiibo.py`](../../tools/generate_test_amiibo.py)
  (identity block set; both HMACs, encrypted settings, and signature zeroed).
- Uploaded through the production portal; confirmed the portal identified it correctly (portal
  reads only the plaintext identity block, so this is expected and not evidence of validity).
- Activated on the adapter and scanned on a real Switch 2 in **Save Mode**, then again in
  **Random Mode**.

## Results

- Portal: correct name/character/series shown.
- Console: **"This isn't an amiibo"** (rejection dialog) in **both** Save Mode and Random Mode.

## Conclusion

**H2 confirmed. The Switch 2 validates amiibo cryptography, not just the identity block.** A
key-free generated image cannot function as a console amiibo. The prior virtual-reader successes
were all genuine dumps whose HMACs were valid; correct plaintext identity is necessary but not
sufficient.

`generate_test_amiibo.py` is therefore a **portal/identity-plumbing test artifact only**, not a
path to console-usable amiibo. It is retained for that narrow purpose and clearly labeled.

## Explicit non-claims

- This did **not** test a genuine dump in Random Mode (see below). It tested generated (zero-HMAC)
  files only.
- It does not establish whether the failing check is the tag HMAC (`0x34`), the data HMAC (`0x80`),
  the originality signature, or several together — only that at least one crypto check is enforced.

## Consequence for Random Mode (strong inference, needs one confirming test)

Random Mode overlays a freshly drawn UID at runtime. The tag HMAC at page `0x34` is computed over
UID-derived data, and the whole encrypted block is bound to the UID through Nintendo key
derivation. Changing the UID without recomputing the HMAC (which requires the retail
`unfixed-info`/`locked-secret` keys we do not ship) will invalidate that HMAC. **Therefore Random
Mode's raw-UID overlay is expected to be rejected even on a genuine dump**, for the same reason the
generated file was rejected.

This has not been directly observed yet: the only Random-Mode test used generated files, which were
already invalid. Before promoting or removing Random Mode, run the confirming test.

### Confirming experiment (do this next)

1. Load a **genuine** dump, select Save Mode, scan on the real console → expect accept (control).
2. Same genuine dump, select Random Mode, scan → record accept/reject.
   - Reject ⇒ inference confirmed: runtime UID randomization is infeasible without retail keys.
     Viable alternatives become (a) a pool of distinct genuine dumps of the same character, or
     (b) on-device HMAC recomputation (requires keys; out of scope for a key-free project).
   - Accept ⇒ surprising; the tag HMAC is not enforced on this path. Capture the UART trace and
     re-open the analysis.

Preserve whichever result occurs; it settles the UID-randomization question.
