#pragma once
#include <cmath>
#include <algorithm>

// All the waveshapers live here. Every function is sample-by-sample, real-time safe,
// and takes `mod` in the range 0..1.
namespace mbd
{
constexpr float kPi = 3.14159265358979f;

// NOTE: the order here is the order of the dropdown. Saved sessions store the index.
enum DistType
{
    Tape = 0,      // Langevin (magnetisation) curve,        Mod = bias
    Tube,          // asymmetric triode-ish curve,           Mod = bias
    LogSoft,       // symmetric log waveshaper,              Mod = asymmetry
    SineFold,      // sin() wavefolder,                      Mod = asymmetry
    TriFold,       // triangle wavefolder,                   Mod = asymmetry
    HardClip,      // hard clipper,                          Mod = fold-back amount
    SlewLimiter,   // slew-rate limiter,                     Mod = slew limit (higher = slower)
    Decimator,     // sample & hold,                         Mod = lower sample rate
    Bitcrusher,    // quantiser,                             Mod = fewer bits
    Fractal,       // Takagi/Weierstrass-style folding sum,  Mod = number of passes (1..8)
    Tanh,          // hyperbolic tangent,                    Mod = asymmetry
    BJT,           // long-tailed pair with degeneration,    Mod = emitter degeneration
    Diode,         // Shockley diode-pair clipper,           Mod = asymmetry
    Rectifier,     // half-wave <-> full-wave rectifier,     Mod = half -> full
    Crossover,     // class-B crossover (dead-zone),         Mod = dead-zone width
    Chebyshev,     // Chebyshev polynomial waveshaper,       Mod = order (1..8, cross-faded)
    AmpCrunch,     // amp-modeller "Crunch" (fitted to a recording), Mod = presence
    AmpLead,       // amp-modeller "Lead"   (fitted to a recording), Mod = presence
    NumTypes
};

// Filter / clipper state of the two amp models (see ampProcess below).
struct AmpFx
{
    float hpX = 0.f, hpY = 0.f;          // pre high-pass
    float hp2X = 0.f, hp2Y = 0.f;        // inter-stage high-pass (Lead)
    float lp1Y = 0.f;                    // inter-stage low-pass (Lead)
    float z1 = 0.f, z2 = 0.f;            // output biquad low-pass (TDF-II)
    float fs = 0.f, presence = -1.f;     // what the cached coefficients below were made for
    float hpK = 0.f, hp2K = 0.f, lp1K = 0.f;
    float b0 = 0.f, b1 = 0.f, b2 = 0.f, a1 = 0.f, a2 = 0.f;
};

struct DistState
{
    AmpFx amp;
    float slewY = 0.f;     // slew limiter output
    float hold  = 0.f;     // decimator held sample
    float phase = 0.f;     // decimator phase accumulator
    float osRatio = 1.f;   // oversampling factor the shaper is running at (keeps Decimator's pitch independent of it)
};

// Triangle wave "fold": identity-ish around 0, reflects at +/-1, period 4.
inline float triFold (float v) noexcept
{
    float t = v + 1.f;
    t -= 4.f * std::floor (t * 0.25f);
    return 1.f - std::abs (t - 2.f);
}

inline float tubeCurve (float v) noexcept
{
    // Positive half saturates to +1, negative half to -0.667, both with unit slope at 0.
    return v >= 0.f ? 1.f - std::exp (-v)
                    : -(1.f - std::exp (1.5f * v)) / 1.5f;
}

inline float logCurve (float v) noexcept
{
    constexpr float a = 6.f;
    const float y = std::log1p (a * std::abs (v)) / std::log1p (a);
    return v < 0.f ? -y : y;
}

inline float sineCurve (float v) noexcept { return std::sin (0.5f * kPi * v); }

// Langevin function L(s) = coth(s) - 1/s, the anhysteretic magnetisation curve used in
// Jiles-Atherton tape models. Slope 1/3 at the origin, saturates at +/-1 with a long, soft
// 1/s knee (much more gradual than tanh's exponential approach). Series below |s| = 0.05
// to avoid the coth(s) - 1/s cancellation.
inline float langevin (float s) noexcept
{
    if (std::abs (s) < 0.05f)
    {
        const float s2 = s * s;
        return s * (1.f / 3.f - s2 * (1.f / 45.f - s2 * (2.f / 945.f)));
    }
    return 1.f / std::tanh (s) - 1.f / s;
}

inline float tapeCurve (float x) noexcept { return langevin (3.f * x); }   // unit slope at 0

// Fractal: Takagi-style sum  sum_n 0.5^n * tri(2^n x). Each extra pass adds a
// self-similar, twice-as-fast, half-as-loud copy of the fold. The last pass is
// cross-faded in so that sweeping Mod is smooth.
inline float fractal (float x, float mod) noexcept
{
    const float passes = 1.f + 7.f * mod;
    const int   whole  = (int) passes;
    const float frac   = passes - (float) whole;

    float acc = 0.f, norm = 0.f, w = 1.f, s = 1.f;
    for (int n = 0; n < 8; ++n)
    {
        const float pw = n < whole ? 1.f : (n == whole ? frac : 0.f);
        if (pw <= 0.f) break;
        acc  += pw * w * triFold (s * x);
        norm += pw * w;
        w *= 0.5f;
        s *= 2.f;
    }
    return acc / norm;
}

// Differential (long-tailed) pair: Iout = tanh(u), Vin = u + d*tanh(u), where d models
// emitter degeneration resistors. Solved with a few Newton steps.
inline float bjtPair (float x, float mod) noexcept
{
    const float d = 6.f * mod;
    const float v = 2.f * x;
    float u = v / (1.f + d);
    for (int i = 0; i < 4; ++i)
    {
        const float t  = std::tanh (u);
        const float f  = u + d * t - v;
        const float fp = 1.f + d * (1.f - t * t);
        u -= f / fp;
    }
    return std::tanh (u) * std::sqrt (1.f + d);   // partial make-up gain for the degeneration
}

// ---------------------------------------------------------------------------------------------
// Diode clipper: a resistor feeding two anti-parallel diodes to ground. The output voltage y
// satisfies   v = y + R*Ip*(exp(y/Vt) - 1) - R*In*(exp(-y/Vt) - 1)
// (Shockley equation, series resistance R). Solved exactly with Newton's method; the start
// point min(v, Vt*ln(1+v/a)) always lies on the right of the root of this convex function, so
// the iteration converges monotonically and 4 steps are plenty. Mod lowers the saturation
// current of the negative-going diode, which pushes its knee out and makes the clipping lopsided.
inline float diodeSolvePositive (float v, float ap, float an) noexcept    // v >= 0
{
    constexpr float Vt = 0.0525f;   // thermal voltage x ideality factor (silicon switching diode)
    float y = std::min (v, Vt * std::log1p (v / ap));
    for (int i = 0; i < 4; ++i)
    {
        const float ep = std::exp (y / Vt);
        const float en = 1.f / ep;
        const float f  = y + ap * (ep - 1.f) - an * (en - 1.f) - v;
        const float fp = 1.f + (ap * ep + an * en) / Vt;
        y -= f / fp;
    }
    return y;
}

inline float diodeClip (float x, float mod) noexcept
{
    constexpr float a     = 5.5e-6f;   // R * Is, in volts
    constexpr float scale = 2.f;       // full scale = 2 V, so the ~0.55 V knee sits at about -11 dBFS
    const float ap = a;
    const float an = a * std::exp (-10.f * mod);

    const float v = std::clamp (x * scale, -1000.f, 1000.f);
    const float y = v >= 0.f ?  diodeSolvePositive ( v, ap, an)
                             : -diodeSolvePositive (-v, an, ap);
    return y / scale;
}

// ---------------------------------------------------------------------------------------------
// |x| with the corner at zero rounded off (sub-millivolt), so the rectifier doesn't throw
// unbounded harmonics from an infinitely sharp kink.
inline float smoothAbs (float x) noexcept { return std::sqrt (x * x + 1.0e-4f) - 1.0e-2f; }

// Chebyshev polynomial waveshaper. T_n(cos t) = cos(n t), so a unit sine in gives exactly the
// n-th harmonic out. Three things keep it well behaved:
//  * T_n explodes outside [-1, 1], so the input is folded back into range (triFold) instead of
//    clipped. Clipping looks natural but is a trap: a driven, clipped input sits at +/-1, where
//    every even-order T_n is a constant, so the band would go silent at high Drive. Folding keeps
//    the polynomial sweeping its whole range at any drive, and is exact (no distortion of its own)
//    for inputs inside +/-1;
//  * the value T_n(0) = cos(n*pi/2) is subtracted, otherwise even orders sit at +/-1 when the
//    input is silent;
//  * Mod cross-fades T_n and T_(n+1) (order 1..8), which is the same thing as interpolating
//    their coefficients.
inline float chebyshev (float x, float mod) noexcept
{
    const float s   = triFold (x);
    const float pos = 1.f + 7.f * std::clamp (mod, 0.f, 1.f);
    const int   n   = std::min ((int) pos, 7);
    const float f   = pos - (float) n;

    float tPrev = 1.f, t = s;                                   // T0, T1
    for (int k = 1; k < n; ++k)
    {
        const float tNext = 2.f * s * t - tPrev;
        tPrev = t;
        t = tNext;
    }
    const float tNext = 2.f * s * t - tPrev;                    // t = T_n, tNext = T_(n+1)

    constexpr float zero[4] = { 1.f, 0.f, -1.f, 0.f };          // cos(k*pi/2)
    const float z0 = zero[n & 3], z1 = zero[(n + 1) & 3];

    return (t + (tNext - t) * f) - (z0 + (z1 - z0) * f);
}


// ---------------------------------------------------------------------------------------------
// Amp models. Fitted to two recordings of a guitar-amp modeller: a 261.6 Hz sine with the amp's
// Drive swept 0 -> 10. Harmonic phases were constant over the sweep, so each amp is a static clipper
// with linear filters around it (not a single curve): high-pass -> gain -> soft clipper -> low-pass.
//   Crunch: one clipper stage.        Lead: two clipper stages with a filter between them.
// Clipper: v / (1 + |v|^p)^(1/p) with a small bias for the even harmonics (DC removed).
// ---------------------------------------------------------------------------------------------
inline bool isAmp (int type) noexcept { return type == AmpCrunch || type == AmpLead; }

// Pre-gain of the clipper as a function of Drive (0..10), from the recordings. The returned value
// is relative to a 0.3 reference level at the shaper input, so that the band level the rest of the
// plug-in works with lands in the range the amp was recorded at.
inline float ampDriveGain (int type, float drive) noexcept
{
    const float d = std::clamp (drive, 0.f, 10.f);
    const float lg = type == AmpCrunch ? (0.01129147f * d + 0.11761154f) * d - 0.67244123f
                                       : (0.03150621f * d + 0.31529737f) * d - 2.13485161f;
    return std::exp (lg) * (1.f / 0.3f);
}

inline float ampClip (float v, float p) noexcept
{
    return v * std::pow (1.f + std::pow (std::abs (v), p), -1.f / p);
}

inline float ampProcess (int type, float x, float mod, AmpFx& a, float fs) noexcept
{
    const bool lead = type == AmpLead;

    // Fitted constants (crunch / lead).
    const float p1 = lead ? 2.5915f : 1.1945f,  b1 = lead ? 0.0190f : 0.0738f;
    const float p2 = 3.1017f,                   b2 = 0.0695f,  k2 = 1.9608f;
    const float fHp  = lead ? 188.46f : 715.0f;
    const float fHp2 = 121.37f,  fLp1 = 6822.7f;
    const float fLp  = lead ? 2611.5f : 2403.0f;
    const float q    = lead ? 0.30f : 0.50f;
    const float outG = lead ? 0.5083f : 0.5305f;

    if (a.fs != fs || std::abs (a.presence - mod) > 1.0e-3f)
    {
        a.fs = fs;  a.presence = mod;
        auto onePole = [fs] (float f) { return std::exp (-2.f * kPi * f / fs); };
        a.hpK = onePole (fHp);  a.hp2K = onePole (fHp2);  a.lp1K = onePole (fLp1);
        // Presence opens the output low-pass up to 4x its recorded cutoff.
        const float fc = std::min (fLp * std::exp2 (2.f * mod), 0.45f * fs);
        const float w = 2.f * kPi * fc / fs, c = std::cos (w), al = std::sin (w) / (2.f * q);
        const float inv = 1.f / (1.f + al);
        a.b0 = (1.f - c) * 0.5f * inv;  a.b1 = (1.f - c) * inv;  a.b2 = a.b0;
        a.a1 = -2.f * c * inv;          a.a2 = (1.f - al) * inv;
    }

    // pre high-pass
    float y = 0.5f * (1.f + a.hpK) * (x - a.hpX) + a.hpK * a.hpY;
    a.hpX = x;  a.hpY = y;

    y = ampClip (y + b1, p1) - ampClip (b1, p1);

    if (lead)
    {
        const float h = 0.5f * (1.f + a.hp2K) * (y - a.hp2X) + a.hp2K * a.hp2Y;
        a.hp2X = y;  a.hp2Y = h;
        a.lp1Y += (1.f - a.lp1K) * (h - a.lp1Y);
        y = ampClip (a.lp1Y * k2 + b2, p2) - ampClip (b2, p2);
    }

    // output low-pass (TDF-II biquad)
    const float o = a.b0 * y + a.z1;
    a.z1 = a.b1 * y - a.a1 * o + a.z2;
    a.z2 = a.b2 * y - a.a2 * o;
    return o * outG;
}

inline float process (int type, float x, float mod, DistState& s, float fs) noexcept
{
    switch (type)
    {
        case Tape:
        {
            const float b = 0.5f * mod;
            return tapeCurve (x + b) - tapeCurve (b);
        }
        case Tube:
        {
            const float b = 0.8f * mod;
            return tubeCurve (x + b) - tubeCurve (b);
        }
        case LogSoft:
        {
            const float b = 0.5f * mod;
            return logCurve (x + b) - logCurve (b);
        }
        case SineFold:
        {
            const float b = 0.5f * mod;
            return sineCurve (x + b) - sineCurve (b);
        }
        case TriFold:
        {
            const float b = 0.5f * mod;
            return triFold (x + b) - triFold (b);
        }
        case HardClip:
        {
            const float c      = std::clamp (x, -1.f, 1.f);
            const float excess = x - c;
            return std::clamp (c - mod * excess, -1.f, 1.f);   // mod=1 reflects the overshoot
        }
        case SlewLimiter:
        {
            const float rate   = 200000.f * std::pow (300.f / 200000.f, mod);   // units per second
            const float maxD   = rate / fs;
            s.slewY += std::clamp (x - s.slewY, -maxD, maxD);
            return s.slewY;
        }
        case Decimator:
        {
            s.phase += std::exp2 (-7.f * mod) / s.osRatio;   // 1 .. 1/128 of the *base* sample rate
            if (s.phase >= 1.f)
            {
                s.phase -= 1.f;
                s.hold = std::clamp (x, -1.f, 1.f);   // converter clips when over-driven
            }
            return s.hold;
        }
        case Bitcrusher:
        {
            const float bits = 16.f - 15.f * mod;
            const float q    = std::exp2 (bits - 1.f);
            return std::round (std::clamp (x, -1.f, 1.f) * q) / q;
        }
        case Fractal: return fractal (x, mod);
        case Tanh:
        {
            const float b = 0.6f * mod;
            return std::tanh (x + b) - std::tanh (b);
        }
        case BJT: return bjtPair (x, mod);
        case Diode: return diodeClip (x, mod);
        case Rectifier:
        {
            // Mod 0 = half-wave (negative half removed), Mod 1 = full-wave (|x|). The tanh keeps the
            // output bounded so that Drive saturates the rectified wave instead of just scaling it.
            const float full = smoothAbs (x);
            const float half = 0.5f * (x + full);
            return std::tanh (half + (full - half) * mod);
        }
        case Crossover:
        {
            // Soft dead zone around zero (x - dz*tanh(x/dz): ~x^3 inside, x - dz outside), followed by
            // the amp's rail clipping.
            const float dz = 0.6f * mod * mod;
            const float y  = dz > 1.0e-4f ? x - dz * std::tanh (x / dz) : x;
            return std::tanh (y);
        }
        case Chebyshev: return chebyshev (x, mod);
        case AmpCrunch:
        case AmpLead:   return ampProcess (type, x, mod, s.amp, fs);
        default:  return x;
    }
}
} // namespace mbd
