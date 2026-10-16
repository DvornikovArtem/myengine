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

add_executable(myengine_controls_tests
  controls/ControlSystemsTests.cpp)

target_link_libraries(myengine_controls_tests PRIVATE myengine_engine)
myengine_apply_compiler_options(myengine_controls_tests)
set_property(TARGET myengine_controls_tests PROPERTY FOLDER "Tests")
add_test(NAME control_systems COMMAND myengine_controls_tests)

add_executable(myengine_script_api_tests
  scripting/ScriptApiTests.cpp)

target_link_libraries(myengine_script_api_tests PRIVATE myengine_engine myengine_python)
myengine_apply_compiler_options(myengine_script_api_tests)
myengine_copy_runtime_assets(myengine_script_api_tests)
set_property(TARGET myengine_script_api_tests PROPERTY FOLDER "Tests")
add_test(NAME script_api COMMAND myengine_script_api_tests)
set_tests_properties(script_api PROPERTIES TIMEOUT 30)

add_executable(myengine_prefab_tests
  scene/PrefabLibraryTests.cpp)

target_link_libraries(myengine_prefab_tests PRIVATE myengine_engine)
myengine_apply_compiler_options(myengine_prefab_tests)
set_property(TARGET myengine_prefab_tests PROPERTY FOLDER "Tests")
add_test(NAME prefab_library COMMAND myengine_prefab_tests)
set_tests_properties(prefab_library PROPERTIES TIMEOUT 30)

add_executable(myengine_gameplay_tests
  scripting/GameplayTests.cpp)

target_link_libraries(myengine_gameplay_tests PRIVATE myengine_engine myengine_python)
myengine_apply_compiler_options(myengine_gameplay_tests)
myengine_copy_runtime_assets(myengine_gameplay_tests)
set_property(TARGET myengine_gameplay_tests PROPERTY FOLDER "Tests")
add_test(NAME scripting_gameplay COMMAND myengine_gameplay_tests)
set_tests_properties(scripting_gameplay PROPERTIES TIMEOUT 30)

# Headless Rocket League match: the car prefab, its wheels and the real script, without a window.
add_executable(myengine_rocket_league_tests
  scripting/RocketLeagueTests.cpp)

target_link_libraries(myengine_rocket_league_tests PRIVATE myengine_engine myengine_python)
myengine_apply_compiler_options(myengine_rocket_league_tests)
myengine_copy_runtime_assets(myengine_rocket_league_tests)
set_property(TARGET myengine_rocket_league_tests PROPERTY FOLDER "Tests")
add_test(NAME rocket_league COMMAND myengine_rocket_league_tests)
set_tests_properties(rocket_league PROPERTIES TIMEOUT 60)

# Manual integration test: plays a scripted round in a DX12 window.
add_executable(myengine_gameplay_smoke
  scene/GameplaySceneSmoke.cpp)

target_link_libraries(myengine_gameplay_smoke PRIVATE myengine_engine)
myengine_apply_compiler_options(myengine_gameplay_smoke)
myengine_copy_runtime_assets(myengine_gameplay_smoke)
set_property(TARGET myengine_gameplay_smoke PROPERTY FOLDER "Tests")

add_executable(myengine_prefab_inspector_tests
  ui/PrefabInspectorTests.cpp)

target_link_libraries(myengine_prefab_inspector_tests PRIVATE myengine_engine)
target_include_directories(myengine_prefab_inspector_tests SYSTEM PRIVATE "${CMAKE_SOURCE_DIR}/external/imgui/Include")
myengine_apply_compiler_options(myengine_prefab_inspector_tests)
myengine_copy_runtime_assets(myengine_prefab_inspector_tests)
set_property(TARGET myengine_prefab_inspector_tests PROPERTY FOLDER "Tests")
add_test(NAME prefab_inspector COMMAND myengine_prefab_inspector_tests)
set_tests_properties(prefab_inspector PROPERTIES TIMEOUT 30)

add_executable(myengine_transform_gizmo_tests
  ui/TransformGizmoTests.cpp)

target_link_libraries(myengine_transform_gizmo_tests PRIVATE myengine_engine)
target_include_directories(myengine_transform_gizmo_tests SYSTEM PRIVATE
  "${CMAKE_SOURCE_DIR}/external/imgui/Include"
  "${CMAKE_SOURCE_DIR}/external/ImGuizmo")
