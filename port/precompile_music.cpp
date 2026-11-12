/*
 * The precompile screen's music.
 *
 * "Embers", an original theme in D written for this project, over the scene's sea, wind, brazier and far thunder:
 * all of it made here, sample by sample, when the screen opens. Nothing is recorded and no file comes with it.
 *
 * The theme: 90 a minute, 32 bars, 85.3 s, looped. An intro on a drone, a heartbeat on the war drum and a choir
 * coming in; theme A (D minor with the Phrygian E flat) over a plucked lyre and drums; theme B, the climax, in D
 * Phrygian dominant (the step from E flat to F sharp), the choir full, low brass, war drums in 3+3+2 and an ostinato
 * on D; theme A again, the lead doubled an octave down, with brass calls; an outro back to the intro. The voices are
 * an 8-bit console's (band-limited pulses, a stepped triangle, noise), with a choir of detuned pulses through vowel
 * formants and a hall: Jezar's Freeverb (public domain), written again here.
 *
 * The loop is rendered once and 8 s past its end, and what sounds past the end (echoes, the hall, releases) is added
 * to its start, as the loop before it leaves it there. The ambience is a loop of the same length crossfaded over its
 * seam. Thunder plays 1.2 s after each of the scene's strokes of lightning (loading_screen.cpp: 6 s in, then every
 * 9.5 s), on the screen's clock.
 *
 * It plays only on the precompile screen, from 2.5 s after the screen opens (a start that finds the shaders built
 * ends sooner, in silence), fading in, until the game's window is up, fading out over a second. [system]
 * precompile_music (GOWA_PRECOMPILE_MUSIC): off, low, normal (the default), high. XAudio2 2.9, Windows 10's own, is
 * loaded when it plays: without it there is none.
 *
 * Ascension-R.exe --music-preview <file.wav> [seconds] writes what the screen plays from its start, at full level.
 */
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <xaudio2.h>

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vector>

#include "ps3emu/ps3_settings.h"
#include "precompile_music.h"

#pragma comment(lib, "ole32.lib")

