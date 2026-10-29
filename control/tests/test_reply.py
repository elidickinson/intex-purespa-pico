#!/usr/bin/env python3
"""Simulate bus.pio's reply program against a synthetic SB-HS10-1 bus (timings from bus-protocol.md).

Usage: python -I test_reply.py <bus.pio.h>, or make -C tests
Pins: 0 = DATA, 1 = CLOCK, 2 = HOLD. 32 MHz: one cycle = 31.25 ns.
"""
import re
import sys
from dataclasses import replace

from pioemu import State, emulate

US = 32  # cycles per microsecond

header = open(sys.argv[1]).read()
block = re.search(r"reply_program_instructions\[\] = \{(.*?)\};", header, re.S).group(1)
opcodes = [int(h, 16) for h in re.findall(r"0x[0-9a-f]{4}", block)]
wrap_target = int(re.search(r"#define reply_wrap_target (\d+)", header).group(1))
wrap_top = int(re.search(r"#define reply_wrap (\d+)", header).group(1))

BIT = round(2.9 * US)      # bit period
CLK_HIGH = round(0.5 * US)
STROBE_AT = round(1.7 * US)  # HOLD rises this long after the 16th CLOCK rise
STROBE = round(0.5 * US)
NEXT_RISE = round(29 * US)   # next frame's first CLOCK rise after this frame's 16th

data, clock, hold = [], [], []
frames = []  # (decoded, 16th rise cycle, next frame's first rise cycle)


def add_frame(decoded, strobe=STROBE):
    raw = ~decoded & 0xFFFF
    start = len(data)
    for i in range(16):
        bit = raw >> (15 - i) & 1
        data.extend([bit] * BIT)
        clock.extend([0] * (BIT - CLK_HIGH) + [1] * CLK_HIGH)
        hold.extend([0] * BIT)
    rise16 = start + 15 * BIT + BIT - CLK_HIGH
    # After the frame: DATA idles high, CLOCK low, HOLD strobe
    gap = rise16 + NEXT_RISE - (BIT - CLK_HIGH) - len(data)
    data.extend([1] * gap)
    clock.extend([0] * gap)
    hold.extend([0] * gap)
    s = rise16 + STROBE_AT
    for t in range(s, s + strobe):
        hold[t] = 1
    frames.append((decoded, rise16, rise16 + NEXT_RISE))


RUN = [0x0100, 0x0108, 0x0102, 0x2100, 0x8100, 0x0500, 0x1100, 0x0180, 0x0110, 0x0140]
BEEP = 0x0100
NEVER = 0xFFFFFFFF
events = {}  # cycle -> value C puts in the TX FIFO


def run(n=1, beeping=False, strobe=STROBE):
    for _ in range(n):
        for f in RUN:
            add_frame(f & ~BEEP if beeping else f, strobe)


def c_writes(value):
    events[len(data)] = value


# A first frame to align on
add_frame(0x0100)
run(2)                     # idle: no replies
c_writes(~0x0108 & 0xFFFF)  # press bubbles
run(3)
run(1, strobe=round(4.9 * US))  # stretched strobes, seen occasionally
run(2, beeping=True)       # the spa beeps: replies must stop and X disarm
run(2)                     # beep over: still disarmed, no second press
c_writes(~0x0110 & 0xFFFF)  # press salt
run(2)
c_writes(NEVER)            # release
run(1)
add_frame(0xFFFF)          # DATA stuck low for a whole frame (raw 0)
run(1)
c_writes(~0x0108 & 0xFFFF)  # press bubbles again
run(1)
run(2, beeping=True)       # someone presses on the panel
events[frames[-1][1] - 10 * US] = ~0x0102 & 0xFFFF  # filter, written during the last beeping frame
run(2)                     # dropped by the beep: no answers
end = len(data)


def pins(t: int) -> int:
    if t >= end:
        return 0b001
    return data[t] | clock[t] << 1 | hold[t] << 2


drive = []  # (start, end) cycles where DATA's direction is output
state = State(x_register=NEVER)
driving_since = None
stops = sorted(events) + [end]
for stop in stops:
    for _, state in emulate(opcodes, stop_when=lambda op, s: s.clock >= stop, initial_state=state,
                            input_source=pins, shift_isr_right=False, shift_osr_right=True,
                            set_base=0, set_count=1, wrap_target=wrap_target, wrap_top=wrap_top):
        out = state.pin_directions & 1
        if out and driving_since is None:
            driving_since = state.clock
        elif not out and driving_since is not None:
            drive.append((driving_since, state.clock))
            driving_since = None
    if stop in events:
        state = replace(state, transmit_fifo=state.transmit_fifo.__class__([events[stop]]))
assert driving_since is None, "still driving DATA at the end"

failures = 0
answered = []
for start, stop in drive:
    match = [(d, r, n) for d, r, n in frames if r < start < n]
    assert len(match) == 1
    d, r, n = match[0]
    answered.append((d, r))
    print(f"answered {d:04x}: DATA low from {(start - r) / US:5.2f} us to {(stop - r) / US:5.2f} us "
          f"after its 16th CLOCK rise; next frame's first rise at {(n - r) / US:.1f} us")
    if not (3.5 <= (start - r) / US <= 4.5) or not (9.5 <= (stop - start) / US <= 10.5) or stop >= n - US:
        print("  ^ timing out of range")
        failures += 1

expected = []
armed = None
for i, (d, r, n) in enumerate(frames):
    for t, v in events.items():
        if frames[i - 1][1] < t <= r:  # written before this frame
            armed = None if v == NEVER else ~v & 0xFFFF
    if armed and not d & BEEP:
        armed = None  # the beep disarms
    if armed and d == armed:
        expected.append((d, r))
print(f"{len(answered)} answers, expected {len(expected)}")
if answered != expected:
    failures += 1
    print("answered:", [f"{d:04x}@{r}" for d, r in answered])
    print("expected:", [f"{d:04x}@{r}" for d, r in expected])
print("FAILED" if failures else "all good")
sys.exit(1 if failures else 0)
