# App Extensions Design — Main Menu / Sound Quiz / GPWS Alarm / Hardware Diagnostics / Scene Playback

- Date: 2026-11-20
- Status: pending final user review
- Source: brainstorming session (all decisions individually selected by the user)
- Pre-implementation: each batch still gets its own plan for approval, per repo convention
- Chinese peer: [2026-11-20-app-extensions-design.zh_CN.md](2026-11-20-app-extensions-design.zh_CN.md)
  (product UI copy and quiz/scene copy are Simplified Chinese; the Chinese peer holds the authoritative copy)

## 1. Background and Goals

Current state: 24-entry PCM aviation-alert catalog with a two-column grid,
three-button input, a 5-row (full) settings popup, and idle screen-off /
deep sleep. This extension adds four features on one shared entry base,
reusing existing architecture assets (stats scroll-list layout, modal edit
pattern, autoplay visual follow, deep-sleep sequence, NVS persistence). New
pure-logic modules keep the host-testable discipline (menu_ctl, quiz_ctl,
scenes, boot ring log).

Estimated size: ~1300 lines (excluding tests), implemented in 5 batches
(section 9).

## 2. Main Menu (entry base)

**menu_ctl.h pure state machine** (~50 lines + host tests): state =
{open, item}; event matrix = UP/DOWN CLICK moves, OK CLICK enters (returns
an enter action), OK LONG closes, RELEASE passes through (no repeat-adjust
semantics). Six fixed rows, no hidden/editing state — modal_ctl is not
reused (its OK-toggles-editing semantics do not match).

**UI**: stats-popup row style; 6×40+57 = 297 px fits one screen (viewport
304) — no scrolling needed. Rows:

| Row | Action |
| --- | --- |
| Sound Quiz | open quiz mode-select page |
| Scene Playback | open scene list page |
| Alarm | open alarm setup page |
| Play Stats | open the existing stats popup (moved out of settings) |
| Settings | open the existing settings popup |
| Hardware Diagnostics | open the diagnostics page |

**Interaction changes**:

- OK long-press on the main screen: opens the settings popup → **opens the
  main menu**.
- Settings popup returns to a pure 4 rows (volume / autoplay / random /
  listen), height 4×40+57 = 217 px; the stats row and MODAL_ITEM_STATS are
  removed (matrix and tests updated in sync).
- Main-menu interaction mirrors the settings popup: UP/DOWN select, OK
  enter, OK long-press exit, 15 s idle auto-close, stop key-repeat on open
  (the modal_open semantics from 37b7ecd).
- Popup mutual exclusion chain unchanged: one popup at a time (main menu /
  settings / stats / diagnostics / quiz pages / alarm page / scene page).

## 3. Sound Quiz

### 3.1 Modes and flow

Main menu → mode-select page (2 rows: **10-question challenge** /
**endless mode**) → play → summary (challenge) or game-over summary
(endless). OK long-press exits to the main menu at any time.

### 3.2 Question generation (quiz_ctl.h, host-test core)

- Question type 8:2 random — **sound questions** (play an alert, pick its
  name from three options) / **scenario questions** (show a scenario
  sentence, pick the matching alert name; no sound plays).
- Generation: random correct entry + 2 distractors. **Distractors prefer
  the same en category** {PULL-UP group (whoop pull up / Boeing pull up /
  pull up), RETARD group (retard loop / retard), STALL group (stall
  warning / Airbus stall), TRAFFIC group (traffic / Airbus traffic)}: with
  ≥3 entries in a category, all three options come from the category
  (hard); otherwise 1 same-category + 1 catalog-wide random (avoiding
  same-en name collisions as distractors).
- Option order is randomized. Two consecutive questions never repeat the
  same entry (endless mode shuffles a 24-entry ring).
- Judging: correct → streak+1, next question after 800 ms; wrong → streak
  resets, red flash + play the correct answer's sound, next question after
  1.5 s (challenge) or summary (endless).
- Sound questions auto-play once on show; no replay gesture (YAGNI: clips
  run 1.2–10 s, one pass is enough, wrong answers replay the correct one).

### 3.3 Scenario copy table (24 entries; static data shipped with quiz_ctl)

Product copy is Simplified Chinese; the table below lists the entry (en
name) with the English sense of each sentence. The Chinese peer document
holds the authoritative product copy.

