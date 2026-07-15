# Edge-Fusion Retrofit

**Liminal Apprentice Case Assessment, Embedded Track.** I took a real project my team already shipped, found the three things that actually broke, and fixed them with AI doing real work at every stage, not just autocomplete.

Full six-question write-up is in [`CASE_STUDY.md`](./CASE_STUDY.md) if you want the long version. This README is the short one.

## The project

Our team built an [Intruder Detection Robot](./original-deck.pdf) for our IoT module: Arduino Uno, a servo-swept ultrasonic sensor, GPS, a buzzer, and two separate alert paths (Bluetooth app + a Google Sheet). It worked, in the demo-day sense. It also had three real problems, and we know exactly what they were because we measured them ourselves:

- **Servo brownout.** Wire up 3 servos and only one runs. Wire up 2 and they stutter. The team's own finding: a servo needs 4.8-6V, and the shared 5V rail couldn't feed more than one at a time.
- **A GPS module that was basically decorative.** Needed a clear outdoor sky view and several minutes to get a fix. On an indoor security node.
- **A detection rule with one job and a lot of false alarms.** Distance under 30cm, trigger. Our own literature review flagged "reliability and false alarms" as the whole industry's problem, and we shipped the simplest version of it anyway.

## The retrofit

Three changes, each aimed at one of those, nothing speculative:

1. Move servo power off the logic rail: a PCA9685 driver plus a dedicated 5V/3A supply. This is the actual fix for the actual brownout, not a workaround.
2. Replace one sweeping sensor with three fixed ones plus a PIR pre-trigger, scored together. Multiple signals have to agree before anything alerts, so it's explainable, not a black box, and it directly answers the false-alarm problem instead of hand-waving at it.
3. One authenticated MQTT path instead of two disconnected ones, with an AI agent on the cloud side that turns a night of raw events into three sentences a human would actually read.

## Proof, not promises

Two things here are not aspirational. I built and ran both.

**The fusion logic** (`firmware/`) has zero hardware dependencies by design, so it's fully testable on a laptop:

```
$ g++ -std=c++17 -Ifirmware/include tests/test_fusion_logic.cpp firmware/src/fusion_logic.cpp -o test_fusion_logic
$ ./test_fusion_logic
  [PASS] empty room does not alert
  [PASS] single daytime sonar hit alone is suppressed
  [PASS] sonar + PIR agreement alerts
  [PASS] full sensor agreement is HIGH confidence
  [PASS] event inside cooldown window is suppressed
  ...
ALL TESTS PASSED
```

**The triage agent** (`ai_agent/`) reads a night's worth of scored events and writes the briefing:

```
$ python3 ai_agent/triage_agent.py ai_agent/sample_events.json --offline
1. [ALERTED] 01:42:11 -> 01:42:27  (peak HIGH, score 90, 4 readings)
   Multiple independent sensors agreed - treat as a likely real event.
3. [suppressed] 03:05:40             (peak LOW, score 45, 1 reading)
   A single signal crossed threshold - plausible false positive (pet, curtain, staff).
ACTION: 2 incident(s) need human review, highest priority first.
```

## What I didn't fake

The ESP32 integration sketch (`firmware/src/main.cpp`) is written and reviewed, but not flashed to real hardware. I didn't have the retrofit parts (PCA9685, second regulator, PIR sensor) in hand this week. I'd rather tell you that directly than dress up an untested sketch as a finished build. Same principle the original team used when they admitted the GPS module basically didn't work.

Other things AI didn't solve for me: the fusion thresholds are principled guesses, not tuned values; the servo power math is datasheet reasoning, not a multimeter reading; there's no trained ML model here on purpose, because faking one without real field data would be worse than not having one.

## Run it yourself

```bash
# Fusion logic unit tests (no hardware, no dependencies)
g++ -std=c++17 -Ifirmware/include tests/test_fusion_logic.cpp firmware/src/fusion_logic.cpp -o test_fusion_logic
./test_fusion_logic

# AI triage agent (offline demo mode, no API key needed)
python3 ai_agent/triage_agent.py ai_agent/sample_events.json --offline
```

## What's in here

```
CASE_STUDY.md              the full six-question write-up
firmware/                  ESP32 sketch + the hardware-free fusion/scoring logic
tests/                     unit tests for the fusion logic (runs today, see above)
ai_agent/                  the AI triage agent + sample event log (runs today, see above)
diagrams/                  before/after architecture diagrams
```
