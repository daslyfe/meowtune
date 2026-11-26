cmake_minimum_required(VERSION 3.22)

if(NOT CMAKE_OSX_ARCHITECTURES)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "Build universal binaries")
endif()
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version")

project(MeowTune VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release)
endif()

option(MEOWTUNE_PLUGIN "Build the plugin (needs JUCE)" ON)

# --- Engine (no JUCE) and its headless test -----------------------------------
add_library(tune_engine STATIC Source/TuneEngine.cpp)
target_include_directories(tune_engine PUBLIC Source)
target_compile_options(tune_engine PRIVATE -O3 -Wall -Wextra)
set_target_properties(tune_engine PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_executable(engine_test tools/engine_test.cpp)
target_link_libraries(engine_test PRIVATE tune_engine)
target_compile_options(engine_test PRIVATE -O2)

# --- Plugin -------------------------------------------------------------------
if(MEOWTUNE_PLUGIN)
    if(NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/libs/JUCE/CMakeLists.txt)
        message(FATAL_ERROR "libs/JUCE is empty. Run: git submodule update --init")
    endif()
    add_subdirectory(libs/JUCE)

    juce_add_plugin(MeowTune
        COMPANY_NAME "Meow"
        BUNDLE_ID "com.meow.meowtune"
        PLUGIN_MANUFACTURER_CODE Meow
        PLUGIN_CODE MTun
        FORMATS VST3 AU
        PRODUCT_NAME "MeowTune"
        DESCRIPTION "Pitch correction with formants, unison and MIDI notes"
        ICON_BIG ${CMAKE_CURRENT_SOURCE_DIR}/assets/icon.png
        IS_SYNTH FALSE
        NEEDS_MIDI_INPUT TRUE
        NEEDS_MIDI_OUTPUT FALSE
        IS_MIDI_EFFECT FALSE
        NEEDS_WEB_BROWSER TRUE
        VST3_CATEGORIES Fx Pitch\ Shift
        AU_MAIN_TYPE kAudioUnitType_MusicEffect
        COPY_PLUGIN_AFTER_BUILD FALSE)

    juce_add_binary_data(MeowTuneUI SOURCES
        ui/index.html
        ui/app.css
        ui/app.js
        ui/dom.js
        ui/css/meow-terminal.css
        ui/fonts/3270-Regular.woff
        libs/JUCE/modules/juce_gui_extra/native/javascript/index.js
        libs/JUCE/modules/juce_gui_extra/native/javascript/check_native_interop.js)

    target_sources(MeowTune PRIVATE
        Source/PluginProcessor.cpp
        Source/PluginEditor.cpp)

    target_compile_definitions(MeowTune PUBLIC
        JUCE_WEB_BROWSER=1
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
        MEOWTUNE_VERSION="${PROJECT_VERSION}")

    target_link_libraries(MeowTune
        PRIVATE
            MeowTuneUI
            tune_engine
            juce::juce_audio_utils
            juce::juce_gui_extra
        PUBLIC
            juce::juce_recommended_config_flags
            juce::juce_recommended_lto_flags
            juce::juce_recommended_warning_flags)

    # The processor and editor built from the plugin's sources, with the page in a real WKWebView.
    juce_add_console_app(proc_test PRODUCT_NAME "proc_test")
    target_sources(proc_test PRIVATE tools/proc_test.cpp tools/native_click.mm Source/PluginProcessor.cpp Source/PluginEditor.cpp)
    target_compile_definitions(proc_test PRIVATE
        JUCE_MODAL_LOOPS_PERMITTED=1 JUCE_WEB_BROWSER=1 JUCE_USE_CURL=0
        JucePlugin_Name="MeowTune" MEOWTUNE_VERSION="${PROJECT_VERSION}")
    target_link_libraries(proc_test PRIVATE
        MeowTuneUI tune_engine
        juce::juce_audio_utils juce::juce_gui_extra
        juce::juce_recommended_config_flags)

    # Loads the installed VST3/AU like a host: audio and MIDI through them, state through the wrappers.
    juce_add_console_app(host_test PRODUCT_NAME "host_test")
    target_sources(host_test PRIVATE tools/host_test.cpp)
    target_compile_definitions(host_test PRIVATE
        JUCE_PLUGINHOST_VST3=1 JUCE_PLUGINHOST_AU=1 JUCE_USE_CURL=0 JUCE_WEB_BROWSER=0 JUCE_MODAL_LOOPS_PERMITTED=1)
    target_link_libraries(host_test PRIVATE
        juce::juce_audio_processors juce::juce_audio_formats
        juce::juce_recommended_config_flags
        "-framework CoreAudioKit")
endif()
