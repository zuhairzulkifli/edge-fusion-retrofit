// Standalone test harness for fusion_logic.
// Compile with a plain desktop compiler, no Arduino toolchain needed:
//   g++ -std=c++17 -I../firmware/include test_fusion_logic.cpp ../firmware/src/fusion_logic.cpp -o test_fusion_logic
//   ./test_fusion_logic
//
// This is the concrete proof-of-work artifact for the case study: the fusion
// logic that will run on the ESP32 is exercised here, on a desktop, with
// zero hardware in the loop.

#include "fusion_logic.h"
#include <cstdio>
#include <cstdlib>

using fusion::Confidence;
using fusion::evaluate;
using fusion::SensorSnapshot;

static int g_failures = 0;

void check(bool cond, const char* test_name) {
    if (cond) {
        std::printf("  [PASS] %s\n", test_name);
    } else {
        std::printf("  [FAIL] %s\n", test_name);
        g_failures++;
    }
}

// Convenience builder: all sensors "quiet" by default.
SensorSnapshot quietRoom() {
    SensorSnapshot s{};
    for (auto& ch : s.sonar) { ch.distance_cm = 200.0f; ch.baseline_cm = 200.0f; }
    s.pir_motion = false;
    s.sound_level = 0.05f;
    s.is_night = false;
    s.ms_since_last_alert = 999999;
    return s;
}

int main() {
    std::printf("fusion_logic test suite\n");
    std::printf("========================\n");

    // 1. Empty room: nothing should fire.
    {
        auto r = evaluate(quietRoom());
        check(!r.should_alert, "empty room does not alert");
        check(r.confidence == Confidence::LOW, "empty room confidence is LOW");
    }

    // 2. Single sonar hit only (e.g. a cat crossing one beam, daytime):
    //    this is the exact false-positive scenario the literature review
    //    flagged. It must NOT alert on its own.
    {
        auto s = quietRoom();
        s.sonar[0].distance_cm = 150.0f; // baseline 200 -> delta 50cm, a hit
        auto r = evaluate(s);
        check(!r.should_alert, "single daytime sonar hit alone is suppressed");
    }

    // 3. Sonar hit + PIR motion agree: should cross the alert threshold.
    {
        auto s = quietRoom();
        s.sonar[0].distance_cm = 150.0f;
        s.pir_motion = true;
        auto r = evaluate(s);
        check(r.should_alert, "sonar + PIR agreement alerts");
        check(r.confidence != Confidence::LOW, "sonar + PIR is not LOW confidence");
    }

    // 4. All signals agree: sonar (2 angles) + PIR + sound -> HIGH confidence.
    {
        auto s = quietRoom();
        s.sonar[0].distance_cm = 140.0f;
        s.sonar[1].distance_cm = 160.0f;
        s.pir_motion = true;
        s.sound_level = 0.9f;
        auto r = evaluate(s);
        check(r.should_alert, "full sensor agreement alerts");
        check(r.confidence == Confidence::HIGH, "full sensor agreement is HIGH confidence");
        check(r.score >= 80, "full sensor agreement scores >= 80");
    }

    // 5. Cooldown: an otherwise-alerting event inside the debounce window
    //    must be suppressed, but still scored/logged (so the triage agent
    //    still sees it happened).
    {
        auto s = quietRoom();
        s.sonar[0].distance_cm = 140.0f;
        s.pir_motion = true;
        s.ms_since_last_alert = 2000; // well inside the 15000ms cooldown
        auto r = evaluate(s);
        check(!r.should_alert, "event inside cooldown window is suppressed");
        check(r.score >= 60, "suppressed event still carries its real score");
    }

    std::printf("========================\n");
    if (g_failures == 0) {
        std::printf("ALL TESTS PASSED\n");
    } else {
        std::printf("%d TEST(S) FAILED\n", g_failures);
    }
    return g_failures == 0 ? 0 : 1;
}