myengine_apply_compiler_options(myengine_transform_gizmo_tests)
set_property(TARGET myengine_transform_gizmo_tests PROPERTY FOLDER "Tests")
add_test(NAME transform_gizmo COMMAND myengine_transform_gizmo_tests)
set_tests_properties(transform_gizmo PROPERTIES TIMEOUT 30)

add_executable(myengine_content_browser_tests
  ui/ContentBrowserTests.cpp)

target_link_libraries(myengine_content_browser_tests PRIVATE myengine_engine)
target_include_directories(myengine_content_browser_tests SYSTEM PRIVATE "${CMAKE_SOURCE_DIR}/external/imgui/Include")
myengine_apply_compiler_options(myengine_content_browser_tests)
set_property(TARGET myengine_content_browser_tests PROPERTY FOLDER "Tests")
add_test(NAME content_browser COMMAND myengine_content_browser_tests)
set_tests_properties(content_browser PROPERTIES TIMEOUT 30)

add_executable(myengine_project_tests
  core/ProjectContextTests.cpp)

target_link_libraries(myengine_project_tests PRIVATE myengine_engine)
myengine_apply_compiler_options(myengine_project_tests)
set_property(TARGET myengine_project_tests PROPERTY FOLDER "Tests")
add_test(NAME project_context COMMAND myengine_project_tests)
set_tests_properties(project_context PROPERTIES TIMEOUT 30)

add_executable(myengine_script_console_tests
  scripting/ScriptConsoleTests.cpp)

target_link_libraries(myengine_script_console_tests PRIVATE myengine_engine)
target_include_directories(myengine_script_console_tests SYSTEM PRIVATE "${CMAKE_SOURCE_DIR}/external/imgui/Include")
myengine_apply_compiler_options(myengine_script_console_tests)
myengine_copy_runtime_assets(myengine_script_console_tests)
set_property(TARGET myengine_script_console_tests PROPERTY FOLDER "Tests")
add_test(NAME script_console COMMAND myengine_script_console_tests)
set_tests_properties(script_console PROPERTIES TIMEOUT 30)

# AS3: markdown, chat history, per-chat settings, attachments (no process is started)
add_executable(myengine_assistant_chat_tests
  assistant/ChatTests.cpp)
target_link_libraries(myengine_assistant_chat_tests PRIVATE myengine_engine)
myengine_apply_compiler_options(myengine_assistant_chat_tests)
set_property(TARGET myengine_assistant_chat_tests PROPERTY FOLDER "Tests")
add_test(NAME assistant_chat COMMAND myengine_assistant_chat_tests)
set_tests_properties(assistant_chat PROPERTIES TIMEOUT 60)

# AI1: stand-in for the claude CLI. The assistant tests start it through the real pipes and CreateProcess path
add_executable(myengine_fake_claude
  assistant/FakeClaude.cpp)

target_link_libraries(myengine_fake_claude PRIVATE myengine_nlohmann_json)
myengine_apply_compiler_options(myengine_fake_claude)
set_property(TARGET myengine_fake_claude PROPERTY FOLDER "Tests")

add_executable(myengine_assistant_tests
  assistant/AssistantTests.cpp)

target_link_libraries(myengine_assistant_tests PRIVATE myengine_engine)
target_include_directories(myengine_assistant_tests SYSTEM PRIVATE "${CMAKE_SOURCE_DIR}/external/imgui/Include")
add_dependencies(myengine_assistant_tests myengine_fake_claude)
myengine_apply_compiler_options(myengine_assistant_tests)
set_property(TARGET myengine_assistant_tests PROPERTY FOLDER "Tests")
add_test(NAME assistant COMMAND myengine_assistant_tests)
set_tests_properties(assistant PROPERTIES TIMEOUT 120)

# AS4: stand-in for the codex CLI and the tests of the Codex backend, its event parser, chat history and settings
add_executable(myengine_fake_codex
  assistant/FakeCodex.cpp)

target_link_libraries(myengine_fake_codex PRIVATE myengine_nlohmann_json)
myengine_apply_compiler_options(myengine_fake_codex)
set_property(TARGET myengine_fake_codex PROPERTY FOLDER "Tests")

