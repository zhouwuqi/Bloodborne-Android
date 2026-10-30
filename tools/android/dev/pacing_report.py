#!/usr/bin/env python3
"""Summary of a game log's display pacing and thermal lines (gpu/shim/bbport_pacing.h, bbport_adpf.cpp).

usage: pacing_report.py android-game.log   (tools/android/dev/pocket_s.sh log pulls one and runs this)

One row per "Pacing:" line (10 s each): FPS on screen, the share of frames on screen for exactly the frame
interval, the display-only judders, the game's late frames, the hand-over-to-screen time, the thermal headroom
and governor level; then the totals, weighted by frames, and the governor's level changes.
"""
import re
import sys

PACING = re.compile(
    r"Pacing: (?P<fps>[\d.]+) FPS on screen \((?P<mode>[^)]*)\), handed over every (?P<iv>[\d.]+) ms; "
    r"on screen for that long (?P<on>[\d.]+)%, shorter (?P<short>[\d.]+)%, longer (?P<long>[\d.]+)% "
    r"\(uneven at the display only (?P<disp>[\d.]+)%, the game late (?P<late>[\d.]+)%\), "
    r"(?P<mean>[\d.]+) ms sd (?P<sd>[\d.]+); hand-over to screen (?P<lat>[\d.]+) ms \(p5 (?P<p5>[\d.]+), "
    r"p95 (?P<p95>[\d.]+)\); refresh (?P<refresh>[^;]*); lock (?P<lock>[^,]*), (?P<shifts>\d+) shifts; (?P<thermal>.*)$")
HEADROOM = re.compile(r"headroom ([\d.]+)")
GOVERNOR = re.compile(r"governor (\d+)")


def main(path):
    rows, events = [], []
    with open(path, errors="replace") as f:
        for line in f:
            line = line.rstrip("\n")
            m = PACING.search(line)
            if m:
                rows.append(m.groupdict())
            elif "Thermal governor:" in line or "slots move" in line or line.startswith("Thermal: status"):
                events.append(line.strip())
    if not rows:
        print("No Pacing: lines (the pacing build not on the phone, BB_PACING_STATS=0, or no game frames yet)")
        return 1
    print(f"{'t/s':>5} {'FPS':>5} {'on%':>6} {'disp%':>6} {'late%':>6} {'sd ms':>6} {'lat ms':>7} "
          f"{'head':>5} {'gov':>3}  lock")
    total_frames = on = disp = late = 0.0
    for i, r in enumerate(rows):
        fps = float(r["fps"])
        frames = fps * 10.0
        total_frames += frames
        on += frames * float(r["on"]) / 100.0
        disp += frames * float(r["disp"]) / 100.0
        late += frames * float(r["late"]) / 100.0
        h = HEADROOM.search(r["thermal"])
        g = GOVERNOR.search(r["thermal"])
        print(f"{(i + 1) * 10:>5} {fps:>5.1f} {float(r['on']):>6.1f} {float(r['disp']):>6.1f} "
              f"{float(r['late']):>6.1f} {float(r['sd']):>6.2f} {float(r['lat']):>7.1f} "
              f"{h.group(1) if h else '-':>5} {g.group(1) if g else '-':>3}  {r['lock']} ({r['shifts']} shifts, {r['mode']})")
    print()
    print(f"{len(rows) * 10} s, ~{total_frames:.0f} frames: on screen for the frame interval {100 * on / total_frames:.2f}%, "
          f"uneven at the display only {100 * disp / total_frames:.2f}%, the game late {100 * late / total_frames:.2f}%")
    if events:
        print()
        for e in events:
            print(e)
    return 0


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print(__doc__.strip())
        sys.exit(2)
    sys.exit(main(sys.argv[1]))
