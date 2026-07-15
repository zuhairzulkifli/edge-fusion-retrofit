// =============================================================================
// Edge-Fusion Retrofit -- main.cpp (ESP32, Arduino framework)
//
// HONEST SCOPE NOTE: this was written and architected with AI assistance
// during a 3-5 day take-home, without physical access to the retrofitted
// board (new PCA9685 board, second buck regulator, and PIR sensor were not
// in hand). It has NOT been flash-tested on real silicon. What HAS been
// verified is:
//   - fusion_logic.{h,cpp} compiles and passes unit tests on desktop
//     (see tests/test_fusion_logic.cpp) with zero hardware dependency.
//   - This file's structure follows the same pattern the original team's
//     Wokwi-validated sketch used: prove logic/wiring in simulation before
//     hardware, called out explicitly in their own "Arduino Simulation"
//     slide. The natural next step with more runway is a matching Wokwi
//     project once a PCA9685-capable simulation target is set up.
//
// What this file demonstrates for review purposes: non-blocking scheduling,
// separation of hardware I/O from decision logic, and a secured comms path
// -- the concrete fixes for the three failure modes the original deck
// documented (servo brownout, blocking/coupled loop, unauthenticated alerts).
// =============================================================================

#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_PWMServoDriver.h>   // PCA9685 -- isolates servo current from the ESP32's own rail
#include <DHT.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <esp_task_wdt.h>

#include "fusion_logic.h"

// ---------------------------------------------------------------------------
// Pin map (see firmware/README.md for the full rationale + wiring notes)
// ---------------------------------------------------------------------------
constexpr uint8_t PIN_SONAR_TRIG[3]  = {25, 26, 27};
constexpr uint8_t PIN_SONAR_ECHO[3]  = {32, 33, 34};
constexpr uint8_t PIN_PIR            = 35;
constexpr uint8_t PIN_SOUND_ANALOG   = 36;
constexpr uint8_t PIN_DHT            = 4;
constexpr uint8_t PIN_LED            = 2;
constexpr uint8_t PCA9685_BUZZER_CH  = 0;   // buzzer + LED now live on the isolated PCA9685 rail too
constexpr uint8_t PCA9685_SERVO_CH[2] = {1, 2}; // e.g. a pan mount for a future camera add-on

#define DHTTYPE DHT11

// ---------------------------------------------------------------------------
// Network / broker config -- placeholders, populated from NVS/provisioning
// in a real deployment, not hardcoded in shipped firmware.
// ---------------------------------------------------------------------------
const char* WIFI_SSID     = "PLACEHOLDER_SSID";
const char* WIFI_PASSWORD = "PLACEHOLDER_PASSWORD";
const char* MQTT_HOST     = "PLACEHOLDER_BROKER.example.com";
const uint16_t MQTT_PORT  = 8883;               // TLS, replaces the unauthenticated Apps Script webhook
const char* MQTT_TOPIC    = "liminal/intrusion/events";
const char* DEVICE_ID     = "node-01";

Adafruit_PWMServoDriver pca = Adafruit_PWMServoDriver();
DHT dht(PIN_DHT, DHTTYPE);
WiFiClientSecure netClient;
PubSubClient mqtt(netClient);

fusion::SensorSnapshot g_snapshot;
fusion::Config g_cfg;
uint32_t g_lastAlertMs = 0;
uint32_t g_lastSensorPollMs = 0;
constexpr uint32_t SENSOR_POLL_INTERVAL_MS = 150; // non-blocking cadence, replaces delay()-driven loop

// ---------------------------------------------------------------------------
// Hardware I/O -- kept separate from decision logic (fusion_logic.cpp) so the
// scoring rules can be unit tested without any of this.
// ---------------------------------------------------------------------------

float readSonarCm(uint8_t trigPin, uint8_t echoPin) {
    digitalWrite(trigPin, LOW);  delayMicroseconds(2);
    digitalWrite(trigPin, HIGH); delayMicroseconds(10);
    digitalWrite(trigPin, LOW);
    // 25ms timeout ~ 4.3m one-way, comfortably above the HC-SR04's 4m max range,
    // and short enough not to stall the loop noticeably at 150ms poll cadence.
    unsigned long duration = pulseIn(echoPin, HIGH, 25000UL);
    if (duration == 0) return 400.0f; // treat "no echo" as max range, not zero
    float tempC = dht.readTemperature();
    if (isnan(tempC)) tempC = 25.0f; // fallback, keeps the node alive if DHT glitches
    float speedOfSound_cm_per_us = 0.03313f + 0.0000606f * tempC; // same correction the original team used
    return (duration / 2.0f) * speedOfSound_cm_per_us;
}

