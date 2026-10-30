set(BRAINVIEW_HEADERS
  include/brainview/BrainActivity.h
  include/brainview/BrainLayout.h
  include/brainview/BrainPump.h
  include/brainview/BrainRunner.h
  include/brainview/BrainView.h
  include/brainview/BrainViewPanel.h
  include/brainview/BrainViewWindow.h
  include/brainview/DoomMenuBackdrop.h
  include/brainview/OrbitCamera.h
  include/brainview/PlayerBrainPanel.h
)

set(BRAINVIEW_SOURCES
  src/BrainActivity.cpp
  src/BrainLayout.cpp
  src/BrainPump.cpp
  src/BrainRunner.cpp
  src/BrainView.cpp
  src/BrainViewPanel.cpp
  src/BrainViewWindow.cpp
  src/DoomMenuBackdrop.cpp
  src/OrbitCamera.cpp
  src/PlayerBrainPanel.cpp
)

# The 3D view of the fly brain (docs/flybrain/brain-view.md): the cloud of neurons in their places that flash when they spike, the HUD (groups, raster), the
# window Window > Fly Brain with a brain on a thread of its own. Knows nothing of DOOM: whoever runs a brain tells the view which neuron spiked how long ago.
add_library(myengine_brainview STATIC ${BRAINVIEW_HEADERS} ${BRAINVIEW_SOURCES})

target_include_directories(myengine_brainview
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include)

# ImGui is private to the engine library; the panel and the window draw with it, so they get its headers (the engine sets no IMGUI_ macros)
target_include_directories(myengine_brainview
  PRIVATE
    ${CMAKE_SOURCE_DIR}/external/imgui/Include
    ${CMAKE_SOURCE_DIR}/external/imgui/Include/imgui)

target_link_libraries(myengine_brainview
  PUBLIC
    myengine_flybrain
    myengine_engine)

myengine_apply_compiler_options(myengine_brainview)
set_property(TARGET myengine_brainview PROPERTY FOLDER "Game")

source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${BRAINVIEW_HEADERS} ${BRAINVIEW_SOURCES})
