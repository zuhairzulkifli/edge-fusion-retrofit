#pragma once
#include <cstdint>

// ---------------------------------------------------------------------------
// fusion_logic
//
// This module is the "edge AI" of the retrofit. It is deliberately NOT a
// trained ML model. Given the 3-5 day window and no labelled field data from
// the original robot, a trained TinyML classifier would be fabricated
// confidence dressed up as machine learning. Instead this is a small,
// explainable, weighted-evidence scorer: multiple independent sensors must
// agree before a HIGH confidence alert fires, which is the same idea a
// classifier would learn, but auditable and unit-testable on day one.
//
// It has ZERO hardware dependencies (no Arduino.h, no Wire.h) so it can be
// compiled and tested on a desktop with a plain C++ compiler -- see
// tests/test_fusion_logic.cpp. main.cpp is the only file that touches
// hardware; it just calls evaluate() with real sensor readings.
//
// Upgrade path: swap evaluate()'s body for a TensorFlow Lite Micro /
// Edge Impulse inference call once real labelled event data has been
// collected from a deployed unit -- the SensorSnapshot struct is already
// shaped like a feature vector.
// ---------------------------------------------------------------------------

namespace fusion {

// One fixed-angle ultrasonic reading plus its learned "empty room" baseline.
// Using a per-sensor baseline instead of a single global threshold (the
// original system's `distance < 30cm` rule) is what lets the system
// distinguish "someone walked past 40cm away" from "the desk that has
// always been 40cm away".
struct SonarChannel {
    float distance_cm;
    float baseline_cm;
};

struct SensorSnapshot {
    SonarChannel sonar[3];       // 3 fixed-angle sensors, replaces the servo sweep
    bool     pir_motion;         // fast PIR pre-trigger, solves "can't detect fast objects"
    float    sound_level;        // normalized 0.0 - 1.0
    bool     is_night;           // occupancy/context signal
    uint32_t ms_since_last_alert;
};

enum class Confidence : uint8_t { LOW = 0, MEDIUM = 1, HIGH = 2 };

struct FusionResult {
    uint8_t     score;        // 0-100, logged as-is even when suppressed
    Confidence  confidence;
    bool        should_alert; // false if below threshold OR inside cooldown
    const char* reason;       // short machine-readable string for logs / the triage agent
};

// Tunable thresholds, pulled out as named constants so they can be re-tuned
// after real deployment without touching the scoring logic itself.
struct Config {
    float    sonar_delta_cm      = 15.0f;   // "closer than baseline by this much" = a hit
    float    sound_threshold     = 0.55f;
    uint8_t  alert_score_min     = 60;      // score >= this => should_alert (subject to cooldown)
    uint32_t cooldown_ms         = 15000;   // debounce repeated triggers from one event
};

FusionResult evaluate(const SensorSnapshot& snapshot, const Config& cfg = Config{});

} // namespace fusion
