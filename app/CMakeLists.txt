include(${SS_ROOT}/cmake/ss_deps.cmake)

file(GLOB_RECURSE SS_APP_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/Source/*.cpp)

# Standalone app + VST3 and CLAP plugins share one AudioProcessor (spec v0.2 addendum DEC-04/DEC-11).
juce_add_plugin(StageSignal
  COMPANY_NAME "Circuit Drift Labs"
  PRODUCT_NAME "StageSignal"
  FORMATS Standalone VST3
  PLUGIN_MANUFACTURER_CODE Cdlb
  PLUGIN_CODE Stsg
  IS_SYNTH FALSE
  NEEDS_MIDI_INPUT TRUE
  NEEDS_MIDI_OUTPUT TRUE
  EDITOR_WANTS_KEYBOARD_FOCUS TRUE
  COPY_PLUGIN_AFTER_BUILD FALSE
  VERSION ${PROJECT_VERSION}
  BUNDLE_ID com.circuitdriftlabs.stagesignal
  ICON_BIG "" )

clap_juce_extensions_plugin(TARGET StageSignal
  CLAP_ID "com.circuitdriftlabs.stagesignal"
  CLAP_FEATURES audio-effect analyzer utility
  CLAP_MANUAL_URL "https://github.com/djshellshoxxx/stage-signal"
  CLAP_SUPPORT_URL "https://github.com/djshellshoxxx/stage-signal/issues")

target_sources(StageSignal PRIVATE ${SS_APP_SOURCES})
target_include_directories(StageSignal PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/Source)
target_compile_definitions(StageSignal PUBLIC
  JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0
  JUCE_USE_CAMERA=1 JUCE_STRICT_REFCOUNTEDPOINTER=1)
target_compile_features(StageSignal PUBLIC cxx_std_17)

# Core modules linked in as they land (each is portable and JUCE-free).
file(GLOB core_mods CONFIGURE_DEPENDS LIST_DIRECTORIES true ${SS_ROOT}/core/*)
foreach(m ${core_mods})
  get_filename_component(mname ${m} NAME)
  if(EXISTS ${m}/CMakeLists.txt AND TARGET ss_${mname})
    target_link_libraries(StageSignal PRIVATE ss_${mname})
  endif()
endforeach()
target_include_directories(StageSignal PRIVATE ${SS_ROOT}/third_party)

target_link_libraries(StageSignal PRIVATE
  juce::juce_audio_utils juce::juce_audio_devices juce::juce_audio_formats
  juce::juce_gui_extra juce::juce_opengl juce::juce_dsp juce::juce_events
  PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
if(NOT MSVC)
  target_compile_options(StageSignal PRIVATE -Wno-unused-parameter)
endif()
