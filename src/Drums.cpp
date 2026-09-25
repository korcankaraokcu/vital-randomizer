#include "Drums.h"

namespace drums
{
    namespace
    {
        /*  What every kind shares.

            Everything goes through filter one, the sample included. Vital sends
            the sample layer to the effects by default, past both filters, which
            would leave a hi-hat's noise untouched by the high pass that makes
            it a hi-hat, and sends oscillator two to filter two. Sustain is zero
            because nothing here is held.
        */
        std::vector<archetype::Setting> shared (std::vector<archetype::Setting> extra)
        {
            std::vector<archetype::Setting> s = {
                { "osc_1_destination", 0.0 }, { "osc_2_destination", 0.0 },
                { "osc_3_destination", 0.0 }, { "sample_destination", 0.0 },
                { "filter_1_on", 1.0 }, { "env_1_sustain", 0.0 },
                { "env_2_attack", 0.0 }, { "env_2_sustain", 0.0 },
                { "osc_1_unison_voices", 1.0 },
            };
            s.insert (s.end(), extra.begin(), extra.end());
            return s;
        }

        std::vector<Kind> build()
        {
            std::vector<Kind> list;

            {   /*  Kick. A sine-like body whose pitch falls two to three octaves
                    into the note in a few tens of milliseconds, over a thump,
                    under a low pass. An octave under the key, so the preview
                    note lands a kick at 65 Hz. */
                Kind k { "Kick" };
                k.settings = shared ({ { "osc_2_on", 0.0 }, { "reverb_dry_wet", 0.05 } });
                k.routings = { { "env_2", "osc_1_transpose", 0.3f, false } };
                k.character = "fundamental"; k.transpose = -12.0f;
                k.sample = "Thump"; k.sampleLevel = { 0.20f, 0.40f };
                k.attack = { 0.0f, 0.02f }; k.decay = { 0.80f, 0.95f }; k.release = { 0.30f, 0.50f };
                k.blend = { 0.0f, 0.2f }; k.cutoff = { 55.0f, 75.0f }; k.maxResonance = 0.2f;
                k.sweep = { 12.0f, 30.0f };
                k.drop = { 24.0f, 36.0f }; k.dropDecay = { 0.45f, 0.60f };
                k.brightness = { 0.0f, 700.0f }; k.minLowRatio = 0.55f; k.maxHeld = 0.35f;
                list.push_back (k);
            }
            {   /*  Snare. A short tonal body a fourth above the key with a small
                    drop, and a noise layer carrying the rattle, through a low
                    pass set high enough to keep the top of it. */
                Kind k { "Snare" };
                k.settings = shared ({ { "osc_2_on", 0.0 }, { "reverb_dry_wet", 0.12 } });
                k.routings = { { "env_2", "osc_1_transpose", 0.08f, false } };
                // The rattle carries more of a snare's energy than its tone
                // does. A single clean voice at these levels was otherwise
                // nine tenths bass.
                k.character = "fundamental"; k.transpose = 5.0f; k.body = { 0.20f, 0.32f };
                k.sample = "Noise"; k.sampleLevel = { 0.60f, 0.85f };
                k.attack = { 0.0f, 0.02f }; k.decay = { 0.70f, 0.82f }; k.release = { 0.20f, 0.35f };
                k.blend = { 0.0f, 0.3f }; k.cutoff = { 104.0f, 116.0f }; k.maxResonance = 0.25f;
                k.sweep = { 6.0f, 18.0f };
                k.drop = { 5.0f, 12.0f }; k.dropDecay = { 0.45f, 0.55f };
                k.brightness = { 700.0f, 6000.0f }; k.maxHeld = 0.35f;
                list.push_back (k);
            }
            {   /*  Clap. No body, only noise through a band pass, struck three
                    or four times in quick succession before the tail, which is
                    what hands clapping together actually sounds like. */
                Kind k { "Clap" };
                k.settings = shared ({ { "osc_2_on", 0.0 }, { "reverb_dry_wet", 0.15 } });
                k.tonal = false;
                k.sample = "Noise"; k.sampleLevel = { 0.60f, 0.80f };
                k.attack = { 0.0f, 0.02f }; k.decay = { 0.72f, 0.85f }; k.release = { 0.20f, 0.30f };
                k.blend = { 0.9f, 1.0f }; k.cutoff = { 86.0f, 96.0f }; k.maxResonance = 0.3f;
                k.sweep = { 0.0f, 4.0f };
                k.secondFilter = false; k.flam = true;
                // White noise through Vital's gentle band pass keeps plenty of
                // top, and a real clap does too, so the centre sits higher than
                // the band pass does.
                k.brightness = { 1000.0f, 7000.0f }; k.maxHeld = 0.35f;
                list.push_back (k);
            }
            {   /*  Closed hi-hat. The 808's six square cluster with a touch of
                    noise under a high pass, gone in about fifty milliseconds.
                    No oscillator: a wavetable is one cycle, so its partials are
                    all harmonics of one pitch, and the old metallic body read
                    as a note at 1046 Hz. */
                Kind k { "Closed hat" };
                k.settings = shared ({ { "osc_2_on", 0.0 }, { "reverb_dry_wet", 0.06 } });
                k.tonal = false;
                k.sample = "Metal"; k.sampleLevel = { 0.60f, 0.85f }; k.metalNoise = { 0.05f, 0.25f };
                k.attack = { 0.0f, 0.01f }; k.decay = { 0.47f, 0.56f }; k.release = { 0.08f, 0.15f };
                k.blend = { 1.0f, 1.0f }; k.cutoff = { 112.0f, 124.0f }; k.maxResonance = 0.25f;
                k.sweep = { 0.0f, 6.0f };
                k.secondFilter = false;
                k.brightness = { 3500.0f, 16000.0f }; k.maxHeld = 0.25f;
                list.push_back (k);
            }
            {   /*  Open hi-hat. Left to ring for 90 to 600 ms, where the 808's own
                    open hat sits, and so it cannot be the closed hat's metal
                    held longer: a ring that long lets the six squares be heard
                    one by one, which is the metallic edge. It gets two banks of
                    them, twelve partials instead of six, tilted up from higher,
                    under more noise and no resonance, so the metal is a sheen
                    on the hiss rather than a chord. */
                Kind k { "Open hat" };
                k.settings = shared ({ { "osc_2_on", 0.0 }, { "reverb_dry_wet", 0.10 } });
                k.tonal = false;
                k.sample = "Metal"; k.sampleLevel = { 0.60f, 0.85f }; k.metalNoise = { 0.35f, 0.55f };
                k.metalCorner = { 5000.0f, 7000.0f }; k.metalBanks = 2;
                k.attack = { 0.0f, 0.01f }; k.decay = { 0.66f, 0.92f }; k.release = { 0.25f, 0.45f };
                k.blend = { 1.0f, 1.0f }; k.cutoff = { 114.0f, 124.0f }; k.maxResonance = 0.08f;
                k.sweep = { 0.0f, 6.0f };
                k.secondFilter = false;
                k.brightness = { 3500.0f, 16000.0f }; k.maxHeld = 0.45f;
                list.push_back (k);
            }
            {   /*  Crash. A real crash is a plate with thousands of modes, dense
                enough above a few kilohertz to be close to noise, and the high
                ones die first, so it darkens as it rings: measured on sampled
                crashes, the centre of the spectrum falls from about 10 kHz at
                the strike to 5 by two seconds, with next to nothing under 250
                Hz. Six squares, however much noise is laid over them, are a
                few hundred exact harmonic lines at one brightness. So it is
                the Cymbal sample, noise rung through a couple of hundred broad
                resonances, under a low pass the second envelope throws open at
                the strike and closes over the ring. And no compressor: Vital's
                is multiband, and on a cymbal it lifted the quiet low band by
                26 dB into a rumble and held the tail up twice as long as a real
                one rings. */
                Kind k { "Crash" };
                k.settings = shared ({ { "osc_2_on", 0.0 }, { "reverb_dry_wet", 0.22 },
                                       { "compressor_on", 0.0 } });
                k.tonal = false;
                k.sample = "Cymbal"; k.sampleLevel = { 0.60f, 0.85f }; k.metalNoise = { 0.10f, 0.25f };
                k.modes = 200; k.modeQ = { 35.0f, 80.0f };
                k.attack = { 0.0f, 0.02f }; k.decay = { 1.25f, 1.45f }; k.release = { 0.80f, 1.20f };
                k.blend = { 0.0f, 0.1f }; k.cutoff = { 106.0f, 116.0f }; k.maxResonance = 0.1f;
                k.sweep = { 22.0f, 32.0f }; k.sweepDecay = { 1.10f, 1.25f };
                k.secondFilter = false;
                k.brightness = { 2500.0f, 14000.0f }; k.maxHeld = 0.95f;
                list.push_back (k);
            }
            {   /*  Ride. The same kind of plate played for its ping, so fewer,
                sharper resonances and less wash than a crash: sampled rides
                showed twenty or thirty spectral lines standing 11 to 19 dB
                clear of the rest, where the six square cluster stood eighty
                lines 30 dB clear, which is a chord rather than a cymbal. It
                darkens as it rings as well, from about 7 kHz to under 3. */
                Kind k { "Ride" };
                k.settings = shared ({ { "osc_2_on", 0.0 }, { "reverb_dry_wet", 0.15 },
                                       { "compressor_on", 0.0 } });
                k.tonal = false;
                k.sample = "Cymbal"; k.sampleLevel = { 0.60f, 0.85f }; k.metalNoise = { 0.05f, 0.15f };
                k.modes = 90; k.modeQ = { 110.0f, 240.0f };
                k.attack = { 0.0f, 0.02f }; k.decay = { 1.10f, 1.30f }; k.release = { 0.60f, 0.90f };
                k.blend = { 0.0f, 0.2f }; k.cutoff = { 100.0f, 110.0f }; k.maxResonance = 0.15f;
                k.sweep = { 20.0f, 30.0f }; k.sweepDecay = { 1.00f, 1.20f };
                k.secondFilter = false;
                k.brightness = { 2000.0f, 12000.0f }; k.maxHeld = 0.9f;
                list.push_back (k);
            }
            {   /*  Tom. A round body at the note with a modest drop into it,
                    under a low pass, for a third to two thirds of a second. */
                Kind k { "Tom" };
                k.settings = shared ({ { "osc_2_on", 0.0 }, { "reverb_dry_wet", 0.12 } });
                k.routings = { { "env_2", "osc_1_transpose", 0.08f, false } };
                k.character = "fundamental"; k.transpose = 0.0f;
                // No thump underneath: it carries its own falling low pitch,
                // and read against the tom's it pulled the note down by half
                // an octave.
                k.attack = { 0.0f, 0.02f }; k.decay = { 0.85f, 1.00f }; k.release = { 0.30f, 0.50f };
                k.blend = { 0.0f, 0.2f }; k.cutoff = { 65.0f, 85.0f }; k.maxResonance = 0.25f;
                k.sweep = { 6.0f, 18.0f };
                // A small quick glide. A bigger or slower one had not settled
                // by the time the pitch is read, and the tom read sharp.
                k.drop = { 3.0f, 8.0f }; k.dropDecay = { 0.40f, 0.55f };
                k.brightness = { 100.0f, 1800.0f }; k.maxHeld = 0.45f; k.pitched = true;
                list.push_back (k);
            }
            {   /*  Timpani. A kettle drum is not a sine. Its head rings in a
                    handful of modes, and the bowl and the strike pick out the
                    ones near a principal, a fifth, an octave and a tenth above
                    it, at 1, 1.5, 1.98 and 2.44 times, in about 5 to 4 to 3 to
                    1. Those are within a few cents of harmonics two to five of
                    the note an octave down, so the body is that table played an
                    octave down, with the principal on the key. As with a real
                    kettle drum, the ear may place the pitch an octave below the
                    principal, the fundamental those four imply.
                    Over it the felt mallet's thud, and a room, since a timpani
                    is never heard dry. Built as a bare sine under a low pass,
                    which was all it had been, it was a bass. */
                Kind k { "Timpani" };
                k.settings = shared ({ { "osc_2_on", 0.0 }, { "reverb_dry_wet", 0.22 } });
                k.routings = { { "env_2", "osc_1_transpose", 0.02f, false } };
                k.character = "membrane"; k.inharmonic = { 0.0f, 0.0f }; k.transpose = -12.0f;
                k.sample = "Mallet"; k.sampleLevel = { 0.25f, 0.45f };
                k.attack = { 0.0f, 0.02f }; k.decay = { 1.18f, 1.32f }; k.release = { 0.70f, 1.00f };
                k.blend = { 0.0f, 0.2f }; k.cutoff = { 76.0f, 92.0f }; k.maxResonance = 0.2f;
                k.sweep = { 8.0f, 18.0f };
                // A slight, quick settle. A slower one was still sounding when
                // the pitch is read, and half the timpani read as wrong notes.
                k.drop = { 1.0f, 2.0f }; k.dropDecay = { 0.40f, 0.55f };
                k.room = true;
                k.brightness = { 80.0f, 1500.0f }; k.maxHeld = 0.85f; k.pitched = true;
                list.push_back (k);
            }
            {   /*  Cowbell. Two square tones a fifth apart two octaves up, the
                    classic machine recipe, through a band pass, short. */
                Kind k { "Cowbell" };
                k.settings = shared ({ { "osc_2_on", 1.0 }, { "osc_2_level", 0.5 },
                                       { "reverb_dry_wet", 0.08 } });
                k.character = "square"; k.transpose = 24.0f; k.secondTranspose = 31.0f;
                k.attack = { 0.0f, 0.02f }; k.decay = { 0.68f, 0.80f }; k.release = { 0.15f, 0.25f };
                k.blend = { 0.9f, 1.0f }; k.cutoff = { 90.0f, 100.0f }; k.maxResonance = 0.35f;
                k.sweep = { 0.0f, 6.0f };
                k.secondFilter = false;
                k.brightness = { 500.0f, 4000.0f }; k.maxHeld = 0.35f; k.pitched = true;
                list.push_back (k);
            }
            {   /*  Rim and woodblock. A very short, high, slightly inharmonic
                    click through a band pass, gone in well under a tenth of a
                    second. */
                Kind k { "Rim" };
                k.settings = shared ({ { "osc_2_on", 0.0 }, { "reverb_dry_wet", 0.08 } });
                k.character = "fundamental"; k.inharmonic = { 0.1f, 0.4f }; k.transpose = 24.0f;
                k.sample = "Struck"; k.sampleLevel = { 0.08f, 0.18f };
                k.attack = { 0.0f, 0.01f }; k.decay = { 0.45f, 0.58f }; k.release = { 0.05f, 0.15f };
                k.blend = { 1.0f, 1.0f }; k.cutoff = { 92.0f, 104.0f }; k.maxResonance = 0.35f;
                k.sweep = { 0.0f, 8.0f };
                k.secondFilter = false;
                k.brightness = { 600.0f, 6000.0f }; k.maxHeld = 0.2f;
                list.push_back (k);
            }
            {   /*  Shaker. Not one impact but a cloud of little ones, beads or
                    seeds hitting the wall of a shell, which is how Perry Cook's
                    shaker models build them: collisions at random, each a tiny
                    burst of noise rung through the shell's one resonance, 3.2
                    kHz for a maraca, 3 for a cabasa, 5.5 for a sekere. That is
                    the Beads sample. And a shaker is played, not struck: back
                    and forth, sha-ka, a soft stroke each way, the first a little
                    heavier. So while the key is held a one bar pattern in time
                    with the song swells the beads up and down twice a cycle,
                    and a tap is one stroke. */
                Kind k { "Shaker" };
                k.settings = shared ({ { "osc_2_on", 0.0 }, { "reverb_dry_wet", 0.08 } });
                k.tonal = false;
                k.sample = "Beads"; k.sampleLevel = { 0.60f, 0.80f };
                k.attack = { 0.10f, 0.20f }; k.decay = { 0.60f, 0.75f }; k.release = { 0.20f, 0.30f };
                k.sustain = { 0.80f, 0.95f };
                k.blend = { 1.0f, 1.0f }; k.cutoff = { 104.0f, 114.0f }; k.maxResonance = 0.15f;
                k.sweep = { 0.0f, 2.0f };
                k.secondFilter = false;
                k.shake = true;
                k.brightness = { 2500.0f, 14000.0f }; k.maxHeld = 1.0f;
                list.push_back (k);
            }
            return list;
        }
    }

    const std::vector<Kind>& all()
    {
        static const std::vector<Kind> list = build();
        return list;
    }

    const Kind* at (int index)
    {
        const auto& list = all();
        return index >= 0 && index < (int) list.size() ? &list[(size_t) index] : nullptr;
    }

    int indexOf (const std::string& name)
    {
        const auto& list = all();
        for (size_t i = 0; i < list.size(); ++i)
            if (name == list[i].name)
                return (int) i;
        return -1;
    }

    std::string profile (const std::string& style, int kind)
    {
        if (style != "Percussion" || at (kind) == nullptr)
            return style;
        return style + "/" + at (kind)->name;
    }

    const Kind* fromProfile (const std::string& p)
    {
        const auto slash = p.find ('/');
        if (slash == std::string::npos || p.substr (0, slash) != "Percussion")
            return nullptr;
        return at (indexOf (p.substr (slash + 1)));
    }

    std::string styleOf (const std::string& p)
    {
        const auto slash = p.find ('/');
        return slash == std::string::npos ? p : p.substr (0, slash);
    }
}
