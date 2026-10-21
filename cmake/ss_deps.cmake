# Pinned third-party dependencies for the JUCE app layer (spec section 22 / 19: pin tag + SHA).
#   JUCE                 8.0.16-security  ea77444750de45e036e0279bb0291144bf8e3688  (AGPLv3 option)
#   clap-juce-extensions main @           7adee3a1bd4684d4caa5601100e364abccff4b4d  (MIT; bundles CLAP SDK + clap-helpers submodules)
# Local sandbox/dev builds can point at existing checkouts with -DSS_JUCE_DIR=... -DSS_CLAP_EXT_DIR=...
set(SS_JUCE_TAG_SHA ea77444750de45e036e0279bb0291144bf8e3688)
set(SS_CLAPEXT_SHA 7adee3a1bd4684d4caa5601100e364abccff4b4d)

include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

if(SS_JUCE_DIR)
  add_subdirectory(${SS_JUCE_DIR} ${CMAKE_BINARY_DIR}/_juce EXCLUDE_FROM_ALL)
else()
  FetchContent_Declare(juce GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
                       GIT_TAG ${SS_JUCE_TAG_SHA} GIT_SHALLOW OFF)
  FetchContent_MakeAvailable(juce)
endif()

if(SS_CLAP_EXT_DIR)
  add_subdirectory(${SS_CLAP_EXT_DIR} ${CMAKE_BINARY_DIR}/_clapext EXCLUDE_FROM_ALL)
else()
  FetchContent_Declare(clap_juce_extensions GIT_REPOSITORY https://github.com/free-audio/clap-juce-extensions.git
                       GIT_TAG ${SS_CLAPEXT_SHA} GIT_SUBMODULES_RECURSE ON GIT_SHALLOW OFF)
  FetchContent_MakeAvailable(clap_juce_extensions)
endif()
