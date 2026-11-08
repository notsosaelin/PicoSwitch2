# Nintendo Switch App features in the companion (design)

Status: **Shelved (2026-11-07), not on the roadmap.** The project owner judged both
ways of getting past Nintendo's client attestation too risky for now:
- the third-party f-API (D1);
- the installed-app route in §12.

This document stays as the research record for a later revisit. Nothing described
here is implemented. If the concept is revived, start from
[§14](#14-decisions-needed-before-implementation), then
[§13](#13-implementation-outline), and re-check every external fact first. Nintendo
and the f-API change often.

Scope: **everything the Nintendo Switch App (`com.nintendo.znca`) does that a
third-party client can do.** Every feature of the official app is listed in the
[feature catalogue (§2A)](#2a-feature-catalogue), whether we can carry it over or
not, with the reason. ZELDA NOTES is the flagship, not the limit.

Research date: 2026-11-06. Revised 2026-11-07 after reviewing prior-art clients:
Dycool/NSO-Album-Sync, Dycool/nso-webapp (CoralWeb), Dycool/nso-extension-backend,
Bentheminernz/reef and Dycool/nxapi, plus nxapi-znca-api `docs/api.md` and
`docs/api-auth.md` (E25–E30). Every external fact below carries its source and an
evidence class (Confirmed / Strong Evidence / Hypothesis / Unknown), as defined in
`CLAUDE.md`. Facts about Nintendo's services describe what was observed on that date.
They can change without notice.

## 1. The goal

Sign in to a Nintendo Account in the companion. Then, on one device (the motivating
case is a Retroid Pocket 5), do two things at once:

- drive the Switch 2 as the controller through the adapter (Controller Link / the
  Touch Gamepad), and
- use the Nintendo Switch App's features on the same screen:
  - every **game-specific service**: ZELDA NOTES with its live map following Link,
    SplatNet 3, NookLink (type on the handheld and it appears in Animal Crossing),
    Smash World, SplatNet 2;
  - the app's **own** features: friends and presence, adding friends, the album of
    uploaded screenshots and videos, announcements, play history, and the privacy
    settings that go with them.

In short: "Wii U 2". The handheld is the controller and also the second screen.

The rest of this document covers:

- what the official app does and how its web services are reached;
- how a third-party client can reach them, and what that costs (a third-party
  dependency, terms obligations, licensing, account risk);
- how the feature fits the companion's existing architecture and the shared shell;
- what is still unknown, and the experiments that settle it;
- a step-by-step build order with tests and acceptance criteria.

## 2. Evidence summary

| # | Fact | Class | Source |
| --- | --- | --- | --- |
| E1 | ZELDA NOTES is a game-specific service inside the free Nintendo Switch App (`com.nintendo.znca`). It works only with the *Nintendo Switch 2 Editions* of BotW and TotK. | Confirmed (vendor docs) | Nintendo support a_id 68605; nintendo.com en-gb article 2926626 |
| E2 | Nintendo Switch Online is not required for the app or ZELDA NOTES. | Confirmed (vendor docs) | same |
| E3 | Navigation: "The map on your smart device will also update with your in-game position every few seconds while using this feature." | Confirmed (vendor docs) | Nintendo support a_id 68605 |
| E4 | ZELDA NOTES has nine features: Voice Memories, Navigation (voice), My Play Data, Global Play Data, Daily Bonus, amiibo, Photo Studio, Item Sharing, and Autobuild Sharing (TotK only). | Confirmed (vendor docs) | same; nintendo.com/au/apps/zelda-notes |
| E5 | ZELDA NOTES is to support *Ocarina of Time* on Switch 2. | Strong Evidence (one vendor page) | nintendo.com/au/apps/zelda-notes only |
| E6 | Game and app sync over the internet, through Nintendo's servers. The console must be online. A Switch icon in the app turns green once connected. Some guides say the same Nintendo Account must be used on both. | **Strong Evidence**: several independent guides, plus a working third-party client that receives live position from Nintendo's server (E25) | GameSpot, Game Rant, Triforce Times guides; NSO-Album-Sync `src/zeldanotes.cpp` |
| E7 | Switch-to-phone sync uses Bluetooth. | **Disproven** for the phone side by E25, which receives live position with no Bluetooth at all. It was attractive because the phone is usually next to the console. | Nintendo Reporters (claim); NSO-Album-Sync (refutation) |
| E8 | The current Android app is **3.6.0** (Google Play, updated 2026-10-28). | Confirmed (catalogue) | `nintendoapis/nintendo-app-versions` `data/coral-google-play.json` |
| E9 | nxapi's newest public constants are `ZNCA_VERSION` 3.5.0, platform Android 12, UA `com.nintendo.znca/3.5.0(Android/12)`, API compatibility `d8fAZDPzwimzQ7c6`, base `https://api-lp1.znc.srv.nintendo.net`, client id `71b963c1b7b6d119`. | Confirmed (source read) | nxapi `src/api/coral.ts` |
| E10 | Since app 3.0.1 (June 2025), Coral API request and response bodies are encrypted. A client can only encrypt or decrypt them through the f-API's `/encrypt-request` and `/decrypt-response`. Game web services themselves are not encrypted. | Strong Evidence (maintainer docs) | nxapi-znca-api `docs` branch |
| E11 | The f-API runs the real Nintendo Switch App under Frida on rooted Android to produce `f` (hash method 1 = Coral login, 2 = web service token) and to encrypt or decrypt. | Strong Evidence (maintainer docs) | nxapi-znca-api README |
| E12 | f-API access needs an nxapi-auth token (`https://nxapi-auth.fancy.org.uk`, OAuth client credentials, scopes `ca:gf ca:er ca:dr`). Tokens are short-lived, per Coral user, and must not be persisted. | Strong Evidence (maintainer docs) | `znca-api-auth.md` |
| E13 | f-API rate limits: hash 1 is 10 per 60 min per NA user; hash 2 is 20 per 30 min per Coral user. | Strong Evidence (maintainer docs) | `znca-api.md` |
| E14 | f-API terms require: identifying headers (`X-znca-Platform`, `X-znca-Version`, `X-znca-Client-Version`); a User-Agent naming project, version and contact; imitating the official app; **telling the user before sign-in** that the id_token, the Coral token and Coral data go to a third party, and getting explicit acknowledgement; caching tokens for their whole validity; no automatic retries; features free, with no ads or paid gating. | Confirmed (terms text read) | `znca-public-api-terms.md` |
| E15 | Both `/config` and `/health` on the f-API returned **HTTP 502** on 2026-11-06. | Confirmed (observed once) | `curl` on 2026-11-06 |
| E16 | nxapi is AGPL-3.0. nxapi-znca-api has **no licence**. PicoSwitch2 is Apache-2.0. | Confirmed | GitHub; repo `LICENSE` |
| E17 | nxapi's Electron app opens ZELDA NOTES; the app connects to the Switch and detects location. Its one reported bug: `window.znca._private.onPersistentDataRestore` was not called. | Strong Evidence (issue report) | nxapi issue #151 |
| E18 | Web service loading recipe and JS bridge (headers, user agent, `whiteList`, persistent data, bridge methods). See §6. | Confirmed (source read), app-version dependent | nxapi `src/app/main/webservices.ts`, `preload-webservice/znca-js-api.ts` |
| E19 | Android WebView's `setRequestedWithHeaderOriginAllowList` is deprecated and does nothing. Whether the installed WebView sends `X-Requested-With` (with *our* package name) depends on its version. | Strong Evidence | developer.android.com; blink-dev intents |
| E20 | nxapi says console bans do not happen and account bans are unlikely. In 2023 a small number of SplatNet 3 users were banned, which nxapi says was unrelated to nxapi. | Hypothesis-grade reassurance from an interested party | nxapi README |
| E21 | Press reports of the 2025 Nintendo Account User Agreement say it prohibits reverse engineering, circumvention and use beyond the documented purpose. | Strong Evidence (press; agreement text not read here) | press coverage |
| E22 | Retroid Pocket 5: Android 13, Snapdragon 865, 8 GB, 5.5" 1080p touchscreen, Wi-Fi 6, BT 5.1, Play Store. One review says Google Play services are off by default. | Strong Evidence (spec sheets); GMS claim Hypothesis | retailer and spec pages; one review |
| E23 | Our Android companion (minSdk 28, targetSdk 35) already holds `INTERNET`. `MainActivity` routes every key and motion event to the input backend before any view sees it. | Confirmed (repo) | `companions/android/app/build.gradle.kts`, `AndroidManifest.xml`, `MainActivity` |
| E24 | Linux companion is Avalonia; Windows companion is WinUI 3. | Confirmed (repo) | `companions/linux/src/*.csproj`, `companions/windows` |
| E25 | ZELDA NOTES live position is delivered by Nintendo's server as a **Server-Sent Events stream**. Details in §8.1. | Strong Evidence (working third-party implementation; not reproduced by us) | Dycool/NSO-Album-Sync `src/zeldanotes.cpp`, `include/nso_album_sync/zeldanotes.hpp` (MIT) |
| E26 | ZELDA NOTES has two web service ids, `5935781783175168` and `4974384874151936`. Both are served from `https://api.lp1.87abc152.srv.nintendo.net`, whitelist `api.lp1.87abc152.srv.nintendo.net` and `87abc152.srv.nintendo.net`. Why there are two ids is **Unknown** (region? OoT?). | Strong Evidence (three independent clients agree) | NSO-Album-Sync; Dycool/nso-extension-backend `src/services/service-policy.ts`; Bentheminernz/reef `src/lib/server/game-webview.ts` |
| E27 | ZELDA NOTES runs outside the official app: CoralWeb hosts it in a desktop-browser iframe with the bridge injected, and Reef through a server-side proxy. | Strong Evidence (two shipped third-party clients; not run by us) | Dycool/nso-webapp, Dycool/nso-extension-backend, Bentheminernz/reef |
| E28 | nxapi-auth clients are **self-registered** at `https://nxapi-auth.fancy.org.uk/oauth/clients`. Public clients send only `client_id`; a secret or assertion is for confidential clients. A refresh-token grant exists, and on `invalid_grant` the client falls back to client credentials once. | Confirmed (maintainer docs) | nxapi-znca-api `docs/api-auth.md` |
| E29 | f-API error contract: JSON `error` plus `debug_id` (also `X-Trace-Id`). The errors are `service_unavailable` 503, `unauthorised`/`invalid_token` 401, `insufficient_scope` 403, `invalid_request` 400/415, `unsupported_platform` 400, `unsupported_version` 406, `incompatible_client` 400 and `rate_limit` 429. `/f` with `encrypt_token_request` returns the encrypted Coral body in the same call, and counts only against the auth rate limit. | Confirmed (maintainer docs) | nxapi-znca-api `docs/api.md` |
| E30 | Prior-art licences: NSO-Album-Sync, CoralWeb (`nso-webapp`) and `nso-extension-backend` are **MIT**. Reef is **AGPL-3.0**. `Dycool/nxapi` is a mirror identical to upstream (0 commits ahead or behind on 2026-11-07). | Confirmed | GitHub API |

Unknowns that block a confident build are collected in [§11](#11-unknowns-and-experiments).

## 2A. Feature catalogue

Sources:
- Google Play description of 3.6.0 (**P**; `nintendo-app-versions`, `coral-google-play.json`);
- Nintendo Switch App Overview and FAQ, Americas a_id 26938 (**F**);
- How to Voice Chat, a_id 41223 (**V**);
- UK "Unable to Access Game-Specific Services" 2346368 (**U**);
- patch-note coverage of 3.3.0 (Nintendo Life, 2026-03-17) and 3.5.0 (My Nintendo
  News, 2026-09-03);
- nxapi `src/api/coral.ts` and `coral-types.ts` for the endpoint names (**N**).

Membership and age come from F, an Americas page. Other regions may differ.

**How each feature is carried over:**
- **Web**: Nintendo's own page, hosted in our WebView (§6). Once the generic host
  exists, the page needs nothing per service except the quirks in §8.
- **Native**: a Coral API call drawn by our own UI. Every Coral call costs one
  f-API encrypt and one decrypt (E10), so these features are on demand, not polled
  (§4).
- **No**: not portable, with the reason given.

### Game-specific services

| Service | What it does (P, F) | NSO needed | Way | Notes |
| --- | --- | --- | --- | --- |
| ZELDA NOTES (BotW / TotK Switch 2 Editions; OoT per U) | Navigation with live map and voice, Voice Memories, play data, daily bonus, amiibo bonus, Photo Studio, item and Autobuild QR sharing | No (F) | Web | §8. Needs online play and the Nintendo Account privacy setting that allows sharing play data (U). |
| SplatNet 3 (Splatoon 3) | Friends online in the game, battle and Salmon Run stats, stage schedules, replays, ordering gear that arrives in the game | **Yes** (F, U) | Web | §8.2 |
| NookLink (Animal Crossing: New Horizons) | Messages typed on the phone appear in the game; reactions; Best Friends online; send 3DS custom designs into the game; island newspaper | **Yes** (F, U) | Web | §8.3. The strongest second-screen feature after ZELDA NOTES. |
| Smash World (Super Smash Bros. Ultimate) | Posted images and videos, queue user-stage downloads into the game, event notices | **Yes** (F, U) | Web | §8.4 |
| SplatNet 2 (Splatoon 2) | Leaderboards, map schedules, battle and Salmon Run stats | No (F) | Web | §8.4 |

Common requirements (U):
- the console game has save data on the same Nintendo Account;
- console Parental Controls do not restrict free communication;
- the device's WebView is current.

The list of services comes from `/v4/GameWebService/List`, so a new service appears
without code. It only needs quirks if it breaks.

### The app's own features

| Feature | NSO needed (F) | Way | Coral endpoints (N) | Notes |
| --- | --- | --- | --- | --- |
| Friend list with online status and current game | No | Native | `/v4/Friend/List`, `/v4/Friend/Show`, `/v4/Friend/IsNew/Delete` | Refreshed on open and by pull-to-refresh, never polled in the background (§4). |
| Favourite friends | No | Native | `/v3/Friend/Favorite/Create`, `/Delete` | |
| Friend notes (added in 3.3.0) | No | Native | `/v4/Friend/Note/Update` | |
| Add friends by friend code, own QR code, scan a QR code | No | Native | `/v3/Friend/GetUserByFriendCode`, `/v3/Friend/GetUserByFriendCodeHash`, `/v3/Friend/CreateFriendCodeUrl`, `/v4/FriendRequest/Create` | Scanning a QR code needs the camera; it shares the QR reader the ZELDA NOTES and SplatNet sharing features need (U6). |
| Friend requests received and sent: accept, reject, cancel | No | Native | `/v4/FriendRequest/Received/List`, `/Show`, `/MarkAsRead`; `/v3/FriendRequest/Sent/List`, `/Accept`, `/Reject`, `/Cancel` | |
| Remove friend, block, unblock | No | Native | `/v3/Friend/Delete`, `/v3/User/Block/List`, `/Create`, `/Delete` | Destructive actions confirm first. |
| Album: Switch 2 screenshots and videos uploaded from the console; view, save, share, filter, sort, delete from server (3.5.0) | No; sharing needs age 13+ (F) | Native | `/v4/Media/List`, `/v4/Media/Delete`, `/v5/Hashtag/List` (prefilled share text) | NSO-Album-Sync downloads these (fields `appName`, `type`, `capturedAt`, `contentUri`, `thumbnailUri`). Switch 2 captures only (F). Retention and limits are **Unknown**. |
| Play history | No | Native | `/v4/User/PlayLog/Show` | |
| Own profile and presence visibility | No | Native | `/v4/User/ShowSelf`, `/v3/User/Permissions/ShowSelf`, `/v4/User/Permissions/UpdateSelf` | |
| Announcements (operation notices, friend requests) | No | Native | `/v4/Announcement/List`, `/MarkAsRead` | nxapi has types only for OPERATION and FRIEND_REQUEST announcements. |
| GameChat history and "friend request someone you GameChatted with" (3.2.0) | No | Native, Phase 3 | `/v5/Chat/List`, `/v5/Chat/Show`, `/v5/Chat/Member/Show`, `/v5/Chat/FriendCandidate/List` | Response shapes are **Unknown**: nxapi lists these as not yet typed. That they back this feature is Strong Evidence (names and release timing), not Confirmed. |
| **Push notifications**: friend online, friend requests, GameChat invites, voice chat Start Alerts | No | **No** | `/v1/Notification/RegisterDevice`, `/v5/PushNotification/Settings/*` | Nintendo sends these through Firebase Cloud Messaging to *its* app's Firebase project. A registration token from our app belongs to our project, so Nintendo's server cannot deliver to it. Using Nintendo's Firebase identity, taken from its APK, is rejected (§12). Background polling to imitate pushes is ruled out by the cost of encrypt and decrypt and by the f-API terms (§4). **What we do instead:** the friend list and request badges are fresh whenever the user opens them. |
| **Smart-device voice chat** in supported games (Splatoon 3, Smash Ultimate, Nintendo Classics; not Switch 2 exclusives) | **Yes**, age 13+ (F, V) | **No** | `/v1/Event/*`, `/v1/Voip/*` | Voice runs in Nintendo's native `libvoip`, the same library that computes `f` (nxapi README; E11). A third party would have to run or reimplement Nintendo's binary, which is rejected (§12). |
| **GameChat** itself | — | **No** | — | Joining GameChat happens only on a Switch 2 console; the official app only receives invites (F). |
| Parental Controls | — | Out of scope | — | A separate app (`moon` in nxapi), not part of the Nintendo Switch App. F says the Switch App is not covered by Parental Controls. |

## 3. How the official app reaches a web service

The pipeline has four stages. Every arrow is a network call. **f** marks the calls
that need the f-API (§4).

```text
 Nintendo Account (accounts.nintendo.com)
   1. /connect/1.0.0/authorize       browser sign-in, PKCE S256
         response_type=session_token_code
         redirect npf71b963c1b7b6d119://auth#session_token_code=...
   2. /connect/1.0.0/api/session_token  -> session_token   (long-lived, years)
   3. /connect/1.0.0/api/token          -> id_token, access_token  (≈15 min)
         grant urn:ietf:params:oauth:grant-type:jwt-bearer-session-token
   4. api.accounts.nintendo.com/2.0.0/users/me -> NA user (id, country, language, birthday)

 Coral (api-lp1.znc.srv.nintendo.net)          bodies encrypted since 3.0.1 (E10)
   5. f(hash 1, id_token)                       [f-API /f]
   6. /v4/Account/Login (id_token, f, ...)      -> Coral user + webApiServerCredential (≈2 h)
         request encrypted [f-API /encrypt-request], response decrypted [/decrypt-response]
   7. /v4/GameWebService/List                   -> WebService[] {id, uri, whiteList, customAttributes, name, imageUri}

 Web service token
   8. f(hash 2, coral token)                    [f-API /f]
   9. /v4/Game/GetWebServiceToken (id, f, ...)  -> {accessToken, expiresIn}
         encrypted like step 6

 Web service (the game's own site; not encrypted)
  10. GET <uri>?lang=..&na_country=..&na_lang=..
        x-gamewebtoken: <accessToken>, x-appplatform: android,
        x-appcolorscheme: DARK|LIGHT, dnt: 1, X-Requested-With: com.nintendo.znca
  11. The page runs, talks to its own backend, and asks the app (JS bridge) for
      tokens, persistent storage, share, vibrate, close.
```

Lifetimes, from nxapi's caching code:

- The session token is long-lived and is the one durable secret.
- The NA id_token and access_token last about 15 minutes.
- The Coral token (`webApiServerCredential`) lasts about 2 hours.
- The web service token lasts as long as its `expiresIn` says.

The f-API terms (E14) require each token to be cached for its whole validity and
reused rather than requested again.

## 4. The f-API dependency

**Why it is unavoidable for a third-party client.**

- Coral computes `f` inside the official app's native library.
- Since 3.0.1, Coral also encrypts request and response bodies (E10, E11).
- Without `f` and the body crypto, steps 5–9 cannot be completed.

So an independent client has two choices:

1. **Use a public f-API** (nxapi-znca-api is the maintained one). Its terms are the
   price; see E12–E14.
2. **Run the official app's native code ourselves**, for example by loading
   `libvoipjni` on the user's own device, or with an on-device Frida server. This
   is **rejected** (§12). It means redistributing or instrumenting Nintendo's
   binary, and it is the clearest case of the circumvention the Nintendo Account
   agreement prohibits (E21). It also needs root on the user's device.

**Consequences of option 1.** Each one is a design requirement.

- **Trust.** The third party sees the NA id_token, the Coral token, and every
  decrypted Coral response, including friends and account data. It never sees the
  session token. Design rule: the session token never leaves the device. Only what
  a step needs is sent: the id_token for hash 1, and the Coral token plus the
  encrypted bodies for steps 6–9.
- **Consent before sign-in** (E14). A screen states what is shared, with whom, and
  why, and links the f-API project. The user must acknowledge it; it cannot be
  dismissed. If the acknowledgement is withdrawn, local tokens are erased.
- **Availability.** E15 is a 502 on the day of research. The feature has to show
  "the sign-in service is unavailable" as a first-class state, not as a crash or a
  silent retry. The terms forbid automatic retries; retrying is the user's action.
- **Rate limits** (E13). With the caching in §3, normal use is one hash-1 call per
  Coral session (about every 2 h of active use) and one hash-2 call per web-service
  open beyond the token's validity. Both are far below the limits. Counters in
  Diagnostics show it.
- **Every native feature costs two f-API calls per Coral request.** That covers
  friends, album, requests, announcements and the rest of §2A: one
  `/encrypt-request` and one `/decrypt-response` (E10). The encryption rate limit
  is still "TODO" in the f-API docs (E29). The terms also say:
  - tokens are renewed only "as Nintendo's official apps/web services do";
  - requests to Nintendo are never retried automatically;
  - automated requests are allowed only "at a limited frequency, only if the user
    has explicitly allowed this, and has recently interacted". This rule sits under
    the heading for server-side services. We adopt it anyway as the stricter
    reading.

  Design rules that follow:
  - Native data is fetched **when a page opens and when the user pulls to
    refresh**. It is cached in memory, with an age stamp shown ("Updated 2 min
    ago").
  - The friend list auto-refreshes, while open and visible only, at most once a
    minute, and only after the user turns it on.
  - There is **no** background polling and no imitation of push notifications.
  - Game web services do not count against any of this. Their own traffic goes
    straight to the game's servers and is not encrypted (E10).
- **Identity headers** (E14). We send the User-Agent
  `PicoSwitch2-Companion/<version> (+<repo url>; <contact>)`, `X-znca-Platform: Android`,
  `X-znca-Version: <the Coral version we imitate>`, and the `X-znca-Client-Version`
  that nxapi-auth issues. **Decision needed:** the contact address (§14).
- **Client registration** (E28). Self-service at
  `https://nxapi-auth.fancy.org.uk/oauth/clients`, as a **public client**: the app
  ships only a `client_id`, never a secret.
  - The client information entered there may be shown to users later, so it names
    PicoSwitch2 and a contact.
  - The `client_id` is build configuration (a Gradle property), not a source
    constant, so forks register their own.
  - NSO-Album-Sync makes it configurable the same way.
  - The docs say `X-znca-Client-Version` is "a random per-client value issued by
    nxapi-auth" from 3.1.0. NSO-Album-Sync instead sends `d8fAZDPzwimzQ7c6`,
    nxapi's own compatibility constant. Which one a newly registered client should
    send is **Unknown U12**. It is answered at registration, and it is never set
    dynamically.
- **Errors** (E29) map one-to-one onto the state machine (§7.3):
  - 503 and 5xx → `FApiUnavailable`;
  - 406 `unsupported_version` and 400 `incompatible_client` → `UpdateRequired`;
  - 429 → `RateLimited(until Retry-After)`;
  - 401 `invalid_token` → one refresh, then client credentials.
  The `debug_id` is kept for Diagnostics. A `Retry-After` backoff is **persisted**
  so that restarting the app cannot bypass it. NSO-Album-Sync does the same.
- **One call, not two.** Login and GetWebServiceToken use `/f` with
  `encrypt_token_request`, which returns the encrypted body directly (E29). This
  halves the f-API calls and keeps them under the auth limits.
- **Version coupling.** Coral rejects versions it no longer accepts. The f-API
  reports through `/config` which app version it runs. We read the version we
  imitate from `/config` at sign-in and never hard-code it alone. E8/E9 already
  show the gap: nxapi's constant is 3.5.0, and Play has shipped 3.6.0.

## 5. Licensing

- **PicoSwitch2 is Apache-2.0.** Apache-2.0 code cannot include AGPL-3.0 code
  without the combined work becoming AGPL. **Rule: no nxapi source is copied or
  translated line by line.**
  - The Kotlin implementation is written from the *protocol facts* in this
    document: endpoints, field names, header names, and flow order. These are facts
    about Nintendo's service, not nxapi's expression.
  - Where nxapi is cited, it is cited as evidence, the way captures are cited.
- **nxapi-znca-api has no licence.** We do not use its code. We call its public
  HTTP API under its published terms, as any client does.
- **Reef is AGPL-3.0** (E30). It falls under the same rule as nxapi: read as
  evidence, never copied.
- **NSO-Album-Sync, CoralWeb and nso-extension-backend are MIT** (E30), which is
  compatible with Apache-2.0.
  - Their protocol knowledge is used freely. The ZELDA NOTES facts in §8.1 come
    from NSO-Album-Sync.
  - Code adapted from them must keep the MIT copyright notice, in the source file
    and in a `THIRD-PARTY-NOTICES` entry.
  - The default is still to write our own Kotlin. They are C++ and browser JS, so
    there is little to adapt anyway.
- **Nintendo's web services** run in a WebView exactly as served. We do not copy,
  modify, mirror or cache their assets beyond the WebView's own HTTP cache.

## 6. Hosting a web service

This section is the recipe nxapi's Electron app uses (E18), restated as
requirements for an Android WebView. Every item is **app-version dependent** and
must be re-verified against the current official app before release (experiment X3).

**Loading**

- URL: `webservice.uri` + `?lang=<NA language>&na_country=<NA country>&na_lang=<NA language>`,
  plus an optional deep-link query when the service's `customAttributes` include
  `deepLinkingEnabled`.
- Headers on the first request:
  - `x-appplatform: android`
  - `x-appcolorscheme: DARK|LIGHT`, following the companion theme
  - `x-gamewebtoken: <accessToken>`
  - `dnt: 1`
  - `X-Requested-With: com.nintendo.znca`. E19 applies: on recent WebViews the
    platform sets this header itself to our package name, and an explicit header
    may be overridden. Whether the services check it is **Unknown U4**.
- User agent: the official Android app's WebView UA is the system WebView UA with
  ` com.nintendo.znca/<version>` appended. **Strong Evidence:** NSO-Album-Sync and
  Reef both send exactly that shape to ZELDA NOTES (E25, E27).
  - We therefore use our WebView's real UA and append the suffix, with the version
    from `/config`.
  - NSO-Album-Sync's UA is hard-coded with Chrome 80 and znca 3.5.0 and still
    works. That suggests the service does not check either version closely
    (Hypothesis).
  - nxapi's mobile Safari UA is the iOS app's shape. Reef uses it only for NookLink
    and Smash World.
- **Token only until the session cookie exists.** For ZELDA NOTES, the first
  document request with `x-gamewebtoken` returns an `a5_token` session cookie.
  After that, Reef stops attaching the token. NSO-Album-Sync sends it only on the
  bootstrap request.
  - Our host attaches `x-gamewebtoken` through `loadUrl(url, headers)`, which
    applies to the first navigation only. That matches this behaviour without any
    request interception.
- **Initial path: Unknown U13.** The clients disagree:
  - Reef rewrites a bare `/` to `/title-select`;
  - CoralWeb marks ZELDA NOTES `preserveExactInitialUri`;
  - NSO-Album-Sync bootstraps at `/` and then uses `/<botw|totk>/complete-guide`.
  The design loads `uri` exactly as Coral returns it. If that fails, X3 decides.
- `customAttributes` include `verifyMembership`. When it is set, the official app
  checks NSO membership before opening the service. ZELDA NOTES does not need NSO
  (E2), so we expect the attribute to be absent. We honour it anyway (§8).

**Navigation**

- Top-level navigations whose host is not in the service's `whiteList` (glob
  patterns) open in the system browser, not in our WebView.
- No other origin receives `x-gamewebtoken`.

**The JS bridge** is `window.jsBridge`; nxapi also assigns each method onto
`window`. The page calls these methods, and the app answers by calling
`window.<callback>(...)`.

| Method | Our behaviour |
| --- | --- |
| `requestGameWebToken()` | Return the cached token, refreshing it through steps 8–9 if it has expired. Answer with `onGameWebTokenReceive(token)`. |
| `restorePersistentData()` | Answer with `onPersistentDataRestore(data ?? '')`. **Also call `window.znca?._private?.onPersistentDataRestore?.(data ?? '')`** (E17: the missing call broke ZELDA NOTES in nxapi). CoralWeb deliberately restores `''` for ZELDA NOTES and clears what it stored before each launch, so stale persistent data may break the page. Whether we must do the same is **Unknown U14** (X3). |
| `storePersistentData(s)` | Store per (NSA id, web service id). Answer with `onPersistentDataStore()`. |
| `invokeNativeShare(json)` / `invokeNativeShareUrl(json)` | Android share sheet. |
| `copyToClipboard(s)` | Clipboard. |
| `downloadImages(json)` | Save to MediaStore Pictures/PicoSwitch2 (Photo Studio, QR codes). |
| `openQRCodeReader(json)` / `closeQRCodeReader()` | **Unknown U6**: the Item and Autobuild Sharing receivers need a camera QR reader. Phase 3. |
| `sendMessage(json)` | `B_SHOW_SUCCESS` / `B_SHOW_ERROR` as a snackbar; `B_SET_INDEX` ignored, with a log entry. |
| `completeLoading()` | Hide our loading indicator. |
| `closeWebView()` | Close the web service and return to Play. |
| `openExternalBrowser(url)` | System browser. |
| `clearUnreadFlag()`, `reloadExtension()` | No-op, logged in Diagnostics. |
| `func_2644(json)` | Vibrate the *device*, never the console controller. CoralWeb reads `pattern` "0" / "1" / "2" (tap / success / error). |
| `func_272e()` | **ZELDA NOTES' own token request.** CoralWeb routes it to the game-web-token fetch with `isZelda: true`; nxapi answers through `window.znca._private.func_1d5e`. Strong Evidence for its meaning. Whether 3.6.0 renamed these obfuscated names is still **Unknown U3**, so the unknown-call counter covers it. |

A call the bridge does not implement is logged with its name and argument length,
but never its contents, since they may hold tokens. A diagnostics counter makes new
bridge needs visible after a Nintendo update.

**WebView settings**

- JavaScript, DOM storage and IndexedDB on.
- Third-party cookies off unless X1 shows they are needed.
- Media playback without a user gesture: on (`mediaPlaybackRequiresUserGesture = false`),
  because voice navigation is spoken in response to game events, not taps.
  **Unknown U8** until X2.
- File access off. Mixed content never.
- Remote debugging (`WebView.setWebContentsDebuggingEnabled`) only in debug
  builds. It is the main research tool (X1–X3).
- Cookies and storage live in a WebView profile used only for this feature
  (`androidx.webkit` `ProfileStore`, where the installed WebView supports profiles;
  otherwise the default profile). Signing out deletes them.

## 7. How the feature fits the companion

### 7.1 Ownership and boundaries

The feature is a new **source of nothing and output of nothing**. It never touches
the bridge, the adapter, the management protocol, the firmware or the input
pipeline. It lives in its own package and talks to the rest of the app only
through the UI shell.

```text
 dev.picoswitch.companion.nso            (new; pure Kotlin where possible)
   NaAuth          PKCE, session_token, id_token, users/me     (unit-tested with fixtures)
   FApiClient      nxapi-auth token, /f, /encrypt-request, /decrypt-response, /config
   CoralClient     Login, GameWebService/List, GetWebServiceToken (bodies via FApiClient)
   CoralFeatures   friends, requests, blocks, media, announcements, play log, permissions
                   (one typed call per §2A endpoint; the in-memory cache with age stamps; the §4 cost rules)
   NsoSession      token cache + lifetimes + state machine (§7.3); no Android types
   NsoStore        session token + persistent web data; Android Keystore AES-GCM file
 dev.picoswitch.companion.ui.nso         (new)
   NsoConsentScreen, NsoSignIn (WebView), NintendoScreen (Services / Friends / Album / Account tabs),
   WebServiceHost (WebView + bridge)
```

Firmware, `contracts/`, management wire formats and descriptors are unchanged, so
there is no protocol-version churn.

### 7.2 Input while the web service is on screen

This is the core of the "Wii U 2" idea, and it already works by construction (E23).
`MainActivity.dispatchKeyEvent` and `dispatchGenericMotionEvent` give every gamepad
event to the input backend before any view sees it. A WebView in our own activity
therefore cannot steal the controller.

- The WebView receives **touch and text only**. Touch is how the map is used. Text
  is NookLink's in-game messages and Photo Studio.
- An in-app web service keeps the activity in the foreground, so Controller Link
  stays in its foreground-service state.
- **The back key** is a controller button (B / Back reach the console). The web
  service therefore has its own on-screen close control and never relies on system
  Back.
- System gesture back is left to the system and closes the web service, the same
  as the on-screen control.
- **Text fields.** NookLink's "type on the phone, it appears in the game" (§8.3)
  needs the soft keyboard inside a page, so text input is a Phase-1 requirement.
  While a page field has focus:
  - gamepad buttons still go to Controller Link;
  - only the soft keyboard and IME commit text to the field;
  - Keyboard & Mouse forwarding does not take hardware keyboard keys meant for the
    field.
  How that last rule is decided (pause forwarding while a page field is focused,
  or an explicit toggle) is **Unknown U9**, settled by X8.

### 7.3 Session state machine

```text
 SignedOut ──consent──> Consented ──sign-in──> NaAuthorized(session_token stored)
     ^                                              │
     │                                     id_token + f(1) + Login
     │                                              v
     ├──────sign out / consent withdrawn──── CoralReady(coral token, expiry)
     │                                              │ open service: f(2) + GetWebServiceToken
     │                                              v
     │                                     ServiceOpen(id, web token, expiry)
     │
 Failure states (shown, never retried automatically):
   FApiUnavailable(http status, debug_id)   UpdateRequired(406 / incompatible_client / Coral upgrade code)
   CoralRejected(status code)   SessionExpired (session_token revoked -> back to Consented)
   RateLimited(until; persisted across restarts)
```

- Transitions happen only in `NsoSession`, a pure Kotlin class with an injectable
  clock and HTTP layer, so every transition is unit-tested.
- Expiry is checked on use, not with timers. A token is refreshed only when it is
  about to be used and has less than 60 s left.

### 7.4 Token storage

- **Session token.** Stored on device only. It is encrypted with an AES-GCM key in
  the Android Keystore (no StrongBox requirement) and kept in a file in no-backup
  storage (`noBackupFilesDir`). It is excluded from Auto Backup and device
  transfer, so it never leaves the device.
- **Coral and web tokens.** Kept in memory only and re-derived after process death.
  This is the cheapest option within the rate limits. Persisting them is the
  optimisation to make only if measurements show a need.
- **nxapi-auth token.** Memory only, as its terms require (E12).
- **Persistent data from the bridge.** Per (NSA id, web service id), in the same
  encrypted store. The page treats it as its own data.
- **Sign out** erases all of the above and the WebView profile. "Remove from this
  device" lives on the account card.

### 7.5 Sign-in on Android

The redirect is the custom scheme `npf71b963c1b7b6d119://auth`. That is the
official app's scheme, so declaring it in our manifest would race the official app
for the intent if both are installed. Imitating the official app means we have to
use that redirect, because Nintendo checks it against the client id. **We do not
register an intent filter.** Instead:

- Sign-in opens in a **WebView owned by the sign-in screen** (not the web-service
  profile).
- It intercepts navigation to `npf71b963c1b7b6d119://auth` in
  `shouldOverrideUrlLoading`, takes `session_token_code` from the fragment, and
  closes.
- The PKCE verifier is created per attempt and kept only in memory.
- **Trade-off:** a WebView sign-in cannot reuse the system browser's Nintendo
  session or its password manager. Many identity providers discourage it, and
  Nintendo's page may detect it (**Unknown U10**).
- nxapi's documented fallback is to have the user paste the redirect link. That is
  not acceptable here (no workarounds), so U10 must be settled by X4 before the
  build.
- **Desktop precedent.** NSO-Album-Sync registers the
  `npf71b963c1b7b6d119` scheme per user (Windows: under `HKCU\Software\Classes`)
  for the length of sign-in, then restores whatever was registered before. It
  accepts the `auth#`, `auth/#` and `auth/?` forms of the callback. If D3 brings in
  the desktop companions, that is the model for them. Android keeps the WebView
  interception, because a manifest intent filter cannot be added and removed
  temporarily.

### 7.6 Shell and UI (all three companions)

The user-facing contract goes into `companion-shell-layout.md` when the build
starts. This design defines it now so all three platforms agree.

- **A new dock destination, "Nintendo"** (Decision D4), holds the whole feature
  set as a tabbed page per the shell's "tabs, not scrolling" rule:
  - **Services:** the grid from `/v4/GameWebService/List`, with icon, name, and an
    "NSO" chip where membership is needed (`verifyMembership`, §6);
  - **Friends:** online first, then the rest; a friend's sheet has their current
    game, the note, favourite, remove and block; a request badge; Add friend (code,
    my QR, scan QR);
  - **Album:** the media grid, which may scroll like Amiibo's; filter by game and
    type; save, share, delete;
  - **Account:** profile, presence visibility, play history, announcements, Sign
    out.
  Before sign-in the page is the consent screen (§4), then sign-in (§7.5).
- **Play** keeps the "Wii U" path short. A **Services** row under the secondary
  actions shows the services the user opened recently (all of them on a wide
  window), so a service opens in one tap while the controller streams, without
  leaving Play. Before sign-in the row is a single "Nintendo Switch App" entry that
  goes to the Nintendo page. No service is hard-coded.
- **Opening a service** fills the page area under the top bar:
  - the navigation stays away (it is a slide-out; companion-shell-layout.md);
  - a slim service bar shows the service name, a sync state if the page exposes
    one, reload, and **Close**;
  - the top bar keeps the adapter state, so the console link stays visible.
- **Side by side.** On a wide window (≥ 960 dp wide and controller streaming), Play's
  status cards can sit in a narrow column beside the web service. That is a later
  refinement (X6). The first version is full page.
- **The Nintendo page's Account tab** also holds: the signed-in account (nickname,
  country), "What is shared" (re-opens the consent text), Sign out, and Remove from
  this device. Settings links to it rather than duplicating it.
- **Diagnostics** gains an NSO section:
  - state machine state;
  - f-API `/config` version vs ours;
  - counts of hash 1 and hash 2 calls in the last window;
  - last error per stage;
  - unknown bridge calls.
  It never shows tokens.

**Desktop.** WinUI 3 has WebView2, and Avalonia on Linux has no first-party
WebView; its options are WebKitGTK through a native control, or CEF. The input
architecture differs too: a desktop companion is not the controller. Android is the
platform where the idea makes sense, so the plan is:

- Android first;
- desktop as a separate phase with its own decision (§14 D3);
- the shell contract above applies to all three, so a desktop build only fills it
  in.

## 8. Game service specifics

### 8.1 ZELDA NOTES

- **Account.** E6 suggests the companion's NA user must be the one linked to the
  console user playing. The sign-in screen says so, and the panel shows the signed-in
  nickname.
- **Console online.** E6. The service is useless offline. We cannot detect that
  from the companion, so the help text says so.
- **Live location.** The page receives it itself, over an SSE stream from
  Nintendo's server (§8.1). **Strong Evidence** that we only need to keep the page
  alive:
  - the screen stays on while a service is open (`FLAG_KEEP_SCREEN_ON` on the
    window, scoped to the service);
  - the WebView is not paused while visible;
  - when the activity is backgrounded, the WebView pauses. This matches the
    official app ("it stops if the phone sleeps").
- **Voice guidance.** Audio comes from the page. It plays through the device's
  media stream, so the handheld's speaker or headphones are used, not the console.
  Autoplay without a gesture is U8 / X2.
- **amiibo bonus, Daily Bonus, Play Data.** Plain page features; no app support.
- **Photo Studio, Item and Autobuild Sharing.** Need `downloadImages`, share and a QR
  reader (U6). Phase 3.
- **Ocarina of Time** (E5). If it appears in the service list, it appears on Play
  with no extra code, which is why the list is never hard-coded.

#### How the live map works

Source: NSO-Album-Sync (E25), which uses this stream for Discord Rich Presence. Its
README credits Bentheminernz's "Zelda Notes RPC" reference work. Everything here is
**Strong Evidence**: a working implementation, but not reproduced by us.

1. **Session.** The client sends `GET https://api.lp1.87abc152.srv.nintendo.net/?lang=..&na_country=..&na_lang=..`
   with:
   - `x-gamewebtoken`, `x-appplatform: android`, `x-appcolorscheme`, `dnt: 1`;
   - `X-NACountry` and `X-Requested-With: com.nintendo.znca`;
   - `x-nso-internal-manual-redirect: 1`.
   The response sets the session cookie `a5_token`. NSO-Album-Sync assumes it lasts
   90 minutes; that is its own assumption, not a measured value.
2. **Stream.** The client opens `GET /continuous-connection/sse?gameId=<0 BotW | 1 TotK>&porterSessionId=<5 random alphanumerics>`
   with the cookie.
3. **Messages** are JSON with a `messageType`:
   - `open`: the client then calls the page's `sendMapSyncStartAction` with
     `[gameId, porterSessionId, "complete-guide"]`;
   - `map_sync_start_ack`;
   - `map_sync_player_info`: `playerPos` [x,y,z] and `playerFront` [x,y,z], plus
     `playerLayer` in TotK (ground / sky / underground).
   Any message can carry `needsAck` with a `messageRequestId`, which is
   acknowledged through an ack action. `gameSessionId` identifies the console
   session, and `sendMapSyncEndAction` ends it.
4. **The actions are Next.js server actions.** They are POSTs to the page URL with
   a `Next-Action: <hash>` header, `Next-Router-State-Tree` and `X-Deployment-Id`.
   NSO-Album-Sync finds the hashes at runtime by scanning the page's scripts,
   because they change with every Nintendo deployment.
5. **Game.** Which game is running comes from Coral presence title ids:
   `01007ef00011e000` is BotW and `0100f2c0115b6000` is TotK.

**Consequences for the design:**

- **The WebView approach needs none of this.** The page does it all. Our job is to
  keep the page loaded, visible and online, and to answer its bridge calls.
- **Every stage is a Nintendo web request,** so the live map depends only on the
  network. E7 is disproven.
- **"Every few seconds"** (E3) describes the rate of `map_sync_player_info`
  messages. Measuring the cadence is part of X1, done in DevTools.
- **Unknown U15:** can two map-sync sessions (ours, and the official app on another
  phone) run at once, or does a new `porterSessionId` displace the old one? This
  matters if the user also has the official app open.
- **A native map is possible but rejected for v1** (§12). The server-action hashes
  change with every deployment, so a native client has to re-discover them by
  scraping Nintendo's JavaScript. That is the most fragile part of
  NSO-Album-Sync's implementation.

### 8.2 SplatNet 3

Sources: nxapi `src/api/splatnet3.ts`, `src/app/preload-webservice/quirks/splatnet3.ts`;
Reef `game-webview.ts`; CoralWeb service policy. **Strong Evidence** from three
clients; not run by us.

- **Service.** Id `4834290508791808`. Host `https://api.lp1.av5ja.srv.nintendo.net`.
  NSO membership required (F).
- **Page tokens.** The page swaps the game web token for a **bullet token** itself,
  with `POST /api/bullet_tokens` carrying:
  - `X-GameWebToken`, `X-NACountry`;
  - `X-Web-View-Ver: <app_ver>`, which the page reads from its own config.
  The page then calls a GraphQL API with `authorization: Bearer <bullet token>`.
  - The bullet token lasts about 2 h. `x-bullettoken-remaining` below 300 s means
    renew.
  - A 401 from `bullet_tokens` means the web service token has expired. The page
    then asks the bridge with `requestGameWebToken`.
  - A 403 means `ERROR_OBSOLETE_VERSION`, which the page reports itself.
  - Reef attaches `x-gamewebtoken` only to `/api/bullet_tokens` and
    `/api/primer_tokens`. In a WebView the page sets the header itself, so we never
    add it to sub-requests.
- **Host work.** None beyond §6, because it all runs inside the page.
  - nxapi's quirks only hide a desktop exit button and fix scroll bars. Neither
    applies to a phone-sized WebView.
  - nxapi's "refresh" quirk calls the page's own pull-to-refresh. In a touch
    WebView the page's own pull-to-refresh already works.
- **Links out.** The SplatNet 3 tournament manager on `c.nintendo.com` is a separate
  site. CoralWeb opens it outside the WebView, and the whitelist rule (§6) does the
  same for us.
- **Second-screen value.** Ordering gear, which arrives in the game. Schedules and
  results, read while the console plays.

### 8.3 NookLink (Animal Crossing: New Horizons)

Sources: nxapi `src/api/nooklink.ts`, `docs/lib/nooklink.md`,
`quirks/nooklink.ts`; Reef; CoralWeb. **Strong Evidence**; not run by us.

- **Service.** Id `4953919198265344`. Host `https://web.sd.lp1.acbaa.srv.nintendo.net`.
  Whitelist includes `dpl.sd.lp1.acbaa.srv.nintendo.net`. NSO required (F).
- **Session.** The first document request, with `x-gamewebtoken`, returns a
  `_gtoken` cookie. Reef sends the token only on that initial document.
- **Users.** NookLink is per *island resident*: the page lists the residents linked
  to the account, then authenticates as one of them (an ACNH-level user token).
  The page does this itself.
  - API requests carry `X-Blanco-Version`, which the page knows.
- **"Type on the phone, it appears in the game."** The page's keyboard and reaction
  features post `POST /sd/v1/messages` with type `keyboard` or `emoticon`. They
  work only while the resident is online in the game. This is a page feature: the
  user types into the WebView's text field.
  - That makes **U9 (soft keyboard inside a page) a Phase-1 requirement**, not
    Phase 3. While a NookLink text field has focus, gamepad input must still go to
    Controller Link and only text goes to the field. Hardware-keyboard keys must
    not be swallowed by Keyboard & Mouse forwarding while a page field has focus.
    See X8.
- **Quirk.** "NookLink must start at the main page, otherwise it will fail to load"
  (nxapi). A reload must return to the initial URL, keeping only the deep-link query
  parameters. Our host's Reload button does that.
- **User agent: Unknown U16.** Reef sends an **iOS Safari UA** to NookLink and Smash
  World, and the Android shape to everything else. The reason is not documented.
  If the Android UA fails, X3 tries the iOS shape.

### 8.4 Smash World and SplatNet 2

- **Smash World** (Super Smash Bros. Ultimate).
  - Ids `5598642853249024`, plus `5614999764533248` (legacy).
  - Host `https://app.smashbros.nintendo.net`, plus `www-aaaba-lp1-hac.cdn.nintendo.net`
    for media.
  - NSO required (F).
  - Queued stage downloads are picked up by the game.
  - It uses the same "token until session cookie" rule as ZELDA NOTES (Reef), and
    the same iOS-UA unknown, U16.
  - Posting Smash screenshots to Smash World from the console's HOME Menu album has
    been withdrawn by Nintendo. The service itself is still listed (P, F).
- **SplatNet 2** (Splatoon 2).
  - Id `5741031244955648`. Host `https://app.splatoon2.nintendo.net`.
  - No NSO needed (F).
  - nxapi's quirks are desktop scroll fixes only.

## 9. Retroid Pocket 5 and other handhelds

- **Android 13 is above minSdk 28.** Every API above is available.
- **The WebView** comes from Android System WebView via the Play Store. **Unknown
  U11** whether the RP5's WebView updates when Google Play services are off (E22).
  Diagnostics shows the WebView package version.
- **Size.** 1080p on 5.5" is about 960×540 dp in landscape, the same size our
  layout lab already uses, so full-page hosting fits as is.
- **Network.** Wi-Fi only, and the console also needs internet (E6).

## 10. Risks

| Risk | Likelihood | Effect | Mitigation |
| --- | --- | --- | --- |
| Nintendo account action against users of third-party clients | Low per E20, but not zero and not in our control | The user loses access to online services | Consent text states it plainly. We call only endpoints the official app calls, only when the user acts, with the official app's cadence or slower (§4). Writes are limited to the user's own explicit actions that the official app offers: friend requests, favourites, notes, blocks, deleting captures, posting from game pages. There are no bulk or automated writes. |
| f-API outage or shutdown (E15) | Observed today | Sign-in and new tokens fail | First-class unavailable state. Running services keep working until their token expires. No retry storms. |
| Coral version gate | Certain over time (E8 vs E9) | Login rejected | Version read from `/config`. "Upgrade required" shown as such. |
| Bridge change in a new app version (U3) | Likely | One feature in a page breaks | Unknown-call counters. X3 re-run per app version. |
| Licence contamination | Avoidable | Repo licence breach | §5 rule. Review step in §13. |
| Nintendo Account agreement (E21) | Exists | Legal or policy exposure for the project | We do not reverse engineer Nintendo's app. We use a public third-party service under its terms and host Nintendo's pages unmodified. **Decision D1 is the user's.** |
| Token leak through logs | Avoidable | Account compromise | Logging redaction tests (§13 step 2). No tokens in Diagnostics or exports. |

## 11. Unknowns and experiments

Each experiment follows `CLAUDE.md`'s method and gets a record under
`docs/experiments/` when run. All of them run on a debug build with WebView remote
debugging, using the user's own account. None needs the adapter or the firmware.

| Id | Unknown | Experiment | Promotes / falsifies |
| --- | --- | --- | --- |
| X1 / U1, U15 | Mechanism largely answered by E25 (SSE). Still open: does our WebView-hosted page receive the stream, at what cadence, and what happens when the official app opens a second session? | Open ZELDA NOTES in the debug host. Run navigation with the console online, then offline, then with the official app opened on another phone. Record `/continuous-connection/sse` in Chrome DevTools. | Promotes E25 to Confirmed in our host. Gives the message cadence. Settles U15. |
| X2 / U8 | Does voice guidance play without a user gesture in our WebView? | X1 setup. Trigger a waypoint with the screen on and media gesture off/on. | Sets `mediaPlaybackRequiresUserGesture` |
| X3 / U3, U4, U13, U14 | Which headers, initial path, persistent-data handling and bridge calls does 3.6.0 need? (UA, U5, is now Strong Evidence; see §6.) | Compare the official app's WebView requests (a proxy on the user's own device, or `chrome://inspect` if the official app's WebView is debuggable; it may not be) with ours. Log every bridge call our host receives. | Header and UA set. Bridge table complete for 3.6.0. |
| X4 / U10 | Does accounts.nintendo.com allow sign-in inside a WebView? | Sign-in screen prototype in a debug build. | Keeps §7.5, or forces a Custom Tab design with a different redirect capture |
| X5 / U2 | Does ZELDA NOTES need the same NA user as the console? | Sign in with a second account that does not own the game. | Help text and error copy |
| X6 | Is the side-by-side layout usable at 960×540 dp? | Layout lab with a stub page | Ships or drops §7.6 side by side |
| — / U6 | QR reader contract | Read the bridge arguments when Item Sharing asks for it (X3 log) | Phase 3 scope |
| — / U7 | ~~nxapi-auth client registration process~~ **Resolved** by E28: self-service, public client | — | — |
| — / U12 | Which `X-znca-Client-Version` a newly registered client sends | Read it from the registration page when D2 is done | The header value |
| X7 / — | Do SplatNet 3, NookLink, Smash World and SplatNet 2 load in the generic host with no per-service code? | Open each one in the debug host with an NSO member account that has save data. Log bridge calls, cookies and failures. | Per-service quirks list (§8.2–8.4) Confirmed or extended |
| X8 / U9 | Text fields inside pages (NookLink keyboard, Photo Studio): soft keyboard, plus gamepad routing while a field has focus | Send a NookLink keyboard message while Controller Link streams. Check that the message appears in the game and the controller never drops. Repeat with a hardware keyboard while Keyboard & Mouse forwarding is on. | Focus rules in §7.2 |
| X9 / U16 | Do NookLink and Smash World need the iOS user agent? | X7 with the Android UA first, then the iOS shape | UA table in §6 |
| X10 / — | Do native Coral features (friends, album, requests, announcements, play log) work through `/encrypt-request` and `/decrypt-response`, and what does each cost in f-API calls? | A debug screen that runs one read of each endpoint and records calls and latency | Confirms the §4 cost rule. Response fixtures (redacted) for the unit tests. |
| — / U17 | Shapes of `/v5/Chat/*` (GameChat history, friend candidates) | X10 run against `/v5/Chat/List` | Promotes the GameChat row of §2A from Phase 3 to buildable |
| — / U11 | WebView updates on the RP5 without GMS | Read the WebView version in Diagnostics on the device | Minimum WebView note |

**Experiments are not implementation.** X1–X5 need a minimal debug-only host
(steps 1–4 of §13) and so cannot run before the build starts. They are the first
thing the build does, and the user-visible feature is built only after them.

## 12. Rejected alternatives (negative knowledge)

- **Split screen with the official app.** Attractive because it needs no sign-in
  code at all. Rejected because Android gives key and motion focus to one window
  only. Gamepad input would go to whichever app was last touched, so touching the
  map would steal the controller from Controller Link. This contradicts the core
  goal.
- **Running Nintendo's native library ourselves** (local `libvoipjni`, or Frida on
  the user's device). Removes the third-party dependency. Rejected: it means
  redistributing or instrumenting Nintendo's binary, which is circumvention under
  E21, and it needs root.
- **Porting nxapi.** Fastest path. Rejected because AGPL-3.0 conflicts with the
  repo's Apache-2.0 (§5).
- **Hard-coding ZELDA NOTES' URL.** Simplest UI. Rejected: the token is per service
  id, the URL can change, and the list gives OoT and future services for free.
- **Picture-in-picture of the official app.** Same focus problem as split screen,
  and the official app does not offer PiP for web services. The second point is
  unverified and moot given the first.
- **Desktop first.** Rejected because the second-screen idea depends on the screen
  being in the user's hands.
- **A native ZELDA NOTES map** drawn by us from the SSE stream (§8.1), instead of
  hosting Nintendo's page. Attractive: no WebView quirks, and our own UI. Rejected
  for v1 for three reasons:
  - it needs Nintendo's map and Complete Guide assets;
  - it needs the deployment-specific Next.js action hashes, scraped from Nintendo's
    scripts;
  - it rebuilds a page Nintendo already maintains.
  It remains a possible later "position only" readout, but is not planned.
- **"Zero-server" clients avoid the f-API.** This is **false**; recorded because the
  wording invites it.
  - CoralWeb's browser extension (`nso-extension-backend`) advertises a
    "zero-server backend". Its `src/nxapi/client.ts` hard-codes
    `https://nxapi-znca-api.fancy.org.uk`, and `manifest.json` requests both
    `fancy.org.uk` hosts.
  - "Zero-server" only means that the extension replaces Dycool's own Cloudflare
    relay, which the web page needs because of CORS.
  - Every client reviewed uses the public f-API: nxapi, NSO-Album-Sync, Reef,
    CoralWeb, and the extension.
- **Route B: use the installed Nintendo Switch App on the same phone** (proposed
  2026-11-07, **shelved as too risky**, not disproven).
  - The idea: make the feature an opt-in bonus that requires the official app to be
    installed. Our app would load the stamp library (`Libvoipjni`) from the user's
    own install and compute `f` locally.
  - Attractive because no third party sees tokens, nothing needs registering, and
    the version always matches.
  - Why the stamp service's method does not transfer: the f-API reaches into the
    running official app with Frida on **rooted** phones (E11), and Android's
    sandbox stops an unrooted app from doing that.
  - Loading the library into our own process might work, but it is unverified on
    four counts:
    - whether the library works outside its own app (package, signature or Java
      dependencies);
    - whether the request and response encryption since 3.0.1 is reachable at all;
    - whether device-integrity checks are involved;
    - how often Nintendo updates break it.
  - It is also the most direct form of the circumvention the Nintendo Account
    agreement forbids (E21), and it moves that exposure onto this project. The
    first step on revisit is a debug-only feasibility experiment on the target
    device: load, compute `f`, have it accepted, and reach the encryption.
- **Receiving Nintendo's push notifications** (friend online, friend requests,
  GameChat invites, voice chat Start Alerts) by registering with
  `/v1/Notification/RegisterDevice`. Attractive because the official app's alerts
  are among its most used features. Rejected because:
  - Nintendo's server sends through Firebase Cloud Messaging to the official app's
    Firebase project, and a token from our app's project cannot receive those
    messages;
  - the only workaround is to take Nintendo's Firebase sender identity from its APK
    and present it as ours, which is impersonation.
- **Imitating pushes by polling in the background.** Rejected: every poll costs two
  f-API calls (§4), and the terms allow automated requests only at a limited
  frequency after recent user interaction. What we do instead is keep the data fresh
  whenever the user looks.
- **Smart-device voice chat.** The official feature runs on Nintendo's native
  `libvoip`, the same binary that produces `f` (nxapi README). Rejected for the same
  reason as running that library ourselves (above). Switch 2 exclusives do not
  support it anyway; GameChat replaced it on the console (F).
- **Routing tokens through a relay server** (CoralWeb's "external server relay"
  fallback, or Reef's server-side proxy). Attractive on platforms where the
  browser's CORS blocks direct calls. Rejected because:
  - a second third party would see the tokens;
  - an Android WebView and a native HTTP client have no CORS problem;
  - our only third party stays the f-API that the terms already cover.

## 13. Implementation outline

Each step ends with green tests and a reviewable diff. Steps 1–5 build a
debug-only host for the experiments, and nothing reaches release builds until
step 9.

1. **Package skeleton and fixtures.** Create `nso/` with interfaces for an HTTP
   transport and a clock. Add redacted JSON fixtures, written by hand from the
   field lists in §3, of the users/me, Login, List and GetWebServiceToken shapes.
   *Test:* fixtures parse.
2. **`NaAuth`.** PKCE generation (RFC 7636 S256 vectors), authorize URL building,
   session_token and token exchanges, users/me.
   *Tests:* RFC vectors, URL building, parsing, and a **redaction test** proving
   that no token string reaches `Log` or diagnostics text.
3. **`FApiClient`.** nxapi-auth client credentials, `/config`, `/f`,
   `/encrypt-request`, `/decrypt-response`. `/f` uses `encrypt_token_request` (E29).
   Identity headers per E14. No retries. Every non-2xx is a typed failure per the
   E29 table. A `Retry-After` backoff is persisted. The `client_id` comes from
   build configuration.
   *Tests:* header presence, no-retry behaviour, every E29 error maps to its
   state, the persisted backoff survives a restart, and the nxapi-auth token is
   never written to storage.
4. **`CoralClient` and `NsoSession`.** The state machine of §7.3 with lifetimes.
   *Tests:* every transition, expiry with a fake clock, and that hash-1 and hash-2
   call counts stay within E13 over a simulated 8-hour session.
5. **`NsoStore`.** Keystore AES-GCM file in no-backup storage. Persistent data per
   (NSA id, service id). Sign-out erases everything.
   *Instrumented test:* round trip, wipe.
6. **Debug host.** `WebServiceHost` with the bridge table of §6, whitelist
   enforcement and the unknown-call counter, reachable from the debug layout lab
   only. **Run X1–X5 and write their records.** Update §6/§8 of this document with
   the results, and change classes where the evidence moves them.
7. **Consent and sign-in screens.** Consent text approved by the user (D1). Sign-in
   per X4's result.
   *Test:* sign-in is impossible without the consent flag, and withdrawing consent
   wipes the store.
8. **Shell integration, game services.** The Nintendo page (Services tab and
   Account tab), Play's Services row, full-page hosting, the service bar,
   keep-screen-on scoped to the service, and the Diagnostics section. **Run X7–X9**
   so every listed service, not only ZELDA NOTES, opens in the host, with text
   input working (NookLink). Update `companion-shell-layout.md` with the contract
   of §7.6.
   *Tests:* `ShellPresentation` strings for the new states. Layout lab screenshots
   at 960×540 and 411×914 dp.
9. **Release gating for services.** Licence review (no AGPL-derived code), the D2
   registration in place, and the user's hardware validation on the RP5 with a
   console:
   - ZELDA NOTES while Controller Link streams: the map follows Link, and the
     controller keeps driving the console;
   - one NSO service, if the user has a membership. NookLink is the best test: a
     typed message appears in the game.
   Only then: STATUS.md, README, CHANGELOG.
10. **Native features, Phase 2.** **Run X10 first** for redacted fixtures and the
    cost per call. Then, in this order:
    - **Friends tab:** list and presence, friend sheet, favourite, note, request
      list (accept, reject, cancel), add by friend code. Remove and block confirm
      first.
    - **Album tab:** list, thumbnails, view, save to MediaStore, share with the
      `/v5/Hashtag/List` prefill, delete from server (confirms first), filter and
      sort.
    - **Account tab:** profile, presence visibility, play history, announcements.
    *Tests:* each Coral call parses its fixture; destructive actions require
    confirmation; the cost rules of §4 (no background calls, a 60 s minimum
    auto-refresh, refresh only while visible) are unit-tested with a fake clock.
    *Validation:* the user sees their real friends, captures and requests.
11. **Phase 3, separately scoped:**
    - the QR reader (scan a friend's QR; ZELDA NOTES and SplatNet sharing);
    - image download and share for the page features;
    - GameChat history and friend candidates after U17;
    - the side-by-side layout (X6);
    - the desktop decision (D3).

**Acceptance for the feature** is the step-9 hardware check, done by the user. It
cannot be validated in software or on an emulator, because it needs a real console
running the game online.

## 14. Decisions needed before implementation

- **D1. Accept the policy position.** Do you accept a third-party f-API that sees
  the user's id_token, Coral token and Coral data, plus the Nintendo Account
  agreement exposure (E21), as a feature of this project? Do you approve the
  consent wording the user will see?
- **D2. Registration.** Registration is self-service (E28). It needs a person to
  register the PicoSwitch2 public client, and a contact the User-Agent and client
  page can show. Whose name and contact?
- **D3. Desktop.** Android only, or later desktop builds with WebView2 (Windows)
  and a chosen Linux web engine?
- **D4. Where it lives.** The recommendation is a sixth dock item, "Nintendo"
  (Services / Friends / Album / Account), plus the Services shortcut row on Play
  (§7.6). The alternative is to put everything under Play. That crowds the page the
  shell keeps short for streaming.
- **D5. Native scope.** Phase 2 (§13 step 10) brings friends, album and account
  natively. Do you want all of it, or the game services first and the native
  features only on request?
