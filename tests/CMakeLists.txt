add_executable(myengine_job_stress
  jobs/JobSystemStress.cpp)

target_link_libraries(myengine_job_stress
  PRIVATE
    myengine_engine)

myengine_apply_compiler_options(myengine_job_stress)

set_property(TARGET myengine_job_stress PROPERTY FOLDER "Tests")

add_test(NAME job_system_stress COMMAND myengine_job_stress)

add_executable(myengine_scene_serialization
  scene/SceneSerializerTests.cpp)

target_link_libraries(myengine_scene_serialization PRIVATE myengine_engine)
myengine_apply_compiler_options(myengine_scene_serialization)
set_property(TARGET myengine_scene_serialization PROPERTY FOLDER "Tests")

add_test(NAME scene_serialization COMMAND myengine_scene_serialization)

# Manual integration test: creates a DX12 window, so it is not part of the headless CTest suite.
add_executable(myengine_scripting_smoke
  scene/ScriptingSceneSmoke.cpp)

target_link_libraries(myengine_scripting_smoke PRIVATE myengine_engine)
myengine_apply_compiler_options(myengine_scripting_smoke)
myengine_copy_runtime_assets(myengine_scripting_smoke)
set_property(TARGET myengine_scripting_smoke PROPERTY FOLDER "Tests")
