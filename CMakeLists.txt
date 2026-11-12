cmake_minimum_required(VERSION 3.22)

# ------------------------------------------------------------------------------------------
# OpenBridge: a Linux implementation of Elektron Overbridge (VST3 first, Digitone II first).
# ------------------------------------------------------------------------------------------
project(OpenBridge VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBUSB REQUIRED IMPORTED_TARGET libusb-1.0)
pkg_check_modules(SAMPLERATE REQUIRED IMPORTED_TARGET samplerate)
pkg_check_modules(ALSA REQUIRED IMPORTED_TARGET alsa)

# ---------------------------------------------------------------- core (no JUCE) ---------
add_library(ob_core STATIC
    src/core/DeviceDesc.cpp
    src/core/UsbDevice.cpp
    src/core/DriftResampler.cpp
    src/core/Messages.cpp
    src/core/Messenger.cpp
    src/core/Digitone2Params.cpp
    src/core/SysexClient.cpp
    src/core/PresetLibrary.cpp)
target_include_directories(ob_core PUBLIC src)
target_link_libraries(ob_core PUBLIC PkgConfig::LIBUSB PkgConfig::SAMPLERATE PkgConfig::ALSA pthread)
target_compile_options(ob_core PRIVATE -Wall -Wextra)

# diagnostics (development checkout only)
if(EXISTS "${CMAKE_SOURCE_DIR}/tools/obprobe.cpp")
    add_executable(obprobe tools/obprobe.cpp)
    add_executable(obsysex tools/obsysex.cpp)
    target_link_libraries(obsysex PRIVATE ob_core)
    target_link_libraries(obprobe PRIVATE ob_core)
endif()

# ---------------------------------------------------------------- plugin (JUCE) ----------
# JUCE 7: the system package (find_package), or JUCE sources given with -DOB_JUCE_DIR=<path>.
set(OB_JUCE_DIR "" CACHE PATH "JUCE 7 source tree to build with (empty: installed JUCE package)")
if(OB_JUCE_DIR)
    add_subdirectory("${OB_JUCE_DIR}" JUCE EXCLUDE_FROM_ALL)
else()
    find_package(JUCE CONFIG REQUIRED)
endif()

# The published release is the VST3 only; the standalone application is built with -DOB_BUILD_STANDALONE=ON.
option(OB_BUILD_STANDALONE "Also build the standalone application" OFF)
set(OB_FORMATS VST3)
if(OB_BUILD_STANDALONE)
    list(APPEND OB_FORMATS Standalone)
endif()

juce_add_plugin(OpenBridgeDigitone2
    PRODUCT_NAME "OpenBridge Digitone II"
    COMPANY_NAME "OpenBridge"
    BUNDLE_ID "org.openbridge.digitone2"
    PLUGIN_MANUFACTURER_CODE Opbr
    PLUGIN_CODE ObD2
    FORMATS ${OB_FORMATS}
    IS_SYNTH TRUE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS TRUE
    VST3_CATEGORIES Instrument Synth
    COPY_PLUGIN_AFTER_BUILD FALSE)

target_sources(OpenBridgeDigitone2 PRIVATE
    src/plugin/PluginProcessor.cpp
    src/plugin/PluginEditor.cpp
    src/plugin/DeviceSession.cpp
    src/plugin/DeviceModel.cpp
    src/plugin/Widgets.cpp
    src/plugin/Diagrams.cpp
    src/plugin/Pages.cpp
    src/plugin/PresetBrowser.cpp)

target_compile_definitions(OpenBridgeDigitone2 PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_VST3_EMULATE_MIDI_CC_WITH_PARAMETERS=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_REPORT_APP_USAGE=0
    JUCE_ALSA=1
    JUCE_JACK=1)

target_link_libraries(OpenBridgeDigitone2
    PRIVATE
        ob_core
        juce::juce_audio_utils
        juce::juce_audio_processors
        juce::juce_gui_extra
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

# ---------------------------------------------------------------- tools ------------------
if(EXISTS "${CMAKE_SOURCE_DIR}/tools/obsnapshot.cpp")
juce_add_console_app(obsnapshot PRODUCT_NAME "obsnapshot")
target_sources(obsnapshot PRIVATE
    tools/obsnapshot.cpp
    src/plugin/PluginProcessor.cpp
    src/plugin/PluginEditor.cpp
    src/plugin/DeviceSession.cpp
    src/plugin/DeviceModel.cpp
    src/plugin/Widgets.cpp
    src/plugin/Diagrams.cpp
    src/plugin/Pages.cpp
    src/plugin/PresetBrowser.cpp)
target_include_directories(obsnapshot PRIVATE src)
target_compile_definitions(obsnapshot PRIVATE JUCE_DISPLAY_SPLASH_SCREEN=0 JUCE_USE_FLAC=0 JUCE_USE_OGGVORBIS=0 JUCE_MODAL_LOOPS_PERMITTED=1 JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_ALSA=1 JUCE_JACK=0
    JucePlugin_Build_Standalone=0 JucePlugin_Name="OpenBridge Digitone II")
target_link_libraries(obsnapshot PRIVATE ob_core juce::juce_audio_utils juce::juce_audio_processors juce::juce_gui_extra
    juce::juce_recommended_config_flags)
endif()