add_executable(myengine_assistant_codex_tests
  assistant/CodexTests.cpp)

target_link_libraries(myengine_assistant_codex_tests PRIVATE myengine_engine)
add_dependencies(myengine_assistant_codex_tests myengine_fake_codex)
myengine_apply_compiler_options(myengine_assistant_codex_tests)
set_property(TARGET myengine_assistant_codex_tests PROPERTY FOLDER "Tests")
add_test(NAME assistant_codex COMMAND myengine_assistant_codex_tests)
set_tests_properties(assistant_codex PROPERTIES TIMEOUT 120)

# Manual check against the real Claude Code CLI (needs a login, spends money): not part of CTest
add_executable(myengine_assistant_live
  assistant/AssistantLive.cpp)

target_link_libraries(myengine_assistant_live PRIVATE myengine_engine)
add_dependencies(myengine_assistant_live myengine_mcp)
myengine_apply_compiler_options(myengine_assistant_live)
set_property(TARGET myengine_assistant_live PROPERTY FOLDER "Tests")

# AI2: engine tools, MCP bridge (the real myengine_mcp.exe over a named pipe), confirmations
add_executable(myengine_assistant_bridge_tests
  assistant/AssistantBridgeTests.cpp)

target_link_libraries(myengine_assistant_bridge_tests PRIVATE myengine_engine)
target_include_directories(myengine_assistant_bridge_tests SYSTEM PRIVATE "${CMAKE_SOURCE_DIR}/external/imgui/Include")
add_dependencies(myengine_assistant_bridge_tests myengine_mcp myengine_fake_claude)
myengine_apply_compiler_options(myengine_assistant_bridge_tests)
set_property(TARGET myengine_assistant_bridge_tests PROPERTY FOLDER "Tests")
add_test(NAME assistant_bridge COMMAND myengine_assistant_bridge_tests)
set_tests_properties(assistant_bridge PROPERTIES TIMEOUT 120)

# LD1: the loading service: thresholds, progress, polled sources
add_executable(myengine_loading_tests
  core/LoadingServiceTests.cpp)

target_link_libraries(myengine_loading_tests PRIVATE myengine_engine)
myengine_apply_compiler_options(myengine_loading_tests)
set_property(TARGET myengine_loading_tests PROPERTY FOLDER "Tests")
add_test(NAME loading_service COMMAND myengine_loading_tests)

# QA1: GPU diagnostics without a GPU: statistics text, capture tool lookup; the report script self test
add_executable(myengine_gpu_stats_tests
  render/GpuStatsTests.cpp)

target_link_libraries(myengine_gpu_stats_tests PRIVATE myengine_engine)
myengine_apply_compiler_options(myengine_gpu_stats_tests)
set_property(TARGET myengine_gpu_stats_tests PROPERTY FOLDER "Tests")
add_test(NAME gpu_stats COMMAND myengine_gpu_stats_tests)

find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
  add_test(NAME gpu_frame_report_selftest COMMAND ${Python3_EXECUTABLE} "${CMAKE_SOURCE_DIR}/tools/gpu/frame_report.py" --self-test)
endif()

# R2: ThumbnailService on a recording adapter (no GPU): placeholder, budget, pool, disk cache, invalidation
add_executable(myengine_thumbnail_tests
  render/ThumbnailServiceTests.cpp)

target_link_libraries(myengine_thumbnail_tests PRIVATE myengine_engine)
myengine_apply_compiler_options(myengine_thumbnail_tests)
set_property(TARGET myengine_thumbnail_tests PROPERTY FOLDER "Tests")
add_test(NAME thumbnail_service COMMAND myengine_thumbnail_tests)
set_tests_properties(thumbnail_service PROPERTIES TIMEOUT 120)

# R2: render-to-texture on a real DX12 device and window: manual, not part of CTest
add_executable(myengine_render_target_smoke
  render/RenderTargetSmoke.cpp)

target_link_libraries(myengine_render_target_smoke PRIVATE myengine_engine)
myengine_apply_compiler_options(myengine_render_target_smoke)
set_property(TARGET myengine_render_target_smoke PROPERTY FOLDER "Tests")
