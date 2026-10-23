#pragma once
#include "Common.h"
#include "PluginProcessor.h"
#include "UIComponents.h"
#include "SpectrumView.h"
#include "SettingsPanel.h"

struct DarkLookAndFeel : juce::LookAndFeel_V4
{
    DarkLookAndFeel()
    {
        using namespace juce;
        setColour (ResizableWindow::backgroundColourId,   Colour (0xff15171b));
        setColour (Slider::rotarySliderFillColourId,      Colour (0xffe5383b));
        setColour (Slider::rotarySliderOutlineColourId,   Colour (0xff34373d));
        setColour (Slider::thumbColourId,                 Colour (0xfff0f0f0));
        setColour (Slider::textBoxTextColourId,           Colour (0xffd0d0d0));
        setColour (Slider::textBoxOutlineColourId,        Colours::transparentBlack);
        setColour (Label::textColourId,                   Colour (0xffa8abb2));
        setColour (TextButton::buttonColourId,            Colour (0xff2a2d33));
        setColour (TextButton::textColourOffId,           Colour (0xffd0d0d0));
        setColour (TextButton::textColourOnId,            Colours::white);
        setColour (ComboBox::backgroundColourId,          Colour (0xff2a2d33));
        setColour (ComboBox::outlineColourId,             Colour (0xff3a3d44));
        setColour (ComboBox::textColourId,                Colour (0xffd0d0d0));
        setColour (PopupMenu::backgroundColourId,         Colour (0xff22252a));
        setColour (PopupMenu::highlightedBackgroundColourId, Colour (0xffe5383b));
    }

    // Same arc as the stock look, but the indicator is a line instead of a round thumb.
    void drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height, float sliderPos,
                           float startAngle, float endAngle, juce::Slider& slider) override
    {
        using namespace juce;
        const auto bounds = Rectangle<int> (x, y, width, height).toFloat().reduced (10.f);
        const float radius = jmin (bounds.getWidth(), bounds.getHeight()) * 0.5f;
        const float toAngle = startAngle + sliderPos * (endAngle - startAngle);
        const float lineW = jmin (8.f, radius * 0.5f);
        const float arcR = radius - lineW * 0.5f;
        const auto centre = bounds.getCentre();

        Path track;
        track.addCentredArc (centre.x, centre.y, arcR, arcR, 0.f, startAngle, endAngle, true);
        g.setColour (slider.findColour (Slider::rotarySliderOutlineColourId));
        g.strokePath (track, PathStrokeType (lineW, PathStrokeType::curved, PathStrokeType::butt));

        if (slider.isEnabled())
        {
            Path value;
            value.addCentredArc (centre.x, centre.y, arcR, arcR, 0.f, startAngle, toAngle, true);
            g.setColour (slider.findColour (Slider::rotarySliderFillColourId));
            g.strokePath (value, PathStrokeType (lineW, PathStrokeType::curved, PathStrokeType::butt));
        }

        Path pointer;
        pointer.startNewSubPath (centre.getPointOnCircumference (radius * 0.3f, toAngle));
        pointer.lineTo (centre.getPointOnCircumference (radius, toAngle));
        g.setColour (slider.findColour (Slider::thumbColourId));
        g.strokePath (pointer, PathStrokeType (2.5f, PathStrokeType::curved, PathStrokeType::rounded));
    }
};

class MBDEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit MBDEditor (MBDProcessor&);
    ~MBDEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;   // click-away closes the Settings card

private:
    void timerCallback() override;
    void refreshPresetList();
    void savePreset();
    void loadPreset();

    MBDProcessor& proc;
    DarkLookAndFeel lnf;

    // top bar
    juce::Label title;
    juce::ComboBox presetBox;
    juce::TextButton saveBtn { "Save" }, loadBtn { "Load" }, settingsBtn { "Settings" };
    juce::Array<juce::File> presetFiles;
    std::unique_ptr<juce::FileChooser> chooser;

    // display row
    ParamKnob inKnob, outKnob;
    SpectrumView spectrum;

    // bands + crossover dials
    std::array<std::unique_ptr<BandColumn>, 4> columns;
    std::array<std::unique_ptr<ParamKnob>, 3> xoverKnobs;

    // noise gate
    juce::Label gateTitle;
    juce::TextButton gateOn { "ON" };
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> gateOnAtt;
    ParamKnob gateAmount, gateThresh, gateAttack, gateRelease;

    // post filter
    juce::Label pfTitle;
    juce::TextButton pfOn { "ON" };
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> pfOnAtt;
    juce::ComboBox pfType;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> pfTypeAtt;
    ParamKnob pfMix, pfCutoff, pfRes, pfQ;

    juce::Rectangle<int> gateRect, pfRect;
    std::atomic<float> *gateOnRaw = nullptr, *pfOnRaw = nullptr;

    SettingsPanel settingsPanel;   // declared last so it paints on top of everything

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MBDEditor)
};
