#!/usr/bin/env python3
"""
triage_agent.py

The cloud-side half of the AI Fluency story for this retrofit: the firmware
publishes every scored event (alerted AND suppressed) to MQTT/an event
store; this agent reads that stream and turns a night's worth of raw scores
into a short, human-readable briefing a homeowner/guard would actually read
-- clustering repeat events, calling out the one that actually matters, and
explaining *why* in plain language. This is AI used as a product feature,
not just as a coding accelerant during development.

Two modes:
  --offline   Deterministic local summarizer. No network, no API key.
              This is what was actually run to produce the sample output
              in the case study document, so it's provable in this
              sandbox without secrets.
  (default)   Calls the real Anthropic Messages API if ANTHROPIC_API_KEY
              is set, for a richer natural-language briefing. Falls back
              to --offline behaviour automatically if the key is missing
              or the request fails, so the agent degrades gracefully
              rather than going dark during an actual intrusion.

Usage:
  python3 triage_agent.py sample_events.json --offline
  ANTHROPIC_API_KEY=... python3 triage_agent.py sample_events.json
"""

import argparse
import json
import os
import sys
from collections import defaultdict
from datetime import datetime, timedelta, timezone

CONFIDENCE_NAMES = {0: "LOW", 1: "MEDIUM", 2: "HIGH"}


def load_events(path):
    with open(path, "r") as f:
        return json.load(f)


def cluster_events(events, gap_seconds=120):
    """Group events into incidents: consecutive events within `gap_seconds`
    of each other belong to the same incident. Mirrors how a human would
    read a burst of sensor noise as 'one thing happening', not five."""
    events = sorted(events, key=lambda e: e["ts"])
    clusters = []
    current = []
    last_ts = None
    for e in events:
        ts = datetime.fromisoformat(e["ts"])
        if last_ts is not None and (ts - last_ts).total_seconds() > gap_seconds:
            clusters.append(current)
            current = []
        current.append(e)
        last_ts = ts
    if current:
        clusters.append(current)
    return clusters


def summarize_cluster_offline(cluster):
    max_score = max(e["score"] for e in cluster)
    max_conf = max(e["confidence"] for e in cluster)
    alerted = any(not e.get("suppressed", False) for e in cluster)
    reasons = defaultdict(int)
    for e in cluster:
        reasons[e["reason"]] += 1
    top_reason = max(reasons, key=reasons.get)
    start = cluster[0]["ts"]
    end = cluster[-1]["ts"]
    n = len(cluster)

    verdict_map = {
        "sonar+motion+sound_agree": "Multiple independent sensors agreed — treat as a likely real event.",
        "sonar+motion_agree": "Two independent signals agreed — worth a look.",
        "multi_angle_sonar_agree": "Multiple sonar angles agreed but no motion/sound corroboration — check the log if it recurs.",
        "single_signal_above_threshold": "A single signal crossed threshold — plausible false positive (pet, curtain, staff).",
        "single_weak_signal_suppressed": "Weak, isolated signal — automatically suppressed, logged for pattern tracking only.",
        "suppressed_by_cooldown": "Repeat trigger within the debounce window — same event as the prior alert, not a new one.",
    }
    verdict = verdict_map.get(top_reason, "Signal pattern did not match a known profile — flag for manual review.")

    return {
        "window": f"{start} → {end}",
        "event_count": n,
        "peak_score": max_score,
        "peak_confidence": CONFIDENCE_NAMES.get(max_conf, "?"),
        "dominant_reason": top_reason,
        "was_alerted": alerted,
        "verdict": verdict,
    }


def build_offline_briefing(events):
    clusters = cluster_events(events)
    incidents = [summarize_cluster_offline(c) for c in clusters]
    incidents.sort(key=lambda i: i["peak_score"], reverse=True)

    lines = []
    lines.append(f"NIGHTLY TRIAGE BRIEFING — {len(events)} raw events → {len(incidents)} incident(s)")
    lines.append("=" * 70)
    for i, inc in enumerate(incidents, 1):
        flag = "🔴 ALERTED" if inc["was_alerted"] else "⚪ suppressed"
        lines.append(
            f"{i}. [{flag}] {inc['window']}  "
            f"(peak {inc['peak_confidence']}, score {inc['peak_score']}, "
            f"{inc['event_count']} readings)"
        )
        lines.append(f"   {inc['verdict']}")
    lines.append("=" * 70)
    real_alerts = [i for i in incidents if i["was_alerted"]]
    if real_alerts:
        lines.append(f"ACTION: {len(real_alerts)} incident(s) need human review, highest priority first (above).")
    else:
        lines.append("ACTION: nothing crossed the alert threshold tonight — no action needed.")
    return "\n".join(lines)


def build_llm_briefing(events):
    """Calls the real Anthropic Messages API. Requires ANTHROPIC_API_KEY.
    Falls back to the offline briefing if the key is missing or the call
    fails, so a triage run never goes silent."""
    api_key = os.environ.get("ANTHROPIC_API_KEY")
    if not api_key:
        sys.stderr.write("[triage_agent] no ANTHROPIC_API_KEY set, falling back to --offline mode\n")
        return build_offline_briefing(events)

    try:
        import anthropic  # pip install anthropic

        client = anthropic.Anthropic(api_key=api_key)
        prompt = (
            "You are triaging events from a home intrusion-detection sensor node. "
            "Each event has a fused confidence score (0-100), a reason code, and "
            "whether it triggered an alert. Group related events into incidents, "
            "explain in plain language what likely happened, and end with a short "
            "prioritized action list. Be concise -- this is read on a phone.\n\n"
            f"Events (JSON):\n{json.dumps(events, indent=2)}"
        )
        response = client.messages.create(
            model="claude-sonnet-4-6",
            max_tokens=600,
            messages=[{"role": "user", "content": prompt}],
        )
        return "".join(block.text for block in response.content if block.type == "text")
    except Exception as exc:  # network issues, bad key, SDK not installed, etc.
        sys.stderr.write(f"[triage_agent] LLM call failed ({exc}), falling back to --offline mode\n")
        return build_offline_briefing(events)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("events_file", help="Path to a JSON array of logged events")
    parser.add_argument("--offline", action="store_true", help="Use the deterministic local summarizer only")
    args = parser.parse_args()

    events = load_events(args.events_file)
    briefing = build_offline_briefing(events) if args.offline else build_llm_briefing(events)
    print(briefing)


if __name__ == "__main__":
    main()
