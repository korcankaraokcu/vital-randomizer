cmake_minimum_required(VERSION 3.22)
project(VitalRandomizer VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_subdirectory(deps/JUCE)

juce_add_plugin(VitalRandomizer
    COMPANY_NAME            "Korcan Karaokcu"
    PLUGIN_MANUFACTURER_CODE Kkao
    PLUGIN_CODE              Vrnd
    FORMATS                  VST3 Standalone
    IS_SYNTH                 TRUE
    NEEDS_MIDI_INPUT         TRUE
    NEEDS_MIDI_OUTPUT        FALSE
    IS_MIDI_EFFECT           FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS TRUE
    COPY_PLUGIN_AFTER_BUILD  FALSE
    PRODUCT_NAME             "Vital Randomizer")

juce_generate_juce_header(VitalRandomizer)

target_sources(VitalRandomizer PRIVATE
    src/PluginProcessor.cpp
    src/PluginEditor.cpp
    src/VitalHost.cpp
    src/VitalState.cpp
    src/Audition.cpp
    src/WavetableFactory.cpp
    src/Schema.cpp
    src/Axes.cpp
    src/Archetypes.cpp
    src/Generator.cpp
    src/Loudness.cpp
    src/CandidateStore.cpp)

target_compile_definitions(VitalRandomizer PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    # The whole point of this plugin is that it hosts Vital, so the VST3 host
    # side of juce_audio_processors has to be compiled in.
    JUCE_PLUGINHOST_VST3=1
    JUCE_PLUGINHOST_VST=0
    JUCE_PLUGINHOST_AU=0
    JUCE_DISPLAY_SPLASH_SCREEN=0)

target_include_directories(VitalRandomizer PRIVATE deps)

target_link_libraries(VitalRandomizer PRIVATE
    juce::juce_audio_utils
    juce::juce_audio_processors
    juce::juce_dsp
    PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags)

if (MSVC)
    target_compile_options(VitalRandomizer PRIVATE /bigobj /Zc:__cplusplus)
endif()

# JUCE's own COPY_PLUGIN_AFTER_BUILD targets the machine-wide VST3 folder under
# Program Files, which needs an elevated build. Installing to the per-user
# folder instead keeps an ordinary build working.
if (WIN32)
    set(USER_VST3_DIR "$ENV{LOCALAPPDATA}/Programs/Common/VST3")
elseif (APPLE)
    set(USER_VST3_DIR "$ENV{HOME}/Library/Audio/Plug-Ins/VST3")
else()
    set(USER_VST3_DIR "$ENV{HOME}/.vst3")
endif()

add_custom_target(install_user_vst3
    COMMAND ${CMAKE_COMMAND} -E make_directory "${USER_VST3_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "$<TARGET_FILE_DIR:VitalRandomizer_VST3>/../.."
            "${USER_VST3_DIR}/Vital Randomizer.vst3"
    DEPENDS VitalRandomizer_VST3
    COMMENT "Installing to ${USER_VST3_DIR}")

# Console harness that renders generated patches through a real Vital, so the
# C++ generator can be measured against the same bar as the Python reference
# rather than assumed to have survived the port.
juce_add_console_app(vrtest PRODUCT_NAME "vrtest")

target_sources(vrtest PRIVATE
    tests/harness.cpp
    src/VitalHost.cpp
    src/VitalState.cpp
    src/Audition.cpp
    src/WavetableFactory.cpp
    src/Schema.cpp
    src/Axes.cpp
    src/Archetypes.cpp
    src/Generator.cpp
    src/Loudness.cpp)

target_compile_definitions(vrtest PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_PLUGINHOST_VST3=1
    JUCE_PLUGINHOST_VST=0
    JUCE_PLUGINHOST_AU=0
    JUCE_STANDALONE_APPLICATION=1)

target_include_directories(vrtest PRIVATE deps)

target_link_libraries(vrtest PRIVATE
    juce::juce_audio_utils
    juce::juce_audio_processors
    juce::juce_dsp
    PUBLIC
    juce::juce_recommended_config_flags)

if (MSVC)
    target_compile_options(vrtest PRIVATE /bigobj /Zc:__cplusplus)
endif()

# Loads a VST3 the way a DAW does, so the plugin can be checked against a real
# message thread rather than a lightweight scanner that cannot host plugins.
juce_add_console_app(vrscan PRODUCT_NAME "vrscan")
target_sources(vrscan PRIVATE tests/scan.cpp)
target_compile_definitions(vrscan PUBLIC
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0
    JUCE_PLUGINHOST_VST3=1 JUCE_PLUGINHOST_VST=0 JUCE_PLUGINHOST_AU=0
    JUCE_STANDALONE_APPLICATION=1
    # The scanner pumps the message loop while the plugin starts up, which is
    # what lets its worker thread finish loading Vital.
    JUCE_MODAL_LOOPS_PERMITTED=1)
target_link_libraries(vrscan PRIVATE
    juce::juce_audio_utils juce::juce_audio_processors
    PUBLIC juce::juce_recommended_config_flags)
if (MSVC)
    target_compile_options(vrscan PRIVATE /bigobj /Zc:__cplusplus)
endif()