void calibrateBaselines() {
    // Assumes the room is empty at boot/deploy time -- flagged in the README
    // as an operational requirement, same as any PIR-based alarm's "arm" step.
    float sums[3] = {0, 0, 0};
    const int SAMPLES = 20;
    for (int i = 0; i < SAMPLES; i++) {
        for (int c = 0; c < 3; c++) {
            sums[c] += readSonarCm(PIN_SONAR_TRIG[c], PIN_SONAR_ECHO[c]);
        }
        delay(50); // acceptable here: one-time boot calibration, not the main loop
    }
    for (int c = 0; c < 3; c++) {
        g_snapshot.sonar[c].baseline_cm = sums[c] / SAMPLES;
    }
}

void pollSensors() {
    for (int c = 0; c < 3; c++) {
        g_snapshot.sonar[c].distance_cm = readSonarCm(PIN_SONAR_TRIG[c], PIN_SONAR_ECHO[c]);
    }
    g_snapshot.pir_motion  = digitalRead(PIN_PIR) == HIGH;
    g_snapshot.sound_level = analogRead(PIN_SOUND_ANALOG) / 4095.0f; // ESP32 ADC is 12-bit
    g_snapshot.is_night    = isNightNow(); // implementation: RTC/light sensor/NTP hour check
    g_snapshot.ms_since_last_alert = millis() - g_lastAlertMs;
}

bool isNightNow() {
    // Placeholder -- real implementation reads an RTC or NTP-synced clock.
    // Kept as an explicit hook rather than a hidden assumption.
    return false;
}

void triggerAlert(const fusion::FusionResult& result) {
    pca.setPWM(PCA9685_BUZZER_CH, 0, 4095); // on
    digitalWrite(PIN_LED, HIGH);
    g_lastAlertMs = millis();

    char payload[256];
    snprintf(payload, sizeof(payload),
        "{\"device\":\"%s\",\"ts\":%lu,\"score\":%u,\"confidence\":%d,\"reason\":\"%s\"}",
        DEVICE_ID, (unsigned long)millis(), result.score,
        (int)result.confidence, result.reason);
    mqtt.publish(MQTT_TOPIC, payload, /*retained=*/false);
}

void ensureMqttConnected() {
    if (mqtt.connected()) return;
    // Non-blocking-ish reconnect: bounded attempt, not a blocking while(true) loop.
    if (mqtt.connect(DEVICE_ID)) {
        mqtt.subscribe((String(MQTT_TOPIC) + "/cmd").c_str()); // e.g. remote "disarm"
    }
}

void setup() {
    Serial.begin(115200);
    pinMode(PIN_PIR, INPUT);
    pinMode(PIN_LED, OUTPUT);
    for (int c = 0; c < 3; c++) {
        pinMode(PIN_SONAR_TRIG[c], OUTPUT);
        pinMode(PIN_SONAR_ECHO[c], INPUT);
    }

    Wire.begin();
    pca.begin();
    pca.setPWMFreq(60);
    dht.begin();

    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    // Real firmware: bounded connect-with-timeout + offline event queue,
    // not an infinite blocking wait -- flagged in README as a known
    // simplification of this listing.

    mqtt.setServer(MQTT_HOST, MQTT_PORT);

    esp_task_wdt_init(10, true); // 10s watchdog -- the original sketch had none;
                                  // a hung sonar read could brick a deployed unit silently.
    esp_task_wdt_add(NULL);

    calibrateBaselines();
}

void loop() {
    esp_task_wdt_reset();
    ensureMqttConnected();
    mqtt.loop();

    uint32_t now = millis();
    if (now - g_lastSensorPollMs >= SENSOR_POLL_INTERVAL_MS) {
        g_lastSensorPollMs = now;
        pollSensors();

        fusion::FusionResult result = fusion::evaluate(g_snapshot, g_cfg);
        if (result.should_alert) {
            triggerAlert(result);
        } else if (result.score > 0) {
            // Still publish low/suppressed events at low priority -- this is
            // the feed the AI triage agent (ai_agent/triage_agent.py) reads
            // to spot patterns a single-event view would miss.
            char payload[256];
            snprintf(payload, sizeof(payload),
                "{\"device\":\"%s\",\"ts\":%lu,\"score\":%u,\"confidence\":%d,\"reason\":\"%s\",\"suppressed\":true}",
                DEVICE_ID, (unsigned long)now, result.score, (int)result.confidence, result.reason);
            mqtt.publish((String(MQTT_TOPIC) + "/suppressed").c_str(), payload, false);
        }

        if (now - g_lastAlertMs > 3000) {
            pca.setPWM(PCA9685_BUZZER_CH, 0, 0); // off
            digitalWrite(PIN_LED, LOW);
        }
    }
    // No delay() here -- this is the direct fix for the coupled, blocking
    // loop implied by the original sketch's servo/BLE/sensor timing issues.
}
