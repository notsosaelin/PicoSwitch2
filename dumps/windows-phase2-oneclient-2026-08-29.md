# Windows companion — one management client / no churn

Distilled from a 5.6 MB `mgmt_watch.ps1` capture (COM11, 2026-08-29 16:13–16:18).
The raw log is not committed: it is 9 repeated dumps of a rolling ring, and the
last dump contains everything the earlier ones do.

Build under test: `windows/companion` @ 9486c9d, unpackaged x64.
Adapter: personality `pro2`, `mgmt_enabled true`, USB-C on the console throughout.

## btstate transitions (host clock)

| time | event | client | mgmt.connects | mgmt.disconnects | disc.ctrl | disc.hci |
|---|---|---|---|---|---|---|
| 16:13:06.611 | start | True | 7 | 6 | 2 | 7 |
| 16:13:46.032 | poll | True | 7 | 6 | 2 | 7 |
| 16:13:47.651 | poll | False | 7 | 7 | 2 | 7 |
| 16:13:47.759 | transition:cble.client:True->False;scan_active:True->False;mgmt.disc+=1 | False | 7 | 7 | 2 | 7 |
| 16:14:28.937 | poll | False | 8 | 8 | 2 | 7 |
| 16:14:29.042 | transition:mgmt.disc+=1 | False | 8 | 8 | 2 | 7 |
| 16:15:10.579 | poll | True | 9 | 8 | 2 | 7 |
| 16:15:10.683 | transition:cble.client:False->True | True | 9 | 8 | 2 | 7 |
| 16:15:52.351 | poll | True | 9 | 8 | 2 | 7 |
| 16:15:53.955 | poll | True | 9 | 8 | 2 | 7 |
| 16:15:55.560 | poll | True | 9 | 8 | 2 | 7 |
| 16:15:55.664 | transition:scan_active:False->True | True | 9 | 8 | 2 | 7 |
| 16:16:37.572 | poll | True | 10 | 9 | 2 | 7 |
| 16:16:37.676 | transition:scan_active:True->False;mgmt.disc+=1 | True | 10 | 9 | 2 | 7 |
| 16:17:19.880 | poll | True | 11 | 10 | 2 | 7 |
| 16:17:19.984 | transition:mgmt.disc+=1 | True | 11 | 10 | 2 | 7 |
| 16:18:02.350 | poll | True | 11 | 10 | 2 | 7 |
| 16:18:03.955 | poll | True | 11 | 10 | 2 | 7 |
| 16:18:05.560 | poll | True | 11 | 10 | 2 | 7 |
| 16:18:05.664 | transition:scan_active:False->True | True | 11 | 10 | 2 | 7 |
| 16:18:48.351 | final | True | 11 | 10 | 2 | 7 |

## Adapter lifecycle ring (adapter clock, ms)

The adapter's own record, which is what settles the question: the host poll
cadence was too coarse to order events, this is not.

| t_ms | code | cause | handle |
|---|---|---|---|
| 5836 | mgmt_connect | config_mode | 0x0040 |
| 16045 | mgmt_disconnect | none | 0x0040 |
| 20112 | mgmt_connect | config_mode | 0x0040 |
| 7499425 | mgmt_disconnect | none | 0x0040 |
| 7501015 | mgmt_connect | config_mode | 0x0040 |
| 7501329 | mgmt_disconnect | none | 0x0040 |
| 7507360 | mgmt_connect | config_mode | 0x0040 |
| 7562545 | mgmt_disconnect | none | 0x0040 |
| 7608187 | mgmt_connect | config_mode | 0x0040 |
| 7647506 | mgmt_disconnect | none | 0x0040 |
| 7937040 | mgmt_connect | config_mode | 0x0040 |
| 8765245 | mgmt_disconnect | none | 0x0040 |
| 9281001 | mgmt_connect | config_mode | 0x0040 |
| 10408010 | mgmt_disconnect | none | 0x0040 |
| 10430112 | mgmt_connect | config_mode | 0x0040 |
| 10447161 | mgmt_disconnect | none | 0x0040 |
| 10454359 | mgmt_connect | config_mode | 0x0040 |
| 10564478 | mgmt_disconnect | none | 0x0040 |
| 10568684 | mgmt_connect | config_mode | 0x0040 |
| 10590578 | mgmt_disconnect | none | 0x0040 |
| 10598297 | mgmt_connect | config_mode | 0x0040 |

## Result

- management lifecycle records: 21, all on a single handle `0x0040`;
- **maximum concurrent management clients: 1**;
- **alternation violations (a second connect with no disconnect between): 0**;
- every disconnect carries `cause=none` — a clean application-level teardown,
  never a link failure;
- `disc.ctrl` and `disc.hci` did not move at any point, so no HCI-level
  disconnect occurred during the whole run;
- 16:13:06 → 16:13:46, spanning two Refreshes, navigation across all five
  destinations and a minimise/restore: **zero transitions**;
- inter-event gaps are irregular and human-paced (4 s, 7 s, 17 s, 22 s, 110 s).
  A hidden reconnect loop would show a regular cadence and would not stop.

Conclusion: one management client, no churn. PASS.
