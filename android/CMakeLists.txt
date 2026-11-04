# android/CMakeLists.txt - the native part of the Android app: libSDL2.so and libmain.so (the
# game: hardware runtime, recompiled code, frontend, rcheevos). Driven by android/build_apk.py.
cmake_minimum_required(VERSION 3.18)
project(startropics C)

if(NOT SDL2_SOURCE_DIR)
  message(FATAL_ERROR "set SDL2_SOURCE_DIR to an SDL2 source tree (build_apk.py downloads one)")
endif()
set(SDL_SHARED ON CACHE BOOL "" FORCE)
set(SDL_STATIC OFF CACHE BOOL "" FORCE)
set(SDL_TEST OFF CACHE BOOL "" FORCE)
# keep this machine's paths out of the binaries (SDL's assert messages carry __FILE__)
get_filename_component(REPO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/.. ABSOLUTE)
add_compile_options("-ffile-prefix-map=${REPO_ROOT}=.")
add_subdirectory(${SDL2_SOURCE_DIR} sdl2)

get_filename_component(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/.. ABSOLUTE)
file(GLOB RUNTIME_SRC ${ROOT}/runtime/*.c)
list(FILTER RUNTIME_SRC EXCLUDE REGEX "/(headless|recomp_stub)\\.c$")
file(GLOB GEN_SRC ${ROOT}/generated/*.c)
if(NOT GEN_SRC)
  message(FATAL_ERROR "no generated code: run 'make recomp' with your ROM first")
endif()
set(FRONTEND_SRC)
foreach(f sdl_main font ui filedialog audio_out input gpu_sync icon_data rewind achievements touchpad http update)
  list(APPEND FRONTEND_SRC ${ROOT}/frontend/${f}.c)
endforeach()
file(GLOB RC_SRC
  ${ROOT}/third_party/rcheevos/src/api/*.c ${ROOT}/third_party/rcheevos/src/client/*.c
  ${ROOT}/third_party/rcheevos/src/hash/*.c ${ROOT}/third_party/rcheevos/src/runtime/*.c
  ${ROOT}/third_party/rcheevos/src/util/*.c)

add_library(main SHARED ${RUNTIME_SRC} ${GEN_SRC} ${FRONTEND_SRC} ${RC_SRC})
target_include_directories(main PRIVATE ${ROOT}/runtime ${ROOT}/frontend ${ROOT}/generated
  ${ROOT}/third_party/rcheevos/include ${ROOT}/third_party/rcheevos/src)
target_compile_definitions(main PRIVATE RC_CLIENT_SUPPORTS_HASH RC_HASH_NO_DISC RC_HASH_NO_ENCRYPTED RC_HASH_NO_ZIP)
# the Updates tab: this build's version and the GitHub repository it checks (build_apk.py passes them)
set(ST_VERSION "dev" CACHE STRING "this build's version (git describe)")
set(ST_REPO "" CACHE STRING "owner/name of the GitHub repository whose releases the Updates tab checks")
target_compile_definitions(main PRIVATE ST_VERSION="${ST_VERSION}" ST_REPO="${ST_REPO}")
target_compile_options(main PRIVATE -O2 -std=gnu11 -Wno-unused-parameter -Wno-unused-function -Wno-unused-label
  -Wno-unused-variable -Wno-unused-but-set-variable -Wno-implicit-fallthrough -Wno-missing-field-initializers)
target_link_libraries(main PRIVATE SDL2::SDL2 android log m)
