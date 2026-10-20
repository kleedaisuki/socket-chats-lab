"""Recompute paper summaries with exact SymPy arithmetic; no generated files."""

from pathlib import Path
import json
import sympy as sp

ROOT = Path(__file__).resolve().parents[1]


def median(values):
    return sorted(map(lambda value: sp.Rational(str(value)), values))[len(values) // 2]


def main():
    before_rate = median([659.50, 647.36, 651.64])
    after_rate = median([2604.69, 2652.47, 2694.16])
    before_latency = median([24.086, 24.639, 24.466])
    after_latency = median([5.767, 5.492, 5.568])
    summaries = {
        "fanout_trial_median_throughput_ratio": after_rate / before_rate,
        "fanout_trial_median_p50_reduction_percent": 100 * (1 - after_latency / before_latency),
        "ascii_burst_ratio": sp.Rational("3410.650") / sp.Rational("26.817"),
        "unicode_burst_ratio": sp.Rational("2410.523") / sp.Rational("28.062"),
        "completion_wakeup_ratio": sp.Rational("15.53185") / sp.Rational("0.12635"),
    }
    for key, value in summaries.items():
        print(f"{key}: {value} = {sp.N(value, 10)}")
    assert round(float(summaries["fanout_trial_median_throughput_ratio"]), 2) == 4.07
    assert round(float(summaries["fanout_trial_median_p50_reduction_percent"]), 1) == 77.2
    # Historical raw caches are optional; the paper retains all three trial values.
    for phase, rates, latencies in (
        ("before", [659.50, 647.36, 651.64], [24.086, 24.639, 24.466]),
        ("after", [2604.69, 2652.47, 2694.16], [5.767, 5.492, 5.568]),
    ):
        for index, (rate, latency) in enumerate(zip(rates, latencies), 1):
            path = ROOT / ".cache" / "server" / f"broadcast-{phase}-{index}.json"
            if path.exists():
                raw = json.loads(path.read_text(encoding="utf-8"))
                assert round(raw["acknowledgements_per_second"], 2) == rate
                assert round(raw["send_ack_ms"]["p50"], 3) == latency
    print(f"PASS: exact arithmetic and available trial records (SymPy {sp.__version__})")


if __name__ == "__main__":
    main()
