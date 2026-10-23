#pragma once
#include "Common.h"
#include "PluginProcessor.h"

// Live FFT spectrum (red line) with four translucent band bars.
//  - bar horizontal extent = the band's frequency range (edges are the crossover points)
//  - bar vertical position = the band's output Level (-60..+12 dB, full view height)
// The bars are draggable: grab a band edge to move the crossover, drag inside a bar to set its Level.
class SpectrumView : public juce::Component
{
public:
    static constexpr int fftOrder = 12;
    static constexpr int fftSize  = 1 << fftOrder;

    explicit SpectrumView (MBDProcessor& p) : proc (p)
    {
        for (int b = 0; b < 4; ++b)
        {
            const auto id = juce::String ("b") + juce::String (b + 1);
            gainP[b]     = p.apvts.getRawParameterValue (id + "Level");
            muteP[b]     = p.apvts.getRawParameterValue (id + "Mute");
            gainParam[b] = p.apvts.getParameter (id + "Level");
        }
        specColP = p.apvts.getRawParameterValue ("specColour");

        for (int i = 0; i < 3; ++i)
            xoverParam[i] = p.apvts.getParameter ("xover" + juce::String (i + 1));

        prePost.setClickingTogglesState (true);
        prePost.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xffe5383b));
        prePost.onStateChange = [this] { prePost.setButtonText (prePost.getToggleState() ? "POST" : "PRE"); };
        addAndMakeVisible (prePost);
        prePostAtt = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (p.apvts, "specPost", prePost);
        prePost.onStateChange();

        setMouseCursor (juce::MouseCursor::UpDownResizeCursor);
    }

    // Called ~30x per second from the editor.
    void update()
    {
        const int w = getWidth();
        if (w <= 0) return;

        proc.copyLatestSamples (fftData.data(), fftSize);
        std::fill (fftData.begin() + fftSize, fftData.end(), 0.f);
        window.multiplyWithWindowingTable (fftData.data(), fftSize);
        fft.performFrequencyOnlyForwardTransform (fftData.data());

        double sr = proc.getSampleRate();
        if (sr <= 0.0) sr = 44100.0;

        if ((int) smoothDb.size() != w) smoothDb.assign ((size_t) w, -100.f);

        const int   maxBin = fftSize / 2 - 1;
        const float norm   = (float) fftSize * 0.25f;   // full-scale sine under a Hann window -> 0 dB

        for (int x = 0; x < w; ++x)
        {
            const float f0 = minFreq * std::pow (maxFreq / minFreq, (float) x / (float) w);
            const float f1 = minFreq * std::pow (maxFreq / minFreq, (float) (x + 1) / (float) w);
            const float b0 = f0 * (float) fftSize / (float) sr;
            const float b1 = f1 * (float) fftSize / (float) sr;

            float m = 0.f;
            if (b1 - b0 < 1.f)
            {
                const float c = 0.5f * (b0 + b1);
                const int   i = juce::jlimit (0, maxBin - 1, (int) c);
                const float fr = c - (float) i;
                m = fftData[(size_t) i] * (1.f - fr) + fftData[(size_t) i + 1] * fr;
            }
            else
            {
                const int lo = juce::jlimit (0, maxBin, (int) b0);
                const int hi = juce::jlimit (lo, maxBin, (int) b1);
                for (int i = lo; i <= hi; ++i) m = juce::jmax (m, fftData[(size_t) i]);
            }

            const float db = juce::Decibels::gainToDecibels (m / norm, -120.f);
            smoothDb[(size_t) x] = juce::jmax (db, smoothDb[(size_t) x] - 2.5f);   // instant rise, gentle fall
        }
        repaint();
    }

    void resized() override { prePost.setBounds (getWidth() - 64, 6, 56, 20); }

    //==========================================================================
    void paint (juce::Graphics& g) override
    {
        const auto bounds = getLocalBounds().toFloat();
        const float w = bounds.getWidth(), h = bounds.getHeight();

        g.setColour (juce::Colour (0xff0e0f12));
        g.fillRoundedRectangle (bounds, 6.f);

        // grid
        g.setFont (10.f);
        for (float f : { 30.f, 50.f, 100.f, 200.f, 300.f, 500.f, 1000.f, 2000.f, 3000.f, 5000.f, 10000.f })
        {
            const bool major = (f == 100.f || f == 1000.f || f == 10000.f);
            const float x = freqToX (f);
            g.setColour (juce::Colours::white.withAlpha (major ? 0.14f : 0.05f));
            g.drawVerticalLine ((int) x, 0.f, h);
            if (major)
            {
                g.setColour (juce::Colours::white.withAlpha (0.35f));
                g.drawText (f >= 1000.f ? juce::String ((int) (f / 1000.f)) + "k" : juce::String ((int) f),
                            (int) x + 3, (int) h - 14, 30, 12, juce::Justification::left);
            }
        }
        for (int db = -80; db <= 0; db += 20)
        {
            const float y = dbToY ((float) db);
            g.setColour (juce::Colours::white.withAlpha (0.06f));
            g.drawHorizontalLine ((int) y, 0.f, w);
            g.setColour (juce::Colours::white.withAlpha (0.3f));
            g.drawText (juce::String (db), 3, (int) y - 12, 28, 12, juce::Justification::left);
        }

        // band bars
        float f1, f2, f3;
        proc.getCrossovers (f1, f2, f3);
        const float edges[5] = { minFreq, f1, f2, f3, maxFreq };
        static const juce::Colour cols[4] = { juce::Colour (0xff4da3ff), juce::Colour (0xff3ddc84),
                                              juce::Colour (0xffe8c33a), juce::Colour (0xffb57bff) };

        for (int b = 0; b < 4; ++b)
        {
            const float x0 = freqToX (edges[b]), x1 = freqToX (edges[b + 1]);
            const float gdb = gainP[b]->load();
            const bool muted = muteP[b]->load() > 0.5f;

            juce::Rectangle<float> bar (x0 + 1.f, gainToY (gdb) - 8.f, juce::jmax (2.f, x1 - x0 - 2.f), 16.f);
            g.setColour (cols[b].withAlpha (muted ? 0.10f : 0.34f));
            g.fillRoundedRectangle (bar, 3.f);
            g.setColour (cols[b].withAlpha (muted ? 0.25f : 0.85f));
            g.drawRoundedRectangle (bar, 3.f, 1.f);

            g.setColour (juce::Colours::white.withAlpha (muted ? 0.35f : 0.9f));
            g.setFont (11.f);
            g.drawText ("B" + juce::String (b + 1) + "  " + juce::String (gdb, 1) + " dB",
                        bar.toNearestInt(), juce::Justification::centred, false);
        }

        // crossover markers
        const float dashes[] = { 4.f, 4.f };
        for (int i = 1; i <= 3; ++i)
        {
            const float x = freqToX (edges[i]);
            g.setColour (juce::Colours::white.withAlpha (0.35f));
            g.drawDashedLine (juce::Line<float> (x, 0.f, x, h), dashes, 2, 1.f);
            g.setFont (10.f);
            g.drawText (edges[i] >= 1000.f ? juce::String (edges[i] / 1000.f, 2) + " kHz" : juce::String ((int) edges[i]) + " Hz",
                        (int) x + 3, 4, 60, 12, juce::Justification::left);
        }

        // spectrum line
        if (! smoothDb.empty())
        {
            juce::Path path;
            for (size_t x = 0; x < smoothDb.size(); ++x)
            {
                const float y = dbToY (smoothDb[x]);
                if (x == 0) path.startNewSubPath (0.f, y);
                else        path.lineTo ((float) x, y);
            }
            g.setColour (MBDProcessor::getSpectrumColour ((int) specColP->load()));
            g.strokePath (path, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }
    }

    //==========================================================================
    void mouseMove (const juce::MouseEvent& e) override
    {
        setMouseCursor (nearEdge ((float) e.x) >= 0 ? juce::MouseCursor::LeftRightResizeCursor
                                                    : juce::MouseCursor::UpDownResizeCursor);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        dragXover = nearEdge ((float) e.x);
        dragBand  = -1;
        if (dragXover >= 0)
        {
            xoverParam[dragXover]->beginChangeGesture();
            return;
        }

        float f1, f2, f3;
        proc.getCrossovers (f1, f2, f3);
        const float fx = xToFreq ((float) e.x);
        dragBand = fx < f1 ? 0 : fx < f2 ? 1 : fx < f3 ? 2 : 3;
        gainParam[dragBand]->beginChangeGesture();
        setGainFromY ((float) e.y);
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (dragXover >= 0)
        {
            float f[3];
            proc.getCrossovers (f[0], f[1], f[2]);
            const float lo = dragXover == 0 ? minFreq : f[dragXover - 1];
            const float hi = dragXover == 2 ? maxFreq : f[dragXover + 1];
            const float nf = juce::jlimit (lo * 1.25f, hi / 1.25f, xToFreq ((float) e.x));
            xoverParam[dragXover]->setValueNotifyingHost (xoverParam[dragXover]->convertTo0to1 (nf));
        }
        else if (dragBand >= 0)
            setGainFromY ((float) e.y);
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        if (dragXover >= 0) xoverParam[dragXover]->endChangeGesture();
        if (dragBand >= 0)  gainParam[dragBand]->endChangeGesture();
        dragXover = dragBand = -1;
    }

private:
    static constexpr float minFreq = 20.f, maxFreq = 20000.f;
    static constexpr float minDb = -96.f, maxDb = 6.f;
    static constexpr float gainMin = -60.f, gainMax = 12.f;

    float freqToX (float f) const { return (float) getWidth() * std::log (f / minFreq) / std::log (maxFreq / minFreq); }
    float xToFreq (float x) const { return minFreq * std::pow (maxFreq / minFreq, x / (float) getWidth()); }
    float dbToY (float db) const  { return (float) getHeight() * (1.f - (juce::jlimit (minDb, maxDb, db) - minDb) / (maxDb - minDb)); }
    float gainToY (float gdb) const { return juce::jmap (gdb, gainMin, gainMax, (float) getHeight() - 20.f, 20.f); }

    void setGainFromY (float y)
    {
        const float gdb = juce::jlimit (gainMin, gainMax, juce::jmap (y, (float) getHeight() - 20.f, 20.f, gainMin, gainMax));
        gainParam[dragBand]->setValueNotifyingHost (gainParam[dragBand]->convertTo0to1 (gdb));
    }

    int nearEdge (float x) const
    {
        float f[3];
        proc.getCrossovers (f[0], f[1], f[2]);
        for (int i = 0; i < 3; ++i)
            if (std::abs (x - freqToX (f[i])) <= 6.f) return i;
        return -1;
    }

    MBDProcessor& proc;
    juce::TextButton prePost { "PRE" };
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> prePostAtt;

    std::atomic<float>* gainP[4];
    std::atomic<float>* muteP[4];
    std::atomic<float>* specColP = nullptr;
    juce::RangedAudioParameter* gainParam[4];
    juce::RangedAudioParameter* xoverParam[3];
    int dragXover = -1, dragBand = -1;

    juce::dsp::FFT fft { fftOrder };
    juce::dsp::WindowingFunction<float> window { (size_t) fftSize, juce::dsp::WindowingFunction<float>::hann };
    std::array<float, (size_t) fftSize * 2> fftData {};
    std::vector<float> smoothDb;
};
