"""Measure generated patches in a standalone Vital, outside the plugin.

The plugin measures every candidate through the Vital it hosts, which is the
right thing for deciding what to keep. It is the wrong thing for checking
whether that decision was correct, because a mistake in how the plugin renders
is invisible to a check that renders the same way.

This loads Vital independently and measures the .vital files on disk. It is how
the playhead bug was found: the plugin's own numbers looked healthy while the
same patches, played in Vital itself, were nothing like them.

    pip install numpy pedalboard mido
    python measure.py level      out/
    python measure.py pitch      out/
    python measure.py held       out/Bass_01.vital
"""
import argparse
import collections
import glob
import json
import os
import statistics as st
import sys

import numpy as np

import vitalstate

SR = 44100
NOTE = 48
EXPECTED_HZ = 440.0 * 2 ** ((NOTE - 69) / 12.0)


def open_vital(path=None):
    from pedalboard import load_plugin
    for candidate in ([path] if path else []) + [
        r"C:/Program Files/Common Files/VST3/Vital.vst3",
        os.path.expanduser("~/Library/Audio/Plug-Ins/VST3/Vital.vst3"),
        "/usr/lib/vst3/Vital.vst3",
    ]:
        if candidate and os.path.exists(candidate):
            plugin = load_plugin(candidate)
            template = bytes(plugin.raw_state)
            plugin.raw_state = template          # pay the cold start once
            return plugin, template
    raise SystemExit("could not find Vital.vst3, pass --vst3")


def render(plugin, template, path, seconds=2.4, note_off=1.7):
    from mido import Message
    doc = json.load(open(path, encoding="utf-8"))
    plugin.raw_state = vitalstate.write(template, doc)
    midi = [Message("note_on", note=NOTE, velocity=110, time=0.0)]
    if note_off is not None:
        midi.append(Message("note_off", note=NOTE, time=note_off))
    audio = plugin(midi, duration=seconds, sample_rate=SR, num_channels=2, reset=True)
    return audio, doc


def short_term_level(mono, window=0.09, percentile=90):
    """The 90th percentile of short-term windows, which is what the plugin
       matches. An average is dominated by whatever the patch does most of the
       time, and that is the wrong thing: a slow pad spends most of its note
       quiet and a plucked patch spends most of its note decaying."""
    n = int(window * SR)
    count = len(mono) // n
    if count < 2:
        return 0.0
    levels = [float(np.sqrt((mono[i * n:(i + 1) * n] ** 2).mean())) for i in range(count)]
    return float(np.percentile(levels, percentile))


def pitch_of(segment, top=2000.0):
    """Autocorrelation over one window. Returns the pitch and how definite it
       is, 1 being a clear note and 0 noise."""
    if len(segment) < 2048:
        return 0.0, 0.0
    segment = segment - segment.mean()
    correlation = np.correlate(segment, segment, mode="full")[len(segment) - 1:]
    if correlation[0] < 1e-12:
        return 0.0, 0.0
    correlation /= correlation[0]
    """`top` is the highest pitch worth looking for, and it is a trade.

    The default of 2 kHz keeps a bass on its fundamental: raise it and a short
    lag correlates on waveform shape rather than period, and one bass came back
    at 5880 Hz. But a sequence stepping several octaves up sounds above 2 kHz,
    and with the ceiling there every step returned the band edge instead: lag
    pinned to exactly 22 samples, 2004.5 Hz, which happens to sit a quarter tone
    off the semitone grid. That quarter tone was this ceiling, not the patch.

    So the stepped path raises it and the whole-note path does not.

    A whole sample of lag is also a coarse unit up there, 0.77 of a semitone
    between lag 22 and 23, so the peak is interpolated to recover the fraction.
    """
    lo, hi = int(SR / top), int(SR / 40)

    # Autocorrelation starts at 1.0 and falls away, and that opening slope is
    # not a period. Taking the largest value in the band picked a lag on the
    # slope instead of the real peak, so the answer was whatever the band
    # happened to start at: raise the ceiling and the reading followed it up.
    # Stepping past the descent first makes the peak that is found a real one.
    start = lo
    while start < hi and correlation[start] > 0:
        start += 1
    if start >= hi:
        start = lo

    band = correlation[start:hi]
    if not len(band):
        return 0.0, 0.0
    lag = int(np.argmax(band)) + start

    if 0 < lag < len(correlation) - 1:
        y1, y2, y3 = correlation[lag - 1], correlation[lag], correlation[lag + 1]
        denom = y1 - 2 * y2 + y3
        if abs(denom) > 1e-12:
            lag += float(np.clip(0.5 * (y1 - y3) / denom, -0.5, 0.5))

    return SR / lag, float(band.max())


def detect_steps(mono, window=0.09, limit=24):
    """The same, a step at a time.

    A sequence is meant to move, so measuring whether it holds one pitch asks
    the wrong question. A 0.35s window spans about three steps of a synced LFO
    and autocorrelation smears them together, which made clean sequences read as
    noise. These windows are shorter than a sixteenth at 120 BPM, so each holds
    one step."""
    envelope = np.convolve(np.abs(mono), np.ones(1024) / 1024, mode="same")
    peak = envelope.max()
    if peak < 1e-6:
        return []
    loud = np.where(envelope > peak * 0.35)[0]
    if len(loud) < SR // 10:
        return []

    step, out = int(SR * window), []
    for at in range(loud[0] + 512, len(mono) - step, step):
        if np.abs(mono[at:at + step]).max() < peak * 0.2:
            continue        # between steps, or past the end of the note
        hz, salience = pitch_of(mono[at:at + step], top=5000.0)
        if hz > 0:
            out.append((hz, salience))
        if len(out) >= limit:
            break
    return out


