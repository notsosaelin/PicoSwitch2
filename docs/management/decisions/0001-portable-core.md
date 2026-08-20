# 0001 — Plain Kotlin/JVM reference core plus language-neutral contract

Status: Accepted

## Context / problem

Android previously owned command construction, JSON parsing, domain models, and multi-command
workflows. JVM reuse was possible only by extracting Android source, while a non-JVM author had no
stable contract or firmware-shaped vectors.

## Considered options

- keep all behavior in the Android app and document it;
- introduce Kotlin Multiplatform, JNI, Rust, or generated bindings;
- use one plain Kotlin/JVM module plus a language-neutral reference and fixtures.

## Decision

Use `:management-core`, built with the plain Kotlin/JVM plugin, for the reference implementation.
Specify the wire behavior independently in `PROTOCOL.md` and
`tools/fixtures/management/protocol-v1.json`.

## Consequences / trade-offs

JVM clients can reuse code directly. Non-JVM clients reimplement the small contract and use the
same conformance vectors. The module remains physically Android-adjacent because that is the
existing Gradle root, but Android dependencies are mechanically forbidden. No speculative ABI,
code-generation, or multiplatform maintenance burden was introduced.

## Evidence / validation

`:management-core:test` builds without the Android plugin or SDK classes. Architecture tests reject
Android imports and the fixture drives protocol parsing independently of the app.
