# The classic DOOM of the engine (docs/game/doom.md): doomgeneric (GPL v2, the 1997 source of id Software) plus a thin C++ part. No DX12,
# ImGui or ECS in here: the engine glue is engine/src/game/doom, the tests drive DoomGame headless.
enable_language(C)

set(DOOMGENERIC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/doomgeneric")

# Every .c file of the copy is DOOM itself, except the two the port does not use (a sound card configurator and a window icon)
file(GLOB DOOMGENERIC_SOURCES CONFIGURE_DEPENDS "${DOOMGENERIC_DIR}/*.c")
list(REMOVE_ITEM DOOMGENERIC_SOURCES
  "${DOOMGENERIC_DIR}/gusconf.c"
  "${DOOMGENERIC_DIR}/icon.c")
file(GLOB DOOMGENERIC_HEADERS CONFIGURE_DEPENDS "${DOOMGENERIC_DIR}/*.h")

# The code of the third party is not ours: its warnings are off for this target only (the rest of the project builds with /W4)
add_library(myengine_doomgeneric STATIC ${DOOMGENERIC_SOURCES} ${DOOMGENERIC_HEADERS})
target_include_directories(myengine_doomgeneric PUBLIC "${DOOMGENERIC_DIR}")
target_compile_definitions(myengine_doomgeneric
  PUBLIC
    FEATURE_SOUND
    DOOMGENERIC_RESX=320
    DOOMGENERIC_RESY=200
  PRIVATE
    _CRT_SECURE_NO_WARNINGS
    _CRT_NONSTDC_NO_WARNINGS)
target_sources(myengine_doomgeneric PRIVATE src/DoomBridge.c src/DoomSound.c src/DoomBridge.h)
target_include_directories(myengine_doomgeneric PRIVATE src)

# The music: the OPL module of Chocolate Doom (GPL v2) over Nuked OPL3 (LGPL v2.1), both in third_party/opl, driven offline by src/opl_render.c
set(DOOM_OPL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/opl")
target_sources(myengine_doomgeneric PRIVATE
  src/opl_render.c
  src/OplRender.h
  "${DOOM_OPL_DIR}/i_oplmusic.c"
  "${DOOM_OPL_DIR}/midifile.c"
  "${DOOM_OPL_DIR}/midifile.h"
  "${DOOM_OPL_DIR}/opl.h"
  "${DOOM_OPL_DIR}/opl_queue.c"
  "${DOOM_OPL_DIR}/opl_queue.h"
  "${DOOM_OPL_DIR}/opl3.c"
  "${DOOM_OPL_DIR}/opl3.h")
target_include_directories(myengine_doomgeneric PRIVATE "${DOOM_OPL_DIR}")
if(MSVC)
  target_compile_options(myengine_doomgeneric PRIVATE /W0 /FS /utf-8)
endif()
set_property(TARGET myengine_doomgeneric PROPERTY FOLDER "Game")

set(DOOM_HEADERS
  include/doom/DoomAudio.h
  include/doom/DoomGame.h
  include/doom/DoomKeys.h
  src/DoomMusic.h
)
set(DOOM_SOURCES
  src/DoomGame.cpp
  src/DoomMusic.cpp
  src/DoomWad.cpp
)

add_library(myengine_doom STATIC ${DOOM_HEADERS} ${DOOM_SOURCES})
target_include_directories(myengine_doom
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(myengine_doom PUBLIC myengine_doomgeneric)
myengine_apply_compiler_options(myengine_doom)
set_property(TARGET myengine_doom PROPERTY FOLDER "Game")

source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${DOOM_HEADERS} ${DOOM_SOURCES})
