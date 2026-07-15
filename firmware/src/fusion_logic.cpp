#include "fusion_logic.h"

namespace fusion {

namespace {

int countSonarHits(const SensorSnapshot& s, const Config& cfg) {
    int hits = 0;
    for (const auto& ch : s.sonar) {
        if (ch.baseline_cm - ch.distance_cm >= cfg.sonar_delta_cm) {
            hits++;
        }
    }
    return hits;
}

} // namespace

FusionResult evaluate(const SensorSnapshot& s, const Config& cfg) {
    const int sonar_hits = countSonarHits(s, cfg);
    const bool sound_hit = s.sound_level >= cfg.sound_threshold;

    // Weighted-evidence scoring: no single sensor can push the score past
    // ~55 on its own. This directly targets the "Reliability and False
    // Alarms" market challenge the team's own literature review identified
    // -- a single ultrasonic threshold trips on pets, curtains, staff.
    uint8_t score = 0;
    if (sonar_hits >= 1) score += 35;
    if (sonar_hits >= 2) score += 15;          // multiple angles agree -> stronger signal
    if (s.pir_motion)    score += 25;
    if (sound_hit)        score += 15;
    if (s.is_night && sonar_hits >= 1) score += 10; // context: off-hours weighs a hit more heavily
    if (score > 100) score = 100;

    Confidence conf = Confidence::LOW;
    const char* reason = "no_corroborating_evidence";

    if (score >= 80) {
        conf = Confidence::HIGH;
        reason = "sonar+motion+sound_agree";
    } else if (score >= cfg.alert_score_min) {
        conf = Confidence::MEDIUM;
        reason = sonar_hits >= 1 && s.pir_motion ? "sonar+motion_agree"
                 : sonar_hits >= 2               ? "multi_angle_sonar_agree"
                                                  : "single_signal_above_threshold";
    } else if (sonar_hits >= 1 || s.pir_motion || sound_hit) {
        reason = "single_weak_signal_suppressed";
    }

    const bool past_cooldown = s.ms_since_last_alert >= cfg.cooldown_ms;
    const bool should_alert = (score >= cfg.alert_score_min) && past_cooldown;

    if (score >= cfg.alert_score_min && !past_cooldown) {
        reason = "suppressed_by_cooldown";
    }

    return FusionResult{score, conf, should_alert, reason};
}

} // namespace fusion