| idx | Entry (en) | Scenario sentence (English sense) |
| --- | --- | --- |
| 0 | WHOOP PULL UP | after takeoff the aircraft sinks instead of climbing; the most urgent warnings blur together |
| 1 | TERRAIN | terrain ahead closes in; the radar repeats one word over and over |
| 2 | B777 ALERTS | a Boeing flight-deck alarm compilation |
| 3 | STALL | the wing stalls; rapid continuous warning tones |
| 4 | TERRAIN PULL UP | EGPWS: "TERRAIN TERRAIN PULL UP" |
| 5 | WINDSHEAR | radar detects wind shear during the climb-out |
| 6 | TRAFFIC | TCAS reports nearby traffic |
| 7 | RETARD LOOP | flare and touchdown; the retard calls come in a burst |
| 8 | AP DISCONNECT | the Airbus AP1 disconnect chime |
| 9 | SINK RATE | close to the ground and still descending too fast |
| 10 | MASTER CAUTION | the single tone when the master caution light comes on |
| 11 | AIRBUS GPWS | an Airbus radio-altitude callout compilation |
| 12 | PTU BARK | power-on self-test on the ground; a "barking dog" in the bay |
| 13 | RETARD | the single retard callout during landing |
| 14 | MARKER | the audio tone crossing a marker beacon |
| 15 | STALL (Airbus) | a different take on the stall warning |
| 16 | PULL UP (Boeing) | the Boeing continuous "PULL UP" |
| 17 | TRAFFIC (Airbus) | the Airbus TCAS traffic advisory |
| 18 | MINIMUMS | decision altitude reached; the captain calls the word |
| 19 | MAYDAY | a mayday call on the radio |
| 20 | TERRAIN ALERT | the terrain-attention chime sounds |
| 21 | DING DONG | the flight-attendant call chime |
| 22 | PULL UP | the shortest, most urgent pull-up warning |
| 23 | CLIMB | TCAS issues a climb instruction |

### 3.4 UI and interaction

Full-screen scrim popup (modal styling): question area (sound question:
question counter + wave icon; scenario question: the sentence) + three
option rows + a bottom hint. **Three keys map directly to three options in
one step** (UP = top / OK = middle / DOWN = bottom) — zero navigation
state. Correct: option flashes green for 800 ms; wrong: picked option
flashes red and the correct one lights green.

### 3.5 Scoring and records

- Challenge: summary after 10 questions (correct count / longest streak);
  endless: game over on first miss, streak is the score.
- NVS: `quiz_best_round` (challenge best correct count), `quiz_best_streak`
  (endless best streak); the summary shows the best mark and flags a new
  record.

## 4. GPWS Alarm

### 4.1 Setup page (main menu → Alarm)

Modal edit pattern, two rows:

- **Duration**: 1–120 minutes, default 5; click ±1, long-press continuous
  adjust (reuses the volume repeat infrastructure).
- **Track**: UP/DOWN cycles the 24 entries showing the name, default 0
  (WHOOP PULL UP).

OK confirms → a one-second confirmation screen (alarm set, N minutes) →
shutdown animation → deep sleep.

### 4.2 Deep sleep and wake

- app_sleep extension: alarm sleep shares the idle-sleep sequence, but arms
  `esp_sleep_enable_timer_wakeup(N minutes)` **plus** GPIO wake.
- boot_diag records the wake cause (wc uint8 natively covers TIMER=4); a
  new site value marks the alarm arming.
- **GPIO wake (button) = cancel the alarm**: the existing normal wake path
  (key_guard arms as usual).
- **TIMER wake = ring**: new wake branch; key_guard not armed (no key held).

### 4.3 Ringing state

- Screen on with a ring page: track name + wave animation + an
  any-key-to-stop hint.
- Audio layer gains a loop bit on the play command (**single-track loop**,
  ~20 lines): auto-replays at end.
- Any key → stop looping → back to the main UI (normal lit state).
- Low-battery backstop: lowbatt_poll runs as usual while ringing (≤5%
  force sleep, like a real alarm clock); idle screen-off / deep-sleep
  timing does not apply to the ring page (a ringing session is active).

## 5. Hardware Diagnostics

Stats-popup-style read-only scroll list (snapshot on open, no live
polling), UP/DOWN scroll, OK back.

### 5.1 Reboot-history ring log (8 entries)

boot_diag rework: NVS blob `hist` = u8 write pointer + 8 × {rr, wc, site,
run_min} (33 B), one append per boot; old keys rr/wc/ps/rs are dropped.
The page lists 8 entries newest-first (empty slots render as an em dash),
each formatted as `POWERON wake=0 site=0 run=12m`.

