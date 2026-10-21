cmake_minimum_required(VERSION 3.22)
project(StageSignal VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(SS_BUILD_TESTS "Build portable core unit tests" ON)
option(SS_BUILD_APP "Build the JUCE app (standalone + CLAP)" OFF)
set(SS_ROOT ${CMAKE_CURRENT_SOURCE_DIR})
set(SS_TEST_MAIN ${SS_ROOT}/tests/test_main.cpp)

include(cmake/ss_module.cmake)

# ---- Portable core: auto-discover core/<module>/CMakeLists.txt (base first) -------------------
add_subdirectory(core/base)
file(GLOB core_mods CONFIGURE_DEPENDS LIST_DIRECTORIES true ${SS_ROOT}/core/*)
list(SORT core_mods)
foreach(m ${core_mods})
  get_filename_component(mname ${m} NAME)
  if(NOT mname STREQUAL "base" AND EXISTS ${m}/CMakeLists.txt)
    add_subdirectory(${m})
  endif()
endforeach()

# ---- Tests: auto-discover tests/unit/<module>/CMakeLists.txt -----------------------------------
if(SS_BUILD_TESTS)
  enable_testing()
  file(GLOB test_dirs CONFIGURE_DEPENDS LIST_DIRECTORIES true ${SS_ROOT}/tests/unit/*)
  list(SORT test_dirs)
  foreach(t ${test_dirs})
    get_filename_component(tname ${t} NAME)
    if(EXISTS ${t}/CMakeLists.txt)
      add_subdirectory(${t} tests_${tname})
    endif()
  endforeach()
endif()

# ---- JUCE app (CI-built; see app/CMakeLists.txt) -----------------------------------------------
if(SS_BUILD_APP)
  add_subdirectory(app)
endif()