def detect_pitch(mono):
    """Autocorrelation over the loudest part of the note. Returns the pitch and
       how definite it is, 1 being a clear steady note and 0 noise."""
    envelope = np.convolve(np.abs(mono), np.ones(1024) / 1024, mode="same")
    peak = envelope.max()
    if peak < 1e-6:
        return 0.0, 0.0
    loud = np.where(envelope > peak * 0.35)[0]
    if len(loud) < SR // 10:
        return 0.0, 0.0

    return pitch_of(mono[loud[0] + 512:loud[0] + 512 + int(SR * 0.35)])


def files_in(target):
    if os.path.isdir(target):
        return sorted(glob.glob(os.path.join(target, "*.vital")))
    return [target]


def cmd_level(args):
    plugin, template = open_vital(args.vst3)
    rows = []
    for path in files_in(args.target):
        audio, _ = render(plugin, template, path)
        rows.append((short_term_level(audio.mean(axis=0)), os.path.basename(path)))

    values = sorted(r[0] for r in rows)
    q = lambda p: values[int(len(values) * p)]
    print("level, measured in standalone Vital (%d patches)" % len(values))
    print("  p10 %.3f   median %.3f   p90 %.3f" % (q(0.10), st.median(values), q(0.90)))
    print("  p10..p90 spread %.1f dB    full range %.1f dB"
          % (20 * np.log10(q(0.90) / max(q(0.10), 1e-9)),
             20 * np.log10(values[-1] / max(values[0], 1e-9))))

    by_style = collections.defaultdict(list)
    for level, name in rows:
        by_style[name.rsplit("_", 1)[0]].append(level)
    print()
    for style in sorted(by_style):
        print("  %-12s median %.3f" % (style, st.median(by_style[style])))
    print()
    print("  quietest: " + ", ".join("%s %.3f" % (n, v) for v, n in sorted(rows)[:3]))
    print("  loudest : " + ", ".join("%s %.3f" % (n, v) for v, n in sorted(rows)[-3:]))


def cmd_pitch(args):
    """A note is judged against the key that was pressed, except where the patch
       is meant to move. A sequence steps on purpose, so each step is judged on
       whether it lands on a semitone rather than on where it sits.

       Note this renders with no transport, so a tempo synced LFO runs at Vital's
       own default rate rather than the 120 BPM the plugin gives it. Step timing
       here will not match the plugin's, which is fine for asking whether the
       steps are clean and wrong for comparing their rate."""
    plugin, template = open_vital(args.vst3)
    print("pitch against the pressed note, C3 at %.1f Hz" % EXPECTED_HZ)
    print("%-22s %-10s %-11s %s" % ("file", "salience", "pitch", "off by"))
    for path in files_in(args.target):
        audio, _ = render(plugin, template, path)
        mono = audio.mean(axis=0)
        stepped = "Sequence" in os.path.basename(path)

        if stepped:
            steps = detect_steps(mono)
            if not steps:
                print("%-22s %s" % (os.path.basename(path), "no pitch found"))
                continue
            offs = [abs(s - round(s)) for s in
                    (12 * np.log2(hz / EXPECTED_HZ) for hz, _ in steps)]
            salience = float(np.median([s for _, s in steps]))
            error = float(np.median(offs))
            print("%-22s %-10.2f %2d steps      %.2f off the grid%s"
                  % (os.path.basename(path), salience, len(steps), error,
                     "   <-- off the grid" if error > 0.35 else ""))
            continue

        hz, salience = detect_pitch(mono)
        if hz <= 0:
            print("%-22s %s" % (os.path.basename(path), "no pitch found"))
            continue
        semitones = 12 * np.log2(hz / EXPECTED_HZ)
        # An octave is still the same note, so only the remainder is an error.
        error = semitones - round(semitones / 12) * 12
        print("%-22s %-10.2f %8.1f Hz  %+.2f semitones%s"
              % (os.path.basename(path), salience, hz, error,
                 "   <-- wrong note" if abs(error) > 0.5 else ""))


def cmd_held(args):
    """Hold the key and never let go. A bass that keeps going is the single
       most common complaint, and it is invisible from the parameters: a patch
       with its sustain at zero still drones if an LFO pushes a level back up."""
    plugin, template = open_vital(args.vst3)
    for path in files_in(args.target):
        audio, doc = render(plugin, template, path, seconds=4.0, note_off=None)
        mono = np.abs(audio.mean(axis=0))
        step = SR // 10
        envelope = np.array([np.sqrt((mono[i * step:(i + 1) * step] ** 2).mean())
                             for i in range(40)])
        peak = envelope.max()
        if peak < 1e-6:
            print("%-22s silent" % os.path.basename(path))
            continue
        relative = envelope / peak
        below = np.where(relative < 0.10)[0]
        settings = doc["settings"]
        print("%-22s falls to 10%% at %-8s sustain %.2f  decay %.2f   %s"
              % (os.path.basename(path),
                 ("%.1fs" % (below[0] * 0.1)) if len(below) else "never",
                 settings.get("env_1_sustain", -1), settings.get("env_1_decay", -1),
                 " ".join("%.2f" % relative[i] for i in range(0, 24, 3))))


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("command", choices=["level", "pitch", "held"])
    parser.add_argument("target", nargs="?", default="../out",
                        help="a .vital file or a folder of them")
    parser.add_argument("--vst3", default=None, help="path to Vital.vst3")
    args = parser.parse_args()

    {"level": cmd_level, "pitch": cmd_pitch,
     "held": cmd_held}[args.command](args)


if __name__ == "__main__":
    sys.exit(main())
