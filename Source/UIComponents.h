#pragma once
#include "Common.h"
#include "PluginProcessor.h"

//==============================================================================
// Rotary knob with a caption above and a value box below, bound to a parameter.
class ParamKnob : public juce::Component
{
public:
    ParamKnob (juce::AudioProcessorValueTreeState& apvts, const juce::String& paramID,
               const juce::String& labelText, const juce::String& suffix = {}, int decimals = 2)
    {
        label.setText (labelText, juce::dontSendNotification);
        label.setJustificationType (juce::Justification::centred);
        label.setFont (juce::Font (juce::FontOptions (12.f)));
        addAndMakeVisible (label);

        slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
        slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 64, 16);
        slider.setTextValueSuffix (suffix);
        slider.setNumDecimalPlacesToDisplay (decimals);
        addAndMakeVisible (slider);

        attachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (apvts, paramID, slider);
    }

    void setLabel (const juce::String& t) { label.setText (t, juce::dontSendNotification); }

    void resized() override
    {
        auto r = getLocalBounds();
        label.setBounds (r.removeFromTop (12));
        slider.setBounds (r);
    }

private:
    juce::Label label;
    juce::Slider slider;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachment;
};

//==============================================================================
// Small horizontal activity meter (-60 dB .. 0 dB).
class LevelBar : public juce::Component
{
public:
    void setLevelLinear (float lin)
    {
        const float db = juce::Decibels::gainToDecibels (lin, -60.f);
        const float t  = juce::jmap (db, -60.f, 0.f, 0.f, 1.f);
        shown = t > shown ? t : shown * 0.85f + t * 0.15f;
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        g.setColour (juce::Colour (0xff0e0f12));
        g.fillRoundedRectangle (r, 3.f);

        auto fill = r.reduced (1.f);
        fill.setWidth (fill.getWidth() * juce::jlimit (0.f, 1.f, shown));
        juce::ColourGradient grad (juce::Colour (0xff3ddc84), r.getX(), 0.f,
                                   juce::Colour (0xffe5383b), r.getRight(), 0.f, false);
        grad.addColour (0.7, juce::Colour (0xffe8c33a));
        g.setGradientFill (grad);
        g.fillRoundedRectangle (fill, 2.f);
    }

private:
    float shown = 0.f;
};

//==============================================================================
// One band's column of controls (meter, M/S, drive, type, mod, mix).
class BandColumn : public juce::Component
{
public:
    static constexpr int topOffset = 34;   // leaves room so the meter lines up with the crossover dials

    BandColumn (MBDProcessor& p, int index, juce::Colour c)
        : proc (p), idx (index), colour (c),
          drive (p.apvts, pid (index, "Drive"), "Drive", "", 2),
          mod  (p.apvts, pid (index, "Mod"),  "Mod", "", 2),
          mix (p.apvts, pid (index, "Mix"), "Mix", "", 2)
    {
        addAndMakeVisible (meter);

        for (auto* b : { &mute, &solo })
        {
            b->setClickingTogglesState (true);
            addAndMakeVisible (*b);
        }
        mute.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xffd9534f));
        solo.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xffe8c33a));
        solo.setColour (juce::TextButton::textColourOnId, juce::Colours::black);
        muteAtt = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (p.apvts, pid (index, "Mute"), mute);
        soloAtt = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (p.apvts, pid (index, "Solo"), solo);

        typeBox.addItemList (MBDProcessor::getDistortionNames(), 1);
        addAndMakeVisible (typeBox);
        typeAtt = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (p.apvts, pid (index, "Type"), typeBox);

        addAndMakeVisible (drive);
        addAndMakeVisible (mod);
        addAndMakeVisible (mix);

        typeParam = p.apvts.getRawParameterValue (pid (index, "Type"));
        refresh();
    }

    // Called from the editor's timer.
    void refresh()
    {
        meter.setLevelLinear (proc.bandLevel[(size_t) idx].load());

        const int t = (int) typeParam->load();
        if (t != lastType)
        {
            lastType = t;
            // Same order as mbd::DistType.
            static const char* names[mbd::NumTypes] = { "Bias",        // Tape
                                                        "Bias",        // Tube
                                                        "Asymmetry",   // Log Soft
                                                        "Asymmetry",   // Sine Fold
                                                        "Asymmetry",   // Tri Fold
                                                        "Fold",        // Hard Clip
                                                        "Slew limit",  // Slew Limiter
                                                        "Sample rate", // Decimator
                                                        "Bits",        // Bitcrusher
                                                        "Passes",      // Fractal
                                                        "Asymmetry",   // Tanh
                                                        "Degeneration",// BJT Pair
                                                        "Asymmetry",   // Diode
                                                        "Half / Full", // Rectifier
                                                        "Dead zone",   // Crossover
                                                        "Order",       // Chebyshev
                                                        "Presence",    // Amp Crunch
                                                        "Presence" };  // Amp Lead
            mod.setLabel (names[juce::jlimit (0, (int) mbd::NumTypes - 1, t)]);
        }
    }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        r.removeFromTop ((float) topOffset - 8.f);
        g.setColour (juce::Colour (0xff1d2025));
        g.fillRoundedRectangle (r, 6.f);
        g.setColour (colour);
        g.fillRoundedRectangle (r.withHeight (3.f), 1.5f);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (6, 0);
        r.removeFromTop (topOffset);
        meter.setBounds (r.removeFromTop (14));
        r.removeFromTop (6);

        auto ms = r.removeFromTop (22);
        const int half = ms.getWidth() / 2;
        mute.setBounds (ms.removeFromLeft (half).reduced (2, 0));
        solo.setBounds (ms.reduced (2, 0));

        r.removeFromTop (4);
        drive.setBounds (r.removeFromTop (84));
        r.removeFromTop (4);
        typeBox.setBounds (r.removeFromTop (22));
        r.removeFromTop (4);
        mod.setBounds (r.removeFromTop (84));
        r.removeFromTop (4);
        mix.setBounds (r.removeFromTop (84));
    }

private:
    static juce::String pid (int i, const char* s) { return "b" + juce::String (i + 1) + s; }

    MBDProcessor& proc;
    int idx;
    juce::Colour colour;
    int lastType = -1;
    std::atomic<float>* typeParam = nullptr;

    LevelBar meter;
    juce::TextButton mute { "M" }, solo { "S" };
    juce::ComboBox typeBox;
    ParamKnob drive, mod, mix;

    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> muteAtt, soloAtt;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> typeAtt;
};
