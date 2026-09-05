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

    segment = mono[loud[0] + 512:loud[0] + 512 + int(SR * 0.35)]
    if len(segment) < 2048:
        return 0.0, 0.0
    segment = segment - segment.mean()
    correlation = np.correlate(segment, segment, mode="full")[len(segment) - 1:]
    if correlation[0] < 1e-12:
        return 0.0, 0.0
    correlation /= correlation[0]

    lo, hi = int(SR / 2000), int(SR / 40)
    band = correlation[lo:hi]
    lag = int(np.argmax(band)) + lo
    return SR / lag, float(band.max())


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
    plugin, template = open_vital(args.vst3)
    print("pitch against the pressed note, C3 at %.1f Hz" % EXPECTED_HZ)
    print("%-22s %-10s %-11s %s" % ("file", "salience", "pitch", "off by"))
    for path in files_in(args.target):
        audio, _ = render(plugin, template, path)
        hz, salience = detect_pitch(audio.mean(axis=0))
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