namespace {

const double PI = 3.14159265358979323846;
const int RATE = 48000;
const double U = 60.0 / 90.0 / 4.0;   /* a sixteenth */
const int BAR = 16;
const int NBARS = 32;
const double LOOP = NBARS * BAR * U;
const double TAIL = 8.0;              /* rendered past the loop's end and added to its start */
const double MUSIC_RMS = 0.1;         /* -20 dBFS: the level everything is set against */

typedef std::vector<float> Buf;

struct Rng {
    uint32_t s;
    explicit Rng(uint32_t seed) : s(seed ? seed : 1u) {}
    uint32_t next()
    {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return s;
    }
    double uni() { return (next() >> 8) * (1.0 / 16777216.0); }
    double bi() { return uni() * 2.0 - 1.0; }
};

size_t loop_frames(void) { return (size_t)(LOOP * RATE); }

/* ---- notes ----------------------------------------------------------------------------------------------- */

/* "C4" is 60; '#' sharpens, 'b' flattens. */
int note_number(const char* t)
{
    static const int base[7] = { 9, 11, 0, 2, 4, 5, 7 };   /* A B C D E F G */
    int n = base[t[0] - 'A'];
    int i = 1;
    if (t[i] == '#') { n++; i++; }
    else if (t[i] == 'b') { n--; i++; }
    return n + (atoi(t + i) + 1) * 12;
}

double note_hz(int n) { return 440.0 * pow(2.0, (n - 69) / 12.0); }

struct Note {
    double at, len;   /* sixteenths */
    int n;
};

/* "D5:4 A4:2 r:2 | ...": a note or a rest and its length in sixteenths; '|' only marks the bars. */
std::vector<Note> notes_of(const char* s)
{
    std::vector<Note> out;
    double u = 0.0;
    while (*s) {
        while (*s == ' ' || *s == '|') s++;
        if (!*s) break;
        char tok[16];
        int k = 0;
        while (*s && *s != ' ' && *s != '|' && k < 15) tok[k++] = *s++;
        tok[k] = 0;
        char* colon = strchr(tok, ':');
        const double len = colon ? atof(colon + 1) : 1.0;
        if (colon) *colon = 0;
        if (tok[0] != 'r') out.push_back(Note{ u, len, note_number(tok) });
        u += len;
    }
    return out;
}

std::vector<int> chord_of(const char* s)
{
    std::vector<int> out;
    for (const Note& n : notes_of(s)) out.push_back(n.n);
    return out;
}

/* ---- voices ---------------------------------------------------------------------------------------------- */

struct Env {
    double a, d, s, r;   /* attack, decay, sustain level, release (s) */
    double tau;          /* plucked: the decay's time constant */
    bool pluck;
};

double env_at(const Env& e, double t, double len)
{
    double v;
    if (e.pluck) {
        v = t < e.a ? t / e.a : exp(-(t - e.a) / e.tau);
    } else {
        const double h = t < len ? t : len;
        if (h < e.a) v = h / e.a;
        else if (h < e.a + e.d) v = 1.0 - (1.0 - e.s) * (h - e.a) / e.d;
        else v = e.s;
    }
    if (t > len) {
        const double k = 1.0 - (t - len) / e.r;
        if (k <= 0.0) return 0.0;
        v *= k;
    }
    return v;
}

double blep(double t, double dt)
{
    if (t < dt) {
        t /= dt;
        return t + t - t * t - 1.0;
    }
    if (t > 1.0 - dt) {
        t = (t - 1.0) / dt;
        return t * t + t + t + 1.0;
    }
    return 0.0;
}

/* A band-limited pulse's next sample (PolyBLEP), without its DC; phase in [0, 1). */
inline double pulse_step(double& phase, double dt, double duty)
{
    double v = phase < duty ? 1.0 : -1.0;
    v += blep(phase, dt);
    double p2 = phase - duty;
    if (p2 < 0.0) p2 += 1.0;
    v -= blep(p2, dt);
    phase += dt;
    if (phase >= 1.0) phase -= 1.0;
    return v - (2.0 * duty - 1.0);
}

void pulse(Buf& b, double at, double len, double f, double duty, const Env& e, double gain)
{
    const size_t start = (size_t)(at * RATE), n = (size_t)((len + e.r) * RATE);
    double phase = 0.0;
    const double dt = f / RATE;
    for (size_t i = 0; i < n && start + i < b.size(); i++)
        b[start + i] += (float)(pulse_step(phase, dt, duty) * env_at(e, (double)i / RATE, len) * gain);
}

/* The console's triangle: 16 steps, as its 4-bit counter makes it. */
void triangle(Buf& b, double at, double len, double f, const Env& e, double gain)
{
    const size_t start = (size_t)(at * RATE), n = (size_t)((len + e.r) * RATE);
    double phase = 0.25;
    const double dt = f / RATE;
    for (size_t i = 0; i < n && start + i < b.size(); i++) {
        double v = 4.0 * fabs(phase - 0.5) - 1.0;
        v = floor((v + 1.0) * 8.0) / 15.0 * 2.0 - 1.0;
        phase += dt;
        if (phase >= 1.0) phase -= 1.0;
        b[start + i] += (float)(v * env_at(e, (double)i / RATE, len) * gain);
    }
}

/* The lead, one voice: it glides between notes that touch (an old reed's legato), attacks after a rest, puts an accent
 * on each note and a vibrato after a while on it. */
struct Line {
    double at, len;   /* seconds */
    double hz, gain;
};

void lead_line(Buf& b, const std::vector<Line>& ls, double duty, double vib, Rng& rng)
{
    if (ls.empty()) return;
    double phase = 0.0, f = ls[0].hz, amp = 0.0;
    size_t k = 0;
    const double t_end = ls.back().at + ls.back().len + 0.4;
    const double vib_phase0 = rng.uni() * 2.0 * PI;
    for (size_t i = (size_t)(ls[0].at * RATE); i < b.size() && i < (size_t)(t_end * RATE); i++) {
        const double t = (double)i / RATE;
        while (k + 1 < ls.size() && t >= ls[k + 1].at) k++;
        const Line& l = ls[k];
        const bool in = t >= l.at && t < l.at + l.len;
        const bool touches = k > 0 && ls[k - 1].at + ls[k - 1].len >= l.at - 1e-6;
        double target = in ? l.gain : 0.0;
        const double since = t - l.at;
        if (in && since < 0.15) target *= 1.0 + 0.18 * (1.0 - since / 0.15);   /* the accent */
        amp += (target - amp) * (target > amp ? 1.0 / (0.012 * RATE) : 1.0 / (0.09 * RATE));
        const double glide = (in && touches) ? 1.0 / (0.035 * RATE) : 1.0;     /* a jump after a rest */
        f += (l.hz - f) * fmin(1.0, glide);
        double fv = f;
        if (in && since > 0.25) fv *= 1.0 + vib * fmin(1.0, (since - 0.25) / 0.35) * sin(2.0 * PI * 5.3 * t + vib_phase0);
        b[i] += (float)(pulse_step(phase, fv / RATE, duty) * amp);
    }
}

/* A choir: each note three singers a little apart in pitch and in their vibrato, left, middle and right. The vowel
 * comes later, from formants over the whole part (vowels). */
void choir_note(Buf& L, Buf& R, double at, double len, double f, double gain, Rng& rng)
{
    const Env e = { 0.55, 0.0, 1.0, 0.9, 0.0, false };
    const double det[3] = { -0.0045, 0.0005, 0.0042 }, pan_l[3] = { 0.9, 0.6, 0.25 };
    const size_t start = (size_t)(at * RATE), n = (size_t)((len + e.r) * RATE);
    for (int v = 0; v < 3; v++) {
        double phase = rng.uni(), fv = f;
        const double vph = rng.uni() * 2.0 * PI, vhz = 4.3 + rng.uni() * 0.8;
        for (size_t i = 0; i < n && start + i < L.size(); i++) {
            const double t = (double)i / RATE;
            if (i % 16 == 0) fv = f * (1.0 + det[v]) * (1.0 + 0.004 * sin(2.0 * PI * vhz * t + vph));
            const double s = pulse_step(phase, fv / RATE, 0.5) * env_at(e, t, len) * gain;
            L[start + i] += (float)(s * pan_l[v]);
            R[start + i] += (float)(s * (1.15 - pan_l[v]));
        }
    }
}

struct Biquad {
    double b0 = 0, b2 = 0, a1 = 0, a2 = 0, x1 = 0, x2 = 0, y1 = 0, y2 = 0;
    void bandpass(double f, double q)
    {
        const double w = 2.0 * PI * f / RATE, al = sin(w) / (2.0 * q), a0 = 1.0 + al;
        b0 = al / a0;
        b2 = -al / a0;
        a1 = -2.0 * cos(w) / a0;
        a2 = (1.0 - al) / a0;
    }
    double run(double x)
    {
        const double y = b0 * x + b2 * x2 - a1 * y1 - a2 * y2;
        x2 = x1;
        x1 = x;
        y2 = y1;
        y1 = y;
        return y;
    }
};

/* The choir's vowel bar by bar, from "oo" (0) to "ah" (1), moving over about a second: three formants and a little
 * of the voices themselves. */
void vowels(Buf& b)
{
    static const double at_bar[NBARS] = { 0, 0, 0, 0, 0.2, 0.2, 0.2, 0.2, 0.2, 0.2, 0.2, 0.2, 1, 1, 1, 1,
                                          1, 1, 1, 1, 0.8, 0.8, 0.8, 0.8, 0.8, 0.8, 0.8, 0.8, 0.3, 0.3, 0.3, 0.3 };
    Biquad f1, f2, f3;
    double v = at_bar[0];
    for (size_t i = 0; i < b.size(); i++) {
        if (i % 32 == 0) {
            const int bar = (int)((double)i / RATE / (BAR * U)) % NBARS;
            v += (at_bar[bar] - v) * 0.004;
            f1.bandpass(300.0 + 430.0 * v, 6.0);
            f2.bandpass(870.0 + 220.0 * v, 8.0);
            f3.bandpass(2240.0 + 200.0 * v, 10.0);
        }
        const double x = b[i];
        b[i] = (float)(f1.run(x) + f2.run(x) * 0.65 + f3.run(x) * 0.25 + x * 0.12);
    }
}

/* Low brass: a pulse and a second a little above it, through a low-pass that opens with the note, the blat. */
void brass(Buf& b, double at, double len, double f, double gain)
{
    const Env e = { 0.06, 0.25, 0.75, 0.25, 0.0, false };
    const size_t start = (size_t)(at * RATE), n = (size_t)((len + e.r) * RATE);
    double p1 = 0.0, p2 = 0.37, y1 = 0.0, y2 = 0.0;
    for (size_t i = 0; i < n && start + i < b.size(); i++) {
        const double t = (double)i / RATE;
        const double env = env_at(e, t, len);
        const double x = pulse_step(p1, f / RATE, 0.5) + 0.7 * pulse_step(p2, f * 1.004 / RATE, 0.5);
        const double a = 1.0 - exp(-2.0 * PI * (250.0 + 2600.0 * env * env) / RATE);
        y1 += a * (x - y1);
        y2 += a * (y1 - y2);
        b[start + i] += (float)(y2 * env * gain);
    }
}

/* Drums. B: the war drum, deep and long; T: a tom; S: a snare, noise on a short body. */
void drum_hit(Buf& b, double at, char kind, double gain, Rng& rng)
{
    double f0, f1, decay, noise, ndecay, nlp;
    switch (kind) {
    case 'B': f0 = 95.0; f1 = 46.0; decay = 0.55; noise = 0.5; ndecay = 0.03; nlp = 0.05; break;
    case 'T': f0 = 140.0; f1 = 78.0; decay = 0.28; noise = 0.6; ndecay = 0.015; nlp = 0.15; break;
    default: f0 = 210.0; f1 = 170.0; decay = 0.06; noise = 1.3; ndecay = 0.11; nlp = 0.75; break;
    }
    const size_t start = (size_t)(at * RATE), n = (size_t)(fmax(decay, ndecay) * 6.0 * RATE);
    double phase = 0.0, lp = 0.0, prev = 0.0;
    for (size_t i = 0; i < n && start + i < b.size(); i++) {
        const double t = (double)i / RATE;
        phase += (f1 + (f0 - f1) * exp(-t / 0.06)) / RATE;
        lp += nlp * (rng.bi() - lp);
        const double nz = kind == 'S' ? (lp - prev) * 2.0 + lp * 0.5 : lp;
        prev = lp;
        const double v = sin(2.0 * PI * phase) * exp(-t / decay) + nz * noise * exp(-t / ndecay);
        b[start + i] += (float)(v * fmin(1.0, t / 0.002) * gain);
    }
}

void shaker(Buf& b, double at, Rng& rng)
{
    const size_t start = (size_t)(at * RATE), n = (size_t)(0.2 * RATE);
    double prev = 0.0;
    for (size_t i = 0; i < n && start + i < b.size(); i++) {
        const double t = (double)i / RATE, x = rng.bi();
        b[start + i] += (float)((x - prev) * 0.5 * exp(-t / 0.035) * fmin(1.0, t / 0.003));
        prev = x;
    }
}

/* ---- effects --------------------------------------------------------------------------------------------- */

void lowpass(Buf& b, double hz)
{
    const double a = 1.0 - exp(-2.0 * PI * hz / RATE);
    double y1 = 0.0, y2 = 0.0;
    for (float& x : b) {
        y1 += a * (x - y1);
        y2 += a * (y1 - y2);
        x = (float)y2;
    }
}

void highpass(Buf& b, double hz)
{
    const double a = 1.0 - exp(-2.0 * PI * hz / RATE);
    double lp = 0.0;
    for (float& x : b) {
        lp += a * (x - lp);
        x = (float)(x - lp);
    }
}

/* A high-pass over a loop: once through to find where it stands at the end, then from the start on from there. */
void highpass_loop(Buf& b, double hz)
{
    const double a = 1.0 - exp(-2.0 * PI * hz / RATE);
    double lp = 0.0;
    for (float x : b) lp += a * (x - lp);
    for (float& x : b) {
        lp += a * (x - lp);
        x = (float)(x - lp);
    }
}

void add_pan(Buf& L, Buf& R, const Buf& m, double pan, double gain)
{
    const double th = (pan + 1.0) * PI / 4.0;
    const double gl = cos(th) * gain * 1.41421356, gr = sin(th) * gain * 1.41421356;
    for (size_t i = 0; i < m.size() && i < L.size(); i++) {
        L[i] += (float)(m[i] * gl);
        R[i] += (float)(m[i] * gr);
    }
}

void add_to(Buf& dst, const Buf& src, double gain)
{
    for (size_t i = 0; i < dst.size() && i < src.size(); i++) dst[i] += (float)(src[i] * gain);
}

/* Ping-pong: left after one delay, right after two, and so on, each time fb as loud. */
void echo(Buf& L, Buf& R, const Buf& m, double delay_s, double fb, double wet)
{
    const size_t d = (size_t)(delay_s * RATE);
    std::vector<float> bl(d, 0.0f), br(d, 0.0f);   /* the last d samples of each side's line */
    size_t k = 0;
    for (size_t i = 0; i < L.size(); i++) {
        const float in = i >= d ? m[i - d] : 0.0f;
        const float l = in + (float)fb * br[k], r = bl[k];
        bl[k] = l;
        br[k] = r;
        if (++k == d) k = 0;
        L[i] += (float)(wet * l);
        R[i] += (float)(wet * r);
    }
}

/* A hall: eight combs and four all-passes a side, as in Jezar's Freeverb. */
void reverb(const Buf& in, Buf& L, Buf& R, double room, double damp)
{
    static const int combs[8] = { 1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617 };
    static const int alls[4] = { 556, 441, 341, 225 };
    const double k = RATE / 44100.0;
    for (int side = 0; side < 2; side++) {
        Buf& out = side ? R : L;
        const int spread = side ? 23 : 0;
        std::vector<Buf> cb(8), ab(4);
        size_t ci[8] = {}, ai[4] = {};
        double store[8] = {};
        for (int c = 0; c < 8; c++) cb[c].assign((size_t)((combs[c] + spread) * k), 0.0f);
        for (int a = 0; a < 4; a++) ab[a].assign((size_t)((alls[a] + spread) * k), 0.0f);
        for (size_t i = 0; i < in.size(); i++) {
            const double x = in[i] * 0.015;
            double s = 0.0;
            for (int c = 0; c < 8; c++) {
                const double o = cb[c][ci[c]];
                store[c] = o * (1.0 - damp) + store[c] * damp;
                cb[c][ci[c]] = (float)(x + store[c] * room);
                if (++ci[c] >= cb[c].size()) ci[c] = 0;
                s += o;
            }
            for (int a = 0; a < 4; a++) {
                const double bo = ab[a][ai[a]];
                ab[a][ai[a]] = (float)(s + bo * 0.5);
                if (++ai[a] >= ab[a].size()) ai[a] = 0;
                s = bo - s;
            }
            out[i] += (float)s;
        }
    }
}

double rms(const Buf& b)
{
    double s = 0.0;
    for (float x : b) s += (double)x * x;
    return b.empty() ? 0.0 : sqrt(s / b.size());
}

/* Over the samples that sound: a part's level while it plays, its rests left out. */
double active_rms(const Buf& b)
{
    double s = 0.0;
    size_t n = 0;
    for (float x : b)
        if (fabs((double)x) > 1e-3) {
            s += (double)x * x;
            n++;
        }
    return n ? sqrt(s / n) : 0.0;
}

double peak(const Buf& b)
{
    double p = 0.0;
    for (float x : b) p = fmax(p, fabs((double)x));
    return p;
}

/* ---- the theme ------------------------------------------------------------------------------------------- */

struct Chord {
    const char* lyre;    /* five notes, low to high */
    const char* choir;   /* four */
    const char* root;    /* the bass's */
    const char* fifth;
    const char* low;     /* the brass's root, an octave above the bass's */
    const char* high;    /* and its fifth */
};

const Chord Dm = { "D3 A3 D4 F4 A4", "D3 A3 D4 F4", "D2", "A2", "D3", "A3" };
const Chord Dj = { "D3 A3 D4 F#4 A4", "D3 A3 D4 F#4", "D2", "A2", "D3", "A3" };   /* D major */
const Chord Eb = { "Eb3 Bb3 Eb4 G4 Bb4", "Eb3 Bb3 Eb4 G4", "Eb2", "Bb2", "Eb3", "Bb3" };
const Chord Cj = { "C3 G3 C4 E4 G4", "C3 G3 C4 E4", "C2", "G2", "C3", "G3" };
const Chord Cm = { "C3 G3 C4 Eb4 G4", "C3 G3 C4 Eb4", "C2", "G2", "C3", "G3" };
const Chord Bb = { "Bb2 F3 Bb3 D4 F4", "Bb2 F3 Bb3 D4", "Bb2", "F2", "Bb2", "F3" };
const Chord A7 = { "A2 E3 G3 C#4 E4", "A2 E3 G3 C#4", "A2", "E2", "A2", "E3" };

/* intro 0-3, A 4-11, B 12-19, A' 20-27, outro 28-31 */
const Chord* const BARS[NBARS] = {
    &Dm, &Dm, &Eb, &Dm,
    &Dm, &Eb, &Dm, &Cj, &Bb, &Eb, &A7, &Dm,
    &Dj, &Eb, &Cm, &Dj, &Bb, &Cm, &Eb, &Dj,
    &Dm, &Eb, &Dm, &Cj, &Bb, &Eb, &A7, &Dm,
    &Dm, &Eb, &Dm, &Dm,
};

const char* const THEME_A =
    "A4:4 D5:4 F5:4 E5:2 D5:2 | Eb5:8 D5:4 Bb4:4 | A4:8 F4:4 A4:4 | G4:4 C5:4 E5:8 |"
    " F5:8 D5:4 Bb4:4 | G5:8 F5:4 Eb5:4 | E5:4 C#5:4 E5:4 G5:4 | D5:16";
const char* const THEME_B =
    "D5:4 F#5:4 A5:6 G5:2 | G5:6 F#5:2 Eb5:8 | C5:4 Eb5:4 G5:8 | F#5:4 Eb5:4 D5:8 |"
    " D5:4 F5:4 Bb5:6 A5:2 | G5:4 Eb5:4 C5:8 | Bb5:4 G5:4 F#5:4 Eb5:4 | D5:16";
const char* const OUTRO = "A4:4 D5:4 F5:4 E5:2 D5:2 | Eb5:8 D5:8";

/* The drums, a bar each, a character a sixteenth: B war drum, b a softer one, T tom, t softer, S snare. */
const char* drums_of(int bar)
{
    if (bar == 0 || bar >= 30) return "";
    if (bar <= 2) return "B...............";
    if (bar == 3) return "B.......b.......";
    if (bar <= 11) return "T.......t.....t.";
    if (bar <= 17) return "B..T..T.B..TS.T.";
    if (bar == 18) return "B..T..T.SSSSSSSS";
    if (bar == 19) return "B...............";
    if (bar <= 26) return "B...S...T...S.T.";
    if (bar == 27) return "B...S...T.T.S.TT";
    if (bar == 28) return "B.......t.......";
    return "B...............";   /* 29 */
}

enum Part { P_LEAD, P_DBL, P_LYRE, P_CHOIR, P_BASS, P_OST, P_BRASS, P_DRUM, P_SHK };

/* One part over the 32 bars into a (and b, the choir's right side). The shape of the whole: a quiet intro, A held
 * back, B the climax, A' still big, the outro falling away. */
void render_part(Part part, Buf& a, Buf& b, Rng& rng)
{
    const double bar_s = BAR * U;
    if (part == P_LEAD || part == P_DBL) {
        std::vector<Line> ls;
        auto from = [&](const char* notes, int bar, int octave, double gain) {
            for (const Note& n : notes_of(notes))
                ls.push_back(Line{ bar * bar_s + n.at * U, n.len * U, note_hz(n.n + 12 * octave), gain });
        };
        if (part == P_LEAD) {
            from(THEME_A, 4, 0, 0.85);
            from(THEME_B, 12, 0, 1.15);
            from(THEME_A, 20, 0, 1.0);
            from(OUTRO, 28, 0, 0.5);
            lead_line(a, ls, 0.25, 0.0055, rng);
        } else {
            from(THEME_B, 12, -1, 0.85);
            from(THEME_A, 20, -1, 0.8);
            lead_line(a, ls, 0.5, 0.004, rng);
        }
        return;
    }
    const Env pluck = { 0.002, 0, 0, 0.08, 0.25, true };
    const Env bass_env = { 0.03, 0.0, 1.0, 0.25, 0.0, false };
    const Env ost_env = { 0.004, 0.08, 0.55, 0.05, 0.0, false };
    const std::vector<int> ost = chord_of("D2 D2 D3 D2 D2 D2 Eb2 D2 D2 D2 D3 D2 D2 C2 D2 D2");
    for (int bn = 0; bn < NBARS; bn++) {
        const Chord& c = *BARS[bn];
        const double at = bn * bar_s;
        switch (part) {
        case P_LYRE: {   /* the intro's last two bars, A, a softer A', the outro fading */
            double g = 0.0;
            if (bn >= 2 && bn <= 3) g = 0.55;
            else if (bn >= 4 && bn <= 11) g = 0.85;
            else if (bn >= 20 && bn <= 27) g = 0.5;
            else if (bn >= 28) g = 0.65 - 0.12 * (bn - 28);
            if (g <= 0.0) break;
            const std::vector<int> v = chord_of(c.lyre);
            static const char order[] = "01234321";
            for (int k = 0; k < BAR; k++)
                pulse(a, at + k * U, U * 3.0, note_hz(v[(size_t)(order[k % 8] - '0')]), 0.125, pluck,
                      g * (k % 4 == 0 ? 1.0 : 0.8));
            break;
        }
        case P_CHOIR: {   /* every bar: soft "oo" in the intro and A, full in B */
            double g;
            if (bn <= 3) g = 0.2 + 0.06 * bn;
            else if (bn <= 11) g = 0.3;
            else if (bn <= 19) g = 1.0;
            else if (bn <= 27) g = 0.7;
            else g = 0.4 - 0.08 * (bn - 28);
            for (int n : chord_of(c.choir)) choir_note(a, b, at, bar_s, note_hz(n), g, rng);
            break;
        }
        case P_BASS:   /* a drone in the intro and outro, the root held in B, root and fifth in A and A' */
            if (bn <= 3 || bn >= 28) {
                triangle(a, at, bar_s, note_hz(note_number("D2")), bass_env, 0.6);
            } else if (bn >= 12 && bn <= 19) {
                triangle(a, at, bar_s, note_hz(note_number(c.root)), bass_env, 1.0);
            } else {
                triangle(a, at, bar_s / 2, note_hz(note_number(c.root)), bass_env, 1.0);
                triangle(a, at + bar_s / 2, bar_s / 2, note_hz(note_number(c.fifth)), bass_env, 1.0);
            }
            break;
        case P_OST:   /* on D: eighths from A's third bar, sixteenths in B and A' */
            if (bn >= 6 && bn <= 11) {
                for (int k = 0; k < BAR; k += 2)
                    pulse(a, at + k * U, U * 1.6, note_hz(ost[(size_t)k]), 0.25, ost_env, 0.45);
            } else if (bn >= 12 && bn <= 27) {
                const double g = bn <= 19 ? 1.0 : 0.7;
                for (int k = 0; k < BAR; k++)
                    pulse(a, at + k * U, U * 0.8, note_hz(ost[(size_t)k]), 0.25, ost_env, g * (k % 4 == 0 ? 1.0 : 0.8));
            }
            break;
        case P_BRASS: {   /* stabs in B and a swell at its end, a call at each bar of A' */
            const double lo = note_hz(note_number(c.low)), hi = note_hz(note_number(c.high));
            if (bn >= 12 && bn <= 18) {
                brass(a, at, 6 * U, lo, 1.0);
                brass(a, at, 6 * U, hi, 0.8);
                brass(a, at + 10 * U, 4 * U, lo, 0.85);
                brass(a, at + 10 * U, 4 * U, hi, 0.7);
            } else if (bn == 19) {
                brass(a, at, 16 * U, lo, 1.1);
                brass(a, at, 16 * U, hi, 0.9);
            } else if (bn >= 20 && bn <= 27) {
                brass(a, at, 8 * U, lo, 0.5);
                brass(a, at, 8 * U, hi, 0.4);
            }
            break;
        }
        case P_DRUM: {
            const char* d = drums_of(bn);
            const double sec = bn <= 3 ? 0.55 : bn <= 11 ? 0.8 : bn <= 19 ? 1.0 : bn <= 27 ? 0.85 : 0.55;
            for (int k = 0; d[k]; k++) {
                const char ch = d[k];
                if (ch == '.') continue;
                double g = sec * (ch == 'B' ? 1.0 : ch == 'b' ? 0.5 : ch == 'T' ? 0.75 : ch == 't' ? 0.45 : 0.55);
                if (bn == 18 && k >= 8 && ch == 'S') g = 0.3 + 0.08 * (k - 8);   /* the roll swells */
                drum_hit(a, at + k * U, ch == 'b' ? 'B' : ch == 't' ? 'T' : ch, g, rng);
            }
            break;
        }
        case P_SHK:
            if ((bn >= 6 && bn <= 11) || (bn >= 20 && bn <= 27))
                for (int k = 2; k < BAR; k += 4) shaker(a, at + k * U, rng);
            break;
        default:
            break;
        }
    }
}

/* The theme's loop at MUSIC_RMS. Each part is made in turn and mixed at a level against the lead's while each sounds.
 * 0 if *stop was set on the way. */
int render_music_loop(Buf& L, Buf& R, const volatile LONG* stop)
{
    const size_t n = loop_frames(), m = n + (size_t)(TAIL * RATE);
    Buf a(m), b(m), ml(m), mr(m), send(m);
    Rng r_lead(11);
    render_part(P_LEAD, a, b, r_lead);
    lowpass(a, 5500.0);
    const double lr = fmax(1e-9, active_rms(a)), lp = fmax(1e-9, peak(a));
    const double g_lead = 0.25 / lr;
    add_pan(ml, mr, a, 0.0, g_lead);
    echo(ml, mr, a, 3 * U, 0.25, 0.35 * g_lead);
    add_to(send, a, 0.35 * g_lead);
    struct Mix {
        Part part;
        double rel;      /* its level against the lead's: rms while sounding, or the peak's for the drums */
        double lp, hp;   /* filters, 0 none */
        double pan, echo, send;
        bool by_peak;
    };
    static const Mix mixes[] = {
        { P_DBL, 0.5, 3200, 0, -0.05, 0.0, 0.3, false },
        { P_LYRE, 0.35, 3600, 0, 0.35, 0.4, 0.3, false },
        { P_BRASS, 0.5, 0, 0, -0.1, 0.0, 0.45, false },
        { P_OST, 0.33, 1500, 0, -0.25, 0.0, 0.1, false },
        { P_BASS, 0.45, 2400, 0, 0.0, 0.0, 0.0, false },
        { P_DRUM, 0.95, 4200, 0, 0.0, 0.0, 0.3, true },
        { P_SHK, 0.1, 0, 3000, -0.3, 0.0, 0.0, true },
    };
    uint32_t seed = 12;
    for (const Mix& x : mixes) {
        if (stop && *stop) return 0;
        a.assign(m, 0.0f);
        Rng rng(seed++);
        render_part(x.part, a, b, rng);
        if (x.lp > 0.0) lowpass(a, x.lp);
        if (x.hp > 0.0) highpass(a, x.hp);
        const double g = x.by_peak ? 0.25 * lp / lr * x.rel / fmax(1e-9, peak(a)) : 0.25 * x.rel / fmax(1e-9, active_rms(a));
        add_pan(ml, mr, a, x.pan, g);
        if (x.echo > 0.0) echo(ml, mr, a, 3 * U, 0.2, x.echo * g);
        if (x.send > 0.0) add_to(send, a, x.send * g);
    }
    if (stop && *stop) return 0;
    a.assign(m, 0.0f);
    Rng r_choir(seed);
    render_part(P_CHOIR, a, b, r_choir);
    vowels(a);
    vowels(b);
    const double g_choir = 0.25 * 0.45 / fmax(1e-9, 0.5 * (active_rms(a) + active_rms(b)));
    add_to(ml, a, g_choir);
    add_to(mr, b, g_choir);
    add_to(send, a, 0.45 * g_choir);
    add_to(send, b, 0.45 * g_choir);
    Buf().swap(a);
    Buf().swap(b);
    if (stop && *stop) return 0;
    reverb(send, ml, mr, 0.86, 0.35);
    Buf().swap(send);
    for (size_t i = n; i < m; i++) {   /* past the end, into the start */
        ml[i - n] += ml[i];
        mr[i - n] += mr[i];
    }
    ml.resize(n);
    mr.resize(n);
    highpass_loop(ml, 30.0);
    highpass_loop(mr, 30.0);
    const double k = MUSIC_RMS / fmax(1e-9, 0.5 * (rms(ml) + rms(mr)));
    for (size_t i = 0; i < n; i++) {
        ml[i] = (float)(ml[i] * k);
        mr[i] = (float)(mr[i] * k);
    }
    L.swap(ml);
    R.swap(mr);
    return 1;
}

/* ---- the ambience ---------------------------------------------------------------------------------------- */

/* Waves: each rises as it comes in, opens the filter as it breaks, and draws back; the sea's bed under them. */
void waves(Buf& L, Buf& R, Rng& rng)
{
    struct Wave { double at, rise, fall, amp; };
    const double secs = L.size() / (double)RATE;
    std::vector<Wave> ws;
    for (double t = -4.0; t < secs;) {
        ws.push_back(Wave{ t, 2.0 + rng.uni() * 1.4, 3.0 + rng.uni() * 2.5, 0.55 + rng.uni() * 0.45 });
        t += 5.0 + rng.uni() * 3.5;
    }
    double l1 = 0, l2 = 0, r1 = 0, r2 = 0, bl = 0, br = 0;
    for (size_t i = 0; i < L.size(); i++) {
        const double t = i / (double)RATE;
        double e = 0.0;
        for (const Wave& w : ws) {
            const double dt = t - w.at;
            if (dt < 0.0 || dt > w.rise + w.fall * 3.0) continue;
            e += w.amp * (dt < w.rise ? (dt / w.rise) * (dt / w.rise) : exp(-(dt - w.rise) / w.fall));
        }
        const double a = 1.0 - exp(-2.0 * PI * (160.0 + 1100.0 * fmin(e, 1.4)) / RATE);
        l1 += a * (rng.bi() - l1);
        l2 += a * (l1 - l2);
        r1 += a * (rng.bi() - r1);
        r2 += a * (r1 - r2);
        bl += 0.004 * (rng.bi() - bl);
        br += 0.004 * (rng.bi() - br);
        L[i] += (float)(l2 * (0.15 + e) + bl * 2.0);
        R[i] += (float)(r2 * (0.15 + e) + br * 2.0);
    }
}

/* Wind: noise through a band that wanders, now and then stronger. */
void wind(Buf& m, Rng& rng)
{
    double low = 0, band = 0;
    for (size_t i = 0; i < m.size(); i++) {
        const double t = i / (double)RATE;
        const double fc = 520.0 * (1.0 + 0.45 * sin(2.0 * PI * t / 11.0) + 0.2 * sin(2.0 * PI * t / 4.3));
        const double f = 2.0 * sin(PI * fc / RATE);
        low += f * band;
        band += f * (rng.bi() - low - 0.35 * band);
        m[i] += (float)(band * (0.5 + 0.3 * sin(2.0 * PI * t / 17.0 + 1.0) + 0.2 * sin(2.0 * PI * t / 6.1)));
    }
}

/* The brazier: crackles at random, about four a second, each a few ms of bright noise. */
void fire(Buf& m, Rng& rng)
{
    const double secs = m.size() / (double)RATE;
    for (double t = rng.uni() * 0.3; t < secs; t += -log(1.0 - rng.uni() * 0.999) / 4.5) {
        const double amp = pow(rng.uni(), 2.0) * 0.9 + 0.1, len = 0.002 + rng.uni() * 0.01;
        const size_t start = (size_t)(t * RATE), n = (size_t)(len * 4.0 * RATE);
        double prev = 0.0;
        for (size_t i = 0; i < n && start + i < m.size(); i++) {
            const double x = rng.bi();
            m[start + i] += (float)((x - prev) * amp * exp(-(double)i / (len * RATE)));
            prev = x;
        }
    }
}

/* The ambience's loop, n frames, its seam crossfaded over 4 s, at its levels against MUSIC_RMS. */
void render_ambience_loop(Buf& L, Buf& R, size_t n)
{
    const size_t x = (size_t)(4.0 * RATE), m = n + x;
    Rng rng(31);
    Buf wl(m), wr(m), wi(m), fi(m);
    waves(wl, wr, rng);
    wind(wi, rng);
    fire(fi, rng);
    highpass(fi, 1800.0);
    const double kw = 0.30 * MUSIC_RMS / fmax(1e-9, 0.5 * (rms(wl) + rms(wr)));
    const double ki = 0.08 * MUSIC_RMS / fmax(1e-9, rms(wi)) * 0.9;
    const double kf = 0.8 * MUSIC_RMS / fmax(1e-9, peak(fi));
    L.assign(n, 0.0f);
    R.assign(n, 0.0f);
    for (size_t i = 0; i < n; i++) {
        /* the 4 s past the end fade out over the first 4, which fade in: the end runs into the start */
        double a = 1.0, b = 0.0;
        if (i < x) {
            a = sin(0.5 * PI * (double)i / x);
            b = cos(0.5 * PI * (double)i / x);
        }
        const size_t j = n + i;
        auto at = [&](const Buf& s) { return s[i] * a + (i < x ? s[j] * b : 0.0); };
        const double w = at(wi) * ki, f = at(fi) * kf;
        L[i] = (float)(at(wl) * kw + w + f * 0.8);
        R[i] = (float)(at(wr) * kw + w + f * 0.55);
    }
    highpass_loop(L, 30.0);
    highpass_loop(R, 30.0);
}

/* Far thunder, one roll of about 5 s: a low rumble that swells and rolls away, its peak at 1.8 times MUSIC_RMS. */
void render_thunder(Buf& L, Buf& R, uint32_t seed)
{
    const size_t n = (size_t)(5.0 * RATE);
    L.assign(n, 0.0f);
    R.assign(n, 0.0f);
    Rng rng(seed);
    double l1 = 0, l2 = 0, r1 = 0, r2 = 0, mod = 0.5, target = 0.5;
    const double a = 1.0 - exp(-2.0 * PI * 140.0 / RATE);
    for (size_t i = 0; i < n; i++) {
        const double t = i / (double)RATE;
        if (i % 2400 == 0) target = 0.3 + 0.7 * rng.uni();   /* it rolls, twenty times a second */
        mod += 0.0008 * (target - mod);
        l1 += a * (rng.bi() - l1);
        l2 += a * (l1 - l2);
        r1 += a * (rng.bi() - r1);
        r2 += a * (r1 - r2);
        const double e = (t < 0.35 ? t / 0.35 : exp(-(t - 0.35) / 1.2)) * mod;
        L[i] = (float)(l2 * e);
        R[i] = (float)(r2 * e);
    }
    const double k = 1.8 * MUSIC_RMS / fmax(1e-9, fmax(peak(L), peak(R)));
    for (size_t i = 0; i < n; i++) {
        L[i] = (float)(L[i] * k);
        R[i] = (float)(R[i] * k);
    }
}

std::vector<int16_t> pcm16(const Buf& L, const Buf& R)
{
    std::vector<int16_t> out(L.size() * 2);
    for (size_t i = 0; i < L.size(); i++) {
        out[2 * i] = (int16_t)lrint(fmax(-1.0, fmin(1.0, (double)L[i])) * 32767.0);
        out[2 * i + 1] = (int16_t)lrint(fmax(-1.0, fmin(1.0, (double)R[i])) * 32767.0);
    }
    return out;
}

const ULONGLONG START_MS = 2500;                       /* on the screen, before it plays */
const ULONGLONG THUNDER_MS = 6000 + 1200, THUNDER_EVERY_MS = 9500;   /* after the screen's lightning */

/* ---- playing it ------------------------------------------------------------------------------------------ */

HANDLE s_thread, s_stop_event;
volatile LONG s_stop;
ULONGLONG s_t0;
double s_level;

typedef HRESULT(WINAPI* xaudio2_create_fn)(IXAudio2**, UINT32, XAUDIO2_PROCESSOR);

/* At normal priority: below it, the build (normal, on every core) would hold it off until it ended. It takes a
 * core for a few seconds of a build of a minute or two. */
DWORD WINAPI music_main(LPVOID)
{
    const ULONGLONG made0 = GetTickCount64();
    Buf l, r;
    if (!render_music_loop(l, r, &s_stop)) {
        printf("[music] the screen went before it played\n");
        return 0;
    }
    const std::vector<int16_t> music = pcm16(l, r);
    render_ambience_loop(l, r, loop_frames());
    const std::vector<int16_t> sea = pcm16(l, r);
    std::vector<int16_t> thunder[3];
    for (int k = 0; k < 3; k++) {
        render_thunder(l, r, 77u + (uint32_t)k);
        thunder[k] = pcm16(l, r);
    }
    Buf().swap(l);
    Buf().swap(r);
    printf("[music] made in %.1f s: the theme's %.1f s loop, the sea's, three rolls of thunder\n",
           (GetTickCount64() - made0) / 1000.0, LOOP);
    for (;;) {   /* not before START_MS into the screen */
        const ULONGLONG now = GetTickCount64();
        if (s_stop) {
            printf("[music] the screen went before it played\n");
            return 0;
        }
        if (now >= s_t0 + START_MS) break;
        WaitForSingleObject(s_stop_event, (DWORD)(s_t0 + START_MS - now));
    }

    const HRESULT co = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    IXAudio2* xa = NULL;
    IXAudio2MasteringVoice* master = NULL;
    IXAudio2SourceVoice *v_music = NULL, *v_sea = NULL, *v_thunder = NULL;
    HMODULE dll = LoadLibraryExW(L"xaudio2_9.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    xaudio2_create_fn create = dll ? (xaudio2_create_fn)(void*)GetProcAddress(dll, "XAudio2Create") : NULL;
    WAVEFORMATEX wf = {};
    wf.wFormatTag = WAVE_FORMAT_PCM;
    wf.nChannels = 2;
    wf.nSamplesPerSec = RATE;
    wf.wBitsPerSample = 16;
    wf.nBlockAlign = 4;
    wf.nAvgBytesPerSec = RATE * 4;
    HRESULT hr = create ? create(&xa, 0, XAUDIO2_DEFAULT_PROCESSOR) : E_FAIL;
    if (SUCCEEDED(hr)) hr = xa->CreateMasteringVoice(&master);
    if (SUCCEEDED(hr)) hr = xa->CreateSourceVoice(&v_music, &wf);
    if (SUCCEEDED(hr)) hr = xa->CreateSourceVoice(&v_sea, &wf);
    if (SUCCEEDED(hr)) hr = xa->CreateSourceVoice(&v_thunder, &wf);
    if (SUCCEEDED(hr)) {
        XAUDIO2_BUFFER b = {};
        b.LoopCount = XAUDIO2_LOOP_INFINITE;
        b.AudioBytes = (UINT32)(music.size() * 2);
        b.pAudioData = (const BYTE*)music.data();
        hr = v_music->SubmitSourceBuffer(&b);
        b.AudioBytes = (UINT32)(sea.size() * 2);
        b.pAudioData = (const BYTE*)sea.data();
        if (SUCCEEDED(hr)) hr = v_sea->SubmitSourceBuffer(&b);
    }
    if (FAILED(hr)) {
        printf("[music] no sound: XAudio2 %s (0x%08lX)\n", dll ? "did not start" : "is not there", (unsigned long)hr);
    } else {
        master->SetVolume((float)s_level);
        v_music->SetVolume(0.0f);
        v_sea->SetVolume(0.0f);
        v_music->Start();
        v_sea->Start();
        const ULONGLONG start = GetTickCount64();
        unsigned k = 0;
        while (s_t0 + THUNDER_MS + k * THUNDER_EVERY_MS < start) k++;
        unsigned rolls = 0;
        float vol = 0.0f;
        printf("[music] playing (%s)\n", ps3_settings_str("system", "precompile_music", "GOWA_PRECOMPILE_MUSIC", "normal"));
        for (;;) {
            const ULONGLONG now = GetTickCount64();
            vol = (float)fmin(1.0, (now - start) / 2500.0);   /* in over 2.5 s */
            v_music->SetVolume(vol);
            v_sea->SetVolume(vol);
            v_thunder->SetVolume(vol);
            const ULONGLONG thunder_at = s_t0 + THUNDER_MS + k * THUNDER_EVERY_MS;
            if (now >= thunder_at) {
                const std::vector<int16_t>& t = thunder[k % 3];
                XAUDIO2_BUFFER b = {};
                b.AudioBytes = (UINT32)(t.size() * 2);
                b.pAudioData = (const BYTE*)t.data();
                v_thunder->Stop();
                v_thunder->FlushSourceBuffers();
                v_thunder->SubmitSourceBuffer(&b);
                v_thunder->Start();
                rolls++;
                k++;
                continue;
            }
            const ULONGLONG step = vol < 1.0f ? 40 : 1000;   /* the fade's steps, then a second at a time */
            const ULONGLONG wait = thunder_at - now < step ? thunder_at - now : step;
            if (WaitForSingleObject(s_stop_event, (DWORD)wait) == WAIT_OBJECT_0) break;
        }
        const ULONGLONG fade0 = GetTickCount64();   /* out over a second, from where it is */
        for (;;) {
            const double t = (GetTickCount64() - fade0) / 1000.0;
            if (t >= 1.0) break;
            const float g = vol * (float)(1.0 - t);
            v_music->SetVolume(g);
            v_sea->SetVolume(g);
            v_thunder->SetVolume(g);
            Sleep(20);
        }
        XAUDIO2_VOICE_STATE st = {};
        v_music->GetState(&st);
        printf("[music] faded out after %.1f s played, %u rolls of thunder\n", (double)st.SamplesPlayed / RATE, rolls);
    }
    if (v_thunder) v_thunder->DestroyVoice();
    if (v_sea) v_sea->DestroyVoice();
    if (v_music) v_music->DestroyVoice();
    if (master) master->DestroyVoice();
    if (xa) xa->Release();
    if (SUCCEEDED(co)) CoUninitialize();
    return 0;
}

void write_wav(const char* path, const std::vector<int16_t>& pcm)
{
    FILE* f = fopen(path, "wb");
    if (!f) return;
    const uint32_t bytes = (uint32_t)(pcm.size() * 2), riff = 36 + bytes;
    const uint32_t fmt[] = { 16, 0x00020001, (uint32_t)RATE, (uint32_t)RATE * 4, 0x00100004 };
    fwrite("RIFF", 1, 4, f);
    fwrite(&riff, 4, 1, f);
    fwrite("WAVEfmt ", 1, 8, f);
    fwrite(fmt, 4, 5, f);
    fwrite("data", 1, 4, f);
    fwrite(&bytes, 4, 1, f);
    fwrite(pcm.data(), 2, pcm.size(), f);
    fclose(f);
}

}  // namespace

void gowa_music_begin(uint64_t t0_ms)
{
    if (s_thread) return;
    /* Normal is the level it was made at (MUSIC_RMS, -17.2 LUFS with the sea): full, its true peak with the thunder
     * -4.6 dBFS. Low is half that, high 2 dB over it and still under -2.5 dBFS. medium, from before, is normal. */
    const char* v = ps3_settings_str("system", "precompile_music", "GOWA_PRECOMPILE_MUSIC", "normal");
    double level = 1.0;
    if (ps3_settings_ieq(v, "off")) level = 0.0;
    else if (ps3_settings_ieq(v, "low")) level = 0.5;
    else if (ps3_settings_ieq(v, "high")) level = 1.25;
    if (level <= 0.0) {
        printf("[music] off (system.precompile_music)\n");
        return;
    }
    s_level = level;
    s_t0 = t0_ms;
    s_stop_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    s_thread = CreateThread(NULL, 0, music_main, NULL, 0, NULL);
}

void gowa_music_end(void)
{
    if (!s_thread) return;
    InterlockedExchange(&s_stop, 1);
    SetEvent(s_stop_event);   /* not waited for: it fades out on its own thread */
}

int gowa_music_preview(const char* path, double seconds)
{
    const ULONGLONG made0 = GetTickCount64();
    Buf ml, mr, al, ar;
    render_music_loop(ml, mr, NULL);
    render_ambience_loop(al, ar, ml.size());
    Buf tl[3], tr[3];
    for (int k = 0; k < 3; k++) render_thunder(tl[k], tr[k], 77u + (uint32_t)k);
    printf("made in %.1f s\n", (GetTickCount64() - made0) / 1000.0);
    /* as the screen plays it: silence for START_MS, in over 2.5 s, thunder after the lightning */
    const size_t total = (size_t)(fmax(seconds, 1.0) * RATE), n = ml.size(), s0 = (size_t)(START_MS / 1000.0 * RATE);
    Buf L(total, 0.0f), R(total, 0.0f);
    for (size_t i = s0; i < total; i++) {
        const double g = fmin(1.0, (double)(i - s0) / (2.5 * RATE));
        const size_t j = (i - s0) % n;
        L[i] = (float)((ml[j] + al[j]) * g);
        R[i] = (float)((mr[j] + ar[j]) * g);
    }
    unsigned k = 0;
    while ((THUNDER_MS + k * THUNDER_EVERY_MS) * RATE / 1000 < total) {
        const size_t at = (size_t)((THUNDER_MS + k * THUNDER_EVERY_MS) * RATE / 1000);
        if (at >= s0)
            for (size_t i = 0; i < tl[k % 3].size() && at + i < total; i++) {
                L[at + i] += tl[k % 3][i];
                R[at + i] += tr[k % 3][i];
            }
        k++;
    }
    write_wav(path, pcm16(L, R));
    printf("%s: %.1f s\n", path, total / (double)RATE);
    return 0;
}
