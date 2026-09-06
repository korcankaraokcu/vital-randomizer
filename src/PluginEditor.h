#pragma once

#include <memory>
#include <vector>

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "PluginProcessor.h"

/*
    The randomizer strip sits above Vital's own GUI, embedded, so the two read
    as one instrument rather than two windows.

    Auditioning happens by playing, not by clicking previews, which is why the
    candidate list is a filmstrip you walk through rather than a grid you scan.
*/
class Filmstrip : public juce::Component
{
public:
    std::function<void (int)> onSelect;

    void setContents (int count, int cursor);
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;

private:
    int cellAt (int x) const;
    int count = 0, cursor = -1;
};

/*
    The starred patches, one numbered cell each.

    Drawn rather than built out of buttons for the same reason the filmstrip is:
    the count changes every time somebody stars something, and rebuilding a row
    of child components on every change is more machinery than a row of numbers
    deserves. Clicking one loads it, right clicking removes it.
*/
class KeeperRack : public juce::Component,
                   public juce::SettableTooltipClient
{
public:
    std::function<void (int)> onRecall, onRemove;

    void setContents (const std::vector<juce::String>& labels);
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;

private:
    int cellAt (int x) const;
    std::vector<juce::String> names;
};

class VitalRandomizerEditor : public juce::AudioProcessorEditor,
                              private juce::ChangeListener,
                              private juce::Timer
{
public:
    explicit VitalRandomizerEditor (VitalRandomizerProcessor&);
    ~VitalRandomizerEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void timerCallback() override;

    void refreshStyles();
    void refreshFromProcessor();
    void attachVitalEditor();
    void showSettingsMenu();
    void exportCurrent();

    VitalRandomizerProcessor& proc;

    juce::ComboBox styleBox;
    juce::Label styleLabel;

    struct AxisControl
    {
        juce::String key;
        std::unique_ptr<juce::Slider> slider;
        std::unique_ptr<juce::Label> label;
    };
    std::vector<AxisControl> axisControls;

    struct LockControl
    {
        schema::Section section;
        std::unique_ptr<juce::TextButton> button;
    };
    std::vector<LockControl> lockControls;

    juce::TextButton rollButton { "ROLL" };
    juce::TextButton varyButton { "VARY" };
    juce::TextButton prevButton { "<" };
    juce::TextButton nextButton { ">" };
    juce::TextButton starButton { "KEEP" };
    juce::TextButton exportButton { "EXPORT" };
    juce::TextButton settingsButton { "..." };
    juce::Slider varyDepth;

    Filmstrip filmstrip;
    KeeperRack keeperRack;
    juce::Label keeperLabel;
    juce::Label statusLabel;
    juce::Label macroLabel;

    std::unique_ptr<juce::AudioProcessorEditor> vitalEditor;
    std::unique_ptr<juce::FileChooser> chooser;

    bool triedAttaching = false;
    int vitalWidth = 980, vitalHeight = 620;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VitalRandomizerEditor)
};