### 5.2 Battery and I2C snapshot

- One read on open: voltage mV / SOC% / CW2017 version (bsp_battery gains a
  diagnostics-snapshot API, ~30 lines).
- I2C device table: shows the **cold-boot scan cache** (bsp_i2c caches the
  result; zero open latency; no in-page live scan — worst case 5.6 s would
  stall the UI). If the cold-boot scan bailed early (stuck bus), the page
  shows a scan-aborted note.

## 6. Scene Playback

### 6.1 Scene table (scenes.h static data, host-testable)

Scene names are Simplified Chinese product copy (see the Chinese peer);
English senses: Full Takeoff / Approach and Landing / Windshear Escape /
Emergency Descent / Boeing Full Warnings / Airbus Full Set.

| Scene (en sense) | Sequence (idx) | Storyline |
| --- | --- | --- |
| Full Takeoff | 21, 23, 0, 20 | cabin door chime → TCAS climb → pull up off the ground → keep terrain clearance |
| Approach and Landing | 21, 11, 9, 13, 7, 12 | door chime → altitude callouts → sink rate → retard → flare retard loop → PTU "bark" after touchdown |
| Windshear Escape | 5, 16, 3, 0 | windshear alert → pull up → near stall → go-around pull up |
| Emergency Descent | 10, 1, 19 | master caution → terrain warning → radio mayday |
| Boeing Full Warnings | 2, 18, 16 | alerts pack → minimums → continuous pull up |
| Airbus Full Set | 8, 17, 15, 11 | AP disconnect → traffic conflict → stall → altitude callouts |

### 6.2 Playback mechanics

- Main menu → scene list page (row = scene name + entry count, stats
  layout) → OK starts → popups close, back to the grid, **reusing the
  autoplay visual follow** (ui_focus_jump / seq) → **auto-stop at the end**
  (no loop at sequence end).
- Audio layer gains a scene-sequence source (~40 lines): play next entry
  on completion; independent of the autoplay switch (a separate source,
  does not touch autoplay settings).
- Existing interactions all keep working while a scene plays: OK click =
  pause/resume; OK double-tap = screen off (listen mode + scene = dark
  screen sequential playback, natural semantics); switching/stopping
  unchanged.

## 7. Cross-feature boundaries

- Every new page enters the `app_ui_input` consumption chain; the
  one-popup-at-a-time rule is unchanged.
- Alarm sleep and idle sleep are mutually exclusive (whichever comes
  first): confirming the alarm goes straight to deep sleep (timer+GPIO
  armed) — the alarm state has no lit-idle timing; conversely idle sleep
  arms GPIO only and keeps no alarm — an alarm is valid only after
  confirmation and is cleared on the next wake (restart semantics, per the
  app_sleep.h convention).
- Scene playback and quiz playback are mutually exclusive (one track /
  one sequence at a time); entering the quiz stops a scene and vice versa.
- The main menu stops key-repeat on open (37b7ecd semantics); every new
  page stops it symmetrically on open/close.
- Counting: quiz playback and scene playback **count into** play stats
  (same app_audio command channel, identical counting semantics).

## 8. Test strategy

| Module | Host test points |
| --- | --- |
| menu_ctl.h | full key matrix (move/enter/cancel/RELEASE pass-through/closed swallow) |
| quiz_ctl.h | generation distribution (same-category distractors) / no-repeat ring shuffle / judging and streak / challenge round count / endless termination / summary data |
| boot ring log | 8-entry wrap / pointer advance / empty slots / first boot after upgrade (old keys dropped) |
| scenes.h | sequence indices legal (0..23) / entry counts / static table integrity |
| audio loop/scene | command matrix: loop bit / sequence advance / sequence-end stop / orthogonal to autoplay |
| existing modal_ctl | matrix regression after the stats row removal |

## 9. Implementation batches

1. **Main-menu base**: menu_ctl + menu UI + OK long-press rerouting + stats
   migration + settings back to 4 rows.
2. **Hardware diagnostics** (serves the ongoing battery investigation):
   ring log + diagnostics-snapshot API + page.
3. **Sound Quiz**: quiz_ctl + copy table + mode page + play page + summary
   + NVS records.
4. **Scene playback**: scenes.h + audio sequence source + scene list page.
5. **GPWS alarm**: alarm page + dual-wake deep sleep + TIMER-wake ringing
   state + single-track loop.

Each batch independently passes the static gate + firmware build + on-device
verification + its own commit.
