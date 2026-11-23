# Battery Guard, Listen Mode, DFS, Play Stats — Design

<p>
  <a href="2026-11-18-battery-listen-dfs-stats-design.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

Status: approved (all five decision points confirmed by the user on 2026-11-18).
Implementation order: F → D → B → C → E, each step gated by
`./tools/validate.sh --static` + `--firmware` and its own commit.

## Goals

Five approved changes to the GPWS alert-sounds app:

- **F. Micro-race cleanups** — sustain-based quiet detection in the idle
  screen-off path (250 ms grace, same constant semantics as the natural-end
  detection), so the sub-millisecond `s_active == false` handover gap between
  chained entries can never trip a spurious screen-off while playing. Also log
  the return value of both `esp_sleep_pd_config()` calls (non-fatal, logged and
  continued, matching the rest of the shutdown sequence).
- **D. DFS frequency scaling** — enable ESP-IDF PM DFS (`CONFIG_PM_ENABLE`,
  `CONFIG_FREERTOS_USE_TICKLESS_IDLE`, `CONFIG_PM_DFS_INIT_AUTO`). CPU domain
  drops 160→40 MHz when idle; APB stays 80 MHz so LEDC backlight, ADC buttons,
  I2S, and esp_timer are unaffected. Light sleep is rejected: buttons are
  ADC-shared on GPIO0 and cannot wake from light sleep.
- **B. Low-battery guard** — SOC ≤ 10 %: wake screen if off, show a
  non-interactive warning scrim (bell icon, "battery low" title, SOC %,
  "any key to continue" hint), playback continues. Once per discharge
  session, re-armed when SOC recovers to ≥ 15 %. SOC ≤ 5 %: set `s_sleep_due`
  (existing input_task pipeline executes the deep-sleep sequence, which saves
  play counts) even while playing or with popups open — cell protection
  outranks UX. SOC read failure keeps the last value; if never read
  successfully the guard stays inert.
- **C. Listen mode** — new settings item "listen" (visible only while
  autoplay is on, default off, NVS key `"lst"`). While playing with listen
  mode on, idle 60 s turns the backlight off but playback continues
  (listening in the dark). Paused playback is never protected (pause + dark
  screen would be a battery hole, matching `d7d9c30` semantics). Manual
  double-tap screen-off follows the same split. When autoplay is turned off,
  listen mode is force-cleared (no ghost NVS state for an invisible item).
  Deep sleep (5 min) still requires quiet/paused playback, so the end of an
  autoplay chain rolls into the deep sleep timeline without waiting another
  5 min.
- **E. Play stats popup** — settings row "stats" (always visible) opens a
  full-screen scrim + panel listing all 24 entries (name left, play count
  right), sorted by count descending, ties in catalog order. Snapshot on open
  (not a live dashboard). UP/DOWN scrolls one row per click (double = two,
  long-press = continuous via the existing repeat timer), OK closes, 15 s
  idle auto-close. Scroll is a y-offset clamp on the content box, mirroring
  the grid scroll model.

## Key decisions (all user-confirmed)

| # | Decision | Value |
| --- | --- | --- |
| 1 | Listen-mode screen-off delay | reuse `SCREEN_IDLE_OFF_US` (60 s), no new constant |
| 2 | Listen mode vs manual double-tap | backlight off, playback continues (protects playing only) |
| 3 | Stats sort | count descending, ties in catalog order |
| 4 | Battery thresholds | 10 % warn / 5 % force sleep / 15 % re-arm |
| 5 | Autoplay off | listen mode force-cleared and persisted |

## Architecture notes

- **No new tasks or timers.** Battery guard rides the existing 2 s SOC poll
  inside `update_battery()`; forced sleep reuses `s_sleep_due` + input_task;
  stats scrolling reuses the 120 ms repeat timer; listen mode is one bool in
  `app_audio` plus NVS.
- **`s_screen_off` narrows to "backlight off".** `screen_off()` stops playback
  only when not in the listening-protected state (playing + listen + not
  paused). Rendering keeps running (same as today) except the playback progress
  / wave refresh segment, which skips visual updates while the screen is off
  (invisible redraws wasted SPI bandwidth); SOC reads and seq-follow stay live.
- **Settings popup geometry is generalized** (step C) so hidden rows collapse
  instead of leaving gaps: up to 5 rows (volume / autoplay / random / listen /
  stats), panel grows, `modal_refresh()` stacks visible rows by y position.
  The stats popup (step E) reuses the same visual language.
- **`modal_ctl.h` stays pure/host-testable**: items gain `MODAL_ITEM_LISTEN`
  and `MODAL_ITEM_STATS`, row hiding generalizes from the random-row special
  case to skip-any-hidden-row navigation, and new actions
  `MODAL_LISTEN_ON/OFF`, `MODAL_STATS_OPEN` extend the matrix. Host tests
  cover the extended matrix including two-hidden-rows navigation.
- **NVS**: one new u8 key `"lst"`; writes still coalesce at popup close /
  screen-off / deep sleep — wear profile unchanged.

## Verification

- Host: extended `test_modal_ctl.c` matrix (listen/stats items, hidden-row
  navigation, stats-open action).
- Build: full static + firmware gate per step.
- Device (user-run, no serial device in the dev environment): idle screen-off
  during autoplay handovers; DFS sanity for animation/keys/audio; battery
  threshold behavior (temporarily raise thresholds or wait for a natural
  discharge); listen-mode screen-off-while-playing and end-of-chain deep sleep;
  stats popup scroll/sort/auto-close.
