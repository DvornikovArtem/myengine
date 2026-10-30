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

# G8: the limit of the frames per second of the main loop (the rate and the length of a frame)
add_executable(myengine_frame_pacer_tests
  core/FramePacerTests.cpp)

target_link_libraries(myengine_frame_pacer_tests PRIVATE myengine_engine)
myengine_apply_compiler_options(myengine_frame_pacer_tests)
set_property(TARGET myengine_frame_pacer_tests PROPERTY FOLDER "Tests")
add_test(NAME frame_pacer COMMAND myengine_frame_pacer_tests)
# RUN_SERIAL: it measures the length of frames in real time (3 % and half a millisecond); with `ctest -j 8` next to the heavy tests it fails
set_tests_properties(frame_pacer PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE)

# RL-B1: the scale of Windows (DPI -> scale, sizes in real and logical pixels, WM_DPICHANGED, the scale of the game UI in real pixels, a hidden window)
add_executable(myengine_dpi_scale_tests
  core/DpiScaleTests.cpp)

target_link_libraries(myengine_dpi_scale_tests PRIVATE myengine_engine)
target_include_directories(myengine_dpi_scale_tests PRIVATE "${CMAKE_SOURCE_DIR}/external/imgui/Include")
myengine_apply_compiler_options(myengine_dpi_scale_tests)
set_property(TARGET myengine_dpi_scale_tests PROPERTY FOLDER "Tests")
add_test(NAME dpi_scale COMMAND myengine_dpi_scale_tests)
set_tests_properties(dpi_scale PROPERTIES TIMEOUT 60)

# B0: the model of the Build window of the editor (the manifests of the games, the lines of the script, the state of a run, the runner on a child process)
add_executable(myengine_build_panel_model_tests
  core/BuildPanelModelTests.cpp)

target_link_libraries(myengine_build_panel_model_tests PRIVATE myengine_engine)
myengine_apply_compiler_options(myengine_build_panel_model_tests)
set_property(TARGET myengine_build_panel_model_tests PROPERTY FOLDER "Tests")
add_test(NAME build_panel_model COMMAND myengine_build_panel_model_tests)
set_tests_properties(build_panel_model PROPERTIES TIMEOUT 120)

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

# A1 (LR3): shader reflection without a GPU: layouts, messages with file and line, cache, the shaders of the engine
add_executable(myengine_shader_reflection_tests
  render/ShaderReflectionTests.cpp)

target_link_libraries(myengine_shader_reflection_tests PRIVATE myengine_engine)
target_include_directories(myengine_shader_reflection_tests PRIVATE "${CMAKE_SOURCE_DIR}/engine/src/render/dx12")
myengine_apply_compiler_options(myengine_shader_reflection_tests)
set_property(TARGET myengine_shader_reflection_tests PROPERTY FOLDER "Tests")
add_test(NAME shader_reflection COMMAND myengine_shader_reflection_tests)
set_tests_properties(shader_reflection PROPERTIES TIMEOUT 120)

# A2 (LR3): the root signature builder without a GPU: root constants / CBV, tables, visibility, samplers, shape hash
add_executable(myengine_root_signature_tests
  render/RootSignatureTests.cpp)

target_link_libraries(myengine_root_signature_tests PRIVATE myengine_engine)
myengine_apply_compiler_options(myengine_root_signature_tests)
set_property(TARGET myengine_root_signature_tests PROPERTY FOLDER "Tests")
add_test(NAME root_signature COMMAND myengine_root_signature_tests)
set_tests_properties(root_signature PROPERTIES TIMEOUT 120)

# A-check (LR3): the input layout builder without a GPU: order, errors, stream tables, the shaders of the engine
add_executable(myengine_input_layout_tests
  render/InputLayoutTests.cpp)

target_link_libraries(myengine_input_layout_tests PRIVATE myengine_engine)
target_include_directories(myengine_input_layout_tests PRIVATE "${CMAKE_SOURCE_DIR}/engine/src/render/dx12")
myengine_apply_compiler_options(myengine_input_layout_tests)
set_property(TARGET myengine_input_layout_tests PROPERTY FOLDER "Tests")
add_test(NAME input_layout COMMAND myengine_input_layout_tests)

# G7b: the scene passes without a GPU: mesh bounds, the lit and post shaders, the instance stream, the sampler presets
add_executable(myengine_scene_pass_tests
  render/ScenePassTests.cpp)

target_link_libraries(myengine_scene_pass_tests PRIVATE myengine_engine)
target_include_directories(myengine_scene_pass_tests PRIVATE "${CMAKE_SOURCE_DIR}/engine/src/render/dx12")
myengine_apply_compiler_options(myengine_scene_pass_tests)
set_property(TARGET myengine_scene_pass_tests PROPERTY FOLDER "Tests")
add_test(NAME scene_pass COMMAND myengine_scene_pass_tests)
set_tests_properties(scene_pass PROPERTIES TIMEOUT 120)
set_tests_properties(input_layout PROPERTIES TIMEOUT 120)

# A-check (LR3): shader hot reload in the resource manager on a recording adapter: same handle, errors, fallback, includes
add_executable(myengine_shader_hot_reload_tests
  resource/ShaderHotReloadTests.cpp)

target_link_libraries(myengine_shader_hot_reload_tests PRIVATE myengine_engine)
myengine_apply_compiler_options(myengine_shader_hot_reload_tests)
set_property(TARGET myengine_shader_hot_reload_tests PROPERTY FOLDER "Tests")
add_test(NAME shader_hot_reload COMMAND myengine_shader_hot_reload_tests)
set_tests_properties(shader_hot_reload PROPERTIES TIMEOUT 120)

# B3.1 (LR3): the rows of the Material Editor and the edits of a material, without ImGui and without a GPU
add_executable(myengine_material_editor_model_tests
  render/MaterialEditorModelTests.cpp)

target_link_libraries(myengine_material_editor_model_tests PRIVATE myengine_engine)
target_include_directories(myengine_material_editor_model_tests PRIVATE "${CMAKE_SOURCE_DIR}/engine/src/render/dx12")
myengine_apply_compiler_options(myengine_material_editor_model_tests)
set_property(TARGET myengine_material_editor_model_tests PROPERTY FOLDER "Tests")
add_test(NAME material_editor_model COMMAND myengine_material_editor_model_tests)
set_tests_properties(material_editor_model PROPERTIES TIMEOUT 120)

# B2 (LR3): the metadata of the shader parameters (comments "// @label ... @default ...") without a GPU: the tags, every mistake
# with its file and line, includes, @default in the material parameters, a source that changes
add_executable(myengine_shader_metadata_tests
  render/ShaderMetadataTests.cpp)

target_link_libraries(myengine_shader_metadata_tests PRIVATE myengine_engine)
target_include_directories(myengine_shader_metadata_tests PRIVATE "${CMAKE_SOURCE_DIR}/engine/src/render/dx12")
myengine_apply_compiler_options(myengine_shader_metadata_tests)
set_property(TARGET myengine_shader_metadata_tests PROPERTY FOLDER "Tests")
add_test(NAME shader_metadata COMMAND myengine_shader_metadata_tests)
set_tests_properties(shader_metadata PROPERTIES TIMEOUT 120)

# B1 (LR3): the parameters of a material by the names of the shader layout, without a GPU: setters, bytes, errors,
# the old keys of a material, the materials of the project, save and load
add_executable(myengine_material_params_tests
  render/MaterialParamsTests.cpp)

target_link_libraries(myengine_material_params_tests PRIVATE myengine_engine)
target_include_directories(myengine_material_params_tests PRIVATE "${CMAKE_SOURCE_DIR}/engine/src/render/dx12")
myengine_apply_compiler_options(myengine_material_params_tests)
set_property(TARGET myengine_material_params_tests PROPERTY FOLDER "Tests")
add_test(NAME material_params COMMAND myengine_material_params_tests)
set_tests_properties(material_params PROPERTIES TIMEOUT 120)

# B1.2 (LR3): the parameters of a material reach the frame: a real DX12 device (or WARP), a render target, the pixels read
# back. Skipped (exit code 77) when there is no device or no desktop to make the window on.
add_executable(myengine_material_render_tests
  render/MaterialRenderTests.cpp)

target_link_libraries(myengine_material_render_tests PRIVATE myengine_engine)
myengine_apply_compiler_options(myengine_material_render_tests)
set_property(TARGET myengine_material_render_tests PROPERTY FOLDER "Tests")
add_test(NAME material_render COMMAND myengine_material_render_tests)
set_tests_properties(material_render PROPERTIES TIMEOUT 180 SKIP_RETURN_CODE 77)

# B4.1 (LR3): the scenarios of stability of the lab (a shader with an error, a series of reloads, a file that is gone, a reload while
# a render target is drawn, a long session, an exit in a dirty state) on a real DX12 device (or WARP) with the debug layer; after
# Shutdown no object of the device may be alive. Skipped (exit code 77) when there is no device or no desktop to make the window on.
add_executable(myengine_render_stability_tests
  render/RenderStabilityTests.cpp)

target_link_libraries(myengine_render_stability_tests PRIVATE myengine_engine)
myengine_apply_compiler_options(myengine_render_stability_tests)
set_property(TARGET myengine_render_stability_tests PROPERTY FOLDER "Tests")
add_test(NAME render_stability COMMAND myengine_render_stability_tests)
set_tests_properties(render_stability PROPERTIES TIMEOUT 300 SKIP_RETURN_CODE 77)

# B4.2 (LR3): the picture of a fixed scene is compared with the reference tests/render/reference/scene_256.png. The scene is drawn on the
# WARP adapter (the same rasterizer on every machine). The reference is changed only on purpose: --update-reference. Skipped (exit code
# 77) when WARP or a desktop to make the window on is not there.
add_executable(myengine_render_reference_tests
  render/RenderReferenceTests.cpp)

target_link_libraries(myengine_render_reference_tests PRIVATE myengine_engine)
target_compile_definitions(myengine_render_reference_tests PRIVATE MYENGINE_REFERENCE_OUTPUT_DIR="${CMAKE_BINARY_DIR}/render_reference")
myengine_apply_compiler_options(myengine_render_reference_tests)
set_property(TARGET myengine_render_reference_tests PROPERTY FOLDER "Tests")
add_test(NAME render_reference COMMAND myengine_render_reference_tests)
set_tests_properties(render_reference PROPERTIES TIMEOUT 180 SKIP_RETURN_CODE 77)

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

# D0: the classic DOOM of the engine (game/doom) with Freedoom, no window and no sound. Skipped (77) without the WAD:
# tools/doom/fetch_freedoom.ps1 puts it into assets/doom
add_executable(myengine_doom_core_tests
  doom/DoomCoreTests.cpp)
target_link_libraries(myengine_doom_core_tests PRIVATE myengine_doom)
target_compile_definitions(myengine_doom_core_tests PRIVATE DOOM_TEST_WAD_DIR="${CMAKE_SOURCE_DIR}/assets/doom")
myengine_apply_compiler_options(myengine_doom_core_tests)
set_property(TARGET myengine_doom_core_tests PROPERTY FOLDER "Tests")
add_test(NAME doom_core COMMAND myengine_doom_core_tests)
set_tests_properties(doom_core PROPERTIES TIMEOUT 300 SKIP_RETURN_CODE 77)

# G3: the Rocket League physics (Jolt) without a window
add_executable(myengine_rl_physics_tests
  rl/RlPhysicsTests.cpp)

target_link_libraries(myengine_rl_physics_tests PRIVATE myengine_rl)
target_compile_definitions(myengine_rl_physics_tests PRIVATE
  RL_TUNING_JSON="${CMAKE_SOURCE_DIR}/assets/data/rl_tuning.json"
  RL_ARENA_OBJ="${CMAKE_SOURCE_DIR}/assets/models/rl_arena.obj")
myengine_apply_compiler_options(myengine_rl_physics_tests)
set_property(TARGET myengine_rl_physics_tests PROPERTY FOLDER "Tests")
add_test(NAME rl_physics COMMAND myengine_rl_physics_tests)
set_tests_properties(rl_physics PROPERTIES TIMEOUT 300)

# G4: the match, the bots, the pads (no window)
add_executable(myengine_rl_match_tests
  rl/RlMatchTests.cpp)

target_link_libraries(myengine_rl_match_tests PRIVATE myengine_rl)
target_compile_definitions(myengine_rl_match_tests PRIVATE RL_CONTROLS_JSON="${CMAKE_SOURCE_DIR}/assets/data/rl_controls.json")
myengine_apply_compiler_options(myengine_rl_match_tests)
set_property(TARGET myengine_rl_match_tests PROPERTY FOLDER "Tests")
add_test(NAME rl_match COMMAND myengine_rl_match_tests)
set_tests_properties(rl_match PROPERTIES TIMEOUT 600)

# G4f: the bots of the second version against the first ones and against each other (many headless matches)
add_executable(myengine_rl_bots_tests
  rl/RlBotsTests.cpp)

target_link_libraries(myengine_rl_bots_tests PRIVATE myengine_rl)
target_compile_definitions(myengine_rl_bots_tests PRIVATE RL_TUNING_JSON="${CMAKE_SOURCE_DIR}/assets/data/rl_tuning.json")
myengine_apply_compiler_options(myengine_rl_bots_tests)
set_property(TARGET myengine_rl_bots_tests PROPERTY FOLDER "Tests")
# Debug runs the quick look (4 matches, no verdicts): the whole series is 40 minutes without optimisation, and its verdicts (win rates, 0.5 ms per tick)
# are for an optimised build; RelWithDebInfo and Release run all of it
add_test(NAME rl_bots COMMAND myengine_rl_bots_tests $<$<CONFIG:Debug>:--quick>)
set_tests_properties(rl_bots PROPERTIES TIMEOUT 900)

# G4i: the cars of the match (the physics of the six, the choice, six cars in one world); the garage and its data are tested by rl_cars
add_executable(myengine_rl_carphysics_tests
  rl/RlCarPhysicsTests.cpp)

target_link_libraries(myengine_rl_carphysics_tests PRIVATE myengine_rl)
target_compile_definitions(myengine_rl_carphysics_tests PRIVATE RL_CARS_DIR="${CMAKE_SOURCE_DIR}/assets/data/cars")
myengine_apply_compiler_options(myengine_rl_carphysics_tests)
set_property(TARGET myengine_rl_carphysics_tests PROPERTY FOLDER "Tests")
add_test(NAME rl_carphysics COMMAND myengine_rl_carphysics_tests)
set_tests_properties(rl_carphysics PROPERTIES TIMEOUT 300)

# G4e: the car of the game against RocketSim (reference traces written by tests/rl/rocketsim_diff, no RocketSim in this build)
add_executable(myengine_rl_rocketsim_diff
  rl/RlRocketSimDiffTests.cpp)

target_link_libraries(myengine_rl_rocketsim_diff PRIVATE myengine_rl)
target_compile_definitions(myengine_rl_rocketsim_diff PRIVATE
  RL_TUNING_JSON="${CMAKE_SOURCE_DIR}/assets/data/rl_tuning.json"
  RL_DIFF_TRACES="${CMAKE_SOURCE_DIR}/tests/rl/rocketsim_diff/traces"
  RL_CARS_DIR="${CMAKE_SOURCE_DIR}/assets/data/cars")
myengine_apply_compiler_options(myengine_rl_rocketsim_diff)
set_property(TARGET myengine_rl_rocketsim_diff PROPERTY FOLDER "Tests")
add_test(NAME rl_rocketsim_diff COMMAND myengine_rl_rocketsim_diff)
set_tests_properties(rl_rocketsim_diff PROPERTIES TIMEOUT 300)

# G5: the netcode of the match: transport, protocol, host, client with rollback (no window; virtual clock; one test over real UDP)
add_executable(myengine_rl_net_tests
  rl/RlNetTests.cpp)

target_link_libraries(myengine_rl_net_tests PRIVATE myengine_rl)
target_compile_definitions(myengine_rl_net_tests PRIVATE RL_TUNING_JSON="${CMAKE_SOURCE_DIR}/assets/data/rl_tuning.json" RL_CARS_DIR="${CMAKE_SOURCE_DIR}/assets/data/cars")
myengine_apply_compiler_options(myengine_rl_net_tests)
set_property(TARGET myengine_rl_net_tests PROPERTY FOLDER "Tests")
add_test(NAME rl_net COMMAND myengine_rl_net_tests)
set_tests_properties(rl_net PROPERTIES TIMEOUT 600)

# RL-B2: the spin and the steer of the wheels of the cars, from the state of a car (the sim, six cars, a car of another player over the net)
add_executable(myengine_rl_wheel_tests
  rl/RlWheelTests.cpp)

target_link_libraries(myengine_rl_wheel_tests PRIVATE myengine_rl)
target_compile_definitions(myengine_rl_wheel_tests PRIVATE RL_TUNING_JSON="${CMAKE_SOURCE_DIR}/assets/data/rl_tuning.json" RL_CARS_DIR="${CMAKE_SOURCE_DIR}/assets/data/cars")
myengine_apply_compiler_options(myengine_rl_wheel_tests)
set_property(TARGET myengine_rl_wheel_tests PROPERTY FOLDER "Tests")
add_test(NAME rl_wheels COMMAND myengine_rl_wheel_tests)
set_tests_properties(rl_wheels PROPERTIES TIMEOUT 120)

# G6b: the manual check of the Steam code against the real SDK (needs the Steam client running; not in ctest, exit code 77 = skipped)
if(MYENGINE_STEAM)
  add_executable(myengine_rl_steam_smoke
    rl/RlSteamSmoke.cpp)

  target_link_libraries(myengine_rl_steam_smoke PRIVATE myengine_rl)
  target_include_directories(myengine_rl_steam_smoke SYSTEM PRIVATE "${STEAMWORKS_SDK_DIR}/public")
  myengine_apply_compiler_options(myengine_rl_steam_smoke)
  set_property(TARGET myengine_rl_steam_smoke PROPERTY FOLDER "Tests")
endif()

# A build with Steam links steam_api64.lib into everything that uses myengine_rl: every test exe needs steam_api64.dll next to it to start
if(MYENGINE_STEAM)
  get_property(myengine_test_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
  foreach(test_target IN LISTS myengine_test_targets)
    get_target_property(test_target_type ${test_target} TYPE)
    if(test_target_type STREQUAL "EXECUTABLE")
      myengine_rl_steam_runtime(${test_target})
    endif()
  endforeach()
endif()

# An ASan build is several times slower (physics and rendering loops especially): the time limits grow with it
if(MYENGINE_ASAN)
  get_property(myengine_all_tests DIRECTORY PROPERTY TESTS)
  foreach(test_name IN LISTS myengine_all_tests)
    get_test_property(${test_name} TIMEOUT test_timeout)
    if(test_timeout)
      math(EXPR test_timeout "${test_timeout} * 10")
      set_tests_properties(${test_name} PROPERTIES TIMEOUT ${test_timeout})
    endif()
  endforeach()
endif()

# G7c: the audio module without a sound device: WAV, 3D, the pool of voices, buses, fades, the sound bank, the sounds of the project
add_executable(myengine_audio_tests
  audio/AudioTests.cpp)

target_link_libraries(myengine_audio_tests PRIVATE myengine_engine)
myengine_apply_compiler_options(myengine_audio_tests)
set_property(TARGET myengine_audio_tests PROPERTY FOLDER "Tests")
add_test(NAME audio COMMAND myengine_audio_tests)
set_tests_properties(audio PROPERTIES TIMEOUT 120)

# G7c: the particle simulation without a renderer: presets, bursts and rates, the budget, batches, the cost of 5000 particles
add_executable(myengine_fx_tests
  fx/FxTests.cpp)

target_link_libraries(myengine_fx_tests PRIVATE myengine_engine)
myengine_apply_compiler_options(myengine_fx_tests)
set_property(TARGET myengine_fx_tests PROPERTY FOLDER "Tests")
add_test(NAME fx COMMAND myengine_fx_tests)
set_tests_properties(fx PROPERTIES TIMEOUT 120)

# G7d: the sounds and effects of a Rocket League match from the state of the simulation: events of pairs of ticks, no doubles after a
# rollback, the commands that reach the audio and particle systems (no device, no renderer), a bot match with the journal
add_executable(myengine_rl_feedback_tests
  rl/RlFeedbackTests.cpp)

target_link_libraries(myengine_rl_feedback_tests PRIVATE myengine_engine)
myengine_apply_compiler_options(myengine_rl_feedback_tests)
set_property(TARGET myengine_rl_feedback_tests PROPERTY FOLDER "Tests")
add_test(NAME rl_feedback COMMAND myengine_rl_feedback_tests)
set_tests_properties(rl_feedback PROPERTIES TIMEOUT 300)

# G4g: the training: free play, the packs of exercises (assets/data/training), the conditions of success, the attempts, the results file (no window)
add_executable(myengine_rl_training_tests
  rl/RlTrainingTests.cpp)

target_link_libraries(myengine_rl_training_tests PRIVATE myengine_engine)
target_compile_definitions(myengine_rl_training_tests PRIVATE TRAINING_PACKS_DIR="${CMAKE_SOURCE_DIR}/assets/data/training")
myengine_apply_compiler_options(myengine_rl_training_tests)
set_property(TARGET myengine_rl_training_tests PROPERTY FOLDER "Tests")
add_test(NAME rl_training COMMAND myengine_rl_training_tests)
set_tests_properties(rl_training PROPERTIES TIMEOUT 300)

# G4i: the cars of the garage: the data (the hitboxes of RocketSim), the meshes and previews they name, the bars, broken data, the key in the settings (no window)
add_executable(myengine_rl_cars_tests
  rl/RlCarsTests.cpp)

target_link_libraries(myengine_rl_cars_tests PRIVATE myengine_engine)
myengine_apply_compiler_options(myengine_rl_cars_tests)
set_property(TARGET myengine_rl_cars_tests PROPERTY FOLDER "Tests")
add_test(NAME rl_cars COMMAND myengine_rl_cars_tests)
set_tests_properties(rl_cars PROPERTIES TIMEOUT 120)

# G7e: the replay of a goal and the cinematic cameras: the ring of ticks, the clip, the speed profile, the skip, the online case, the cameras,
# RlFeedback in the replay mode (no window)
add_executable(myengine_rl_replay_tests
  rl/RlReplayTests.cpp)

target_link_libraries(myengine_rl_replay_tests PRIVATE myengine_engine)
myengine_apply_compiler_options(myengine_rl_replay_tests)
set_property(TARGET myengine_rl_replay_tests PROPERTY FOLDER "Tests")
add_test(NAME rl_replay COMMAND myengine_rl_replay_tests)
set_tests_properties(rl_replay PROPERTIES TIMEOUT 300)

# G1-kit: the UI kit of the game without a window: the input, the focus, the widgets, the HUD logic, the layout at several window sizes, a draw of the gallery
add_executable(myengine_rl_ui_tests
  ui/RlUiTests.cpp)

target_link_libraries(myengine_rl_ui_tests PRIVATE myengine_engine)
target_include_directories(myengine_rl_ui_tests PRIVATE "${CMAKE_SOURCE_DIR}/external/imgui/Include")
myengine_apply_compiler_options(myengine_rl_ui_tests)
set_property(TARGET myengine_rl_ui_tests PROPERTY FOLDER "Tests")
add_test(NAME rl_ui COMMAND myengine_rl_ui_tests)
set_tests_properties(rl_ui PROPERTIES TIMEOUT 120)

# G1-flow: the game mode of Rocket League without a window: the settings file, the script of the UI tests, and RlGameSystem driven through the commands of the
# screens (a match from the start to the results, the pause, the feed, the lobby, the controls)
add_executable(myengine_rl_game_tests
  ui/RlGameTests.cpp)

target_link_libraries(myengine_rl_game_tests PRIVATE myengine_engine)
myengine_apply_compiler_options(myengine_rl_game_tests)
set_property(TARGET myengine_rl_game_tests PROPERTY FOLDER "Tests")
add_test(NAME rl_game COMMAND myengine_rl_game_tests)
set_tests_properties(rl_game PROPERTIES TIMEOUT 300)

# W1: the sound engine of Wwise starts and stops without a device (cmake/Wwise.cmake, docs/engine/wwise.md). Only with -DMYENGINE_WWISE=ON.
if(MYENGINE_WWISE)
  add_executable(myengine_wwise_runtime_tests
    audio/WwiseRuntimeTests.cpp)

  target_link_libraries(myengine_wwise_runtime_tests PRIVATE myengine_engine)
  myengine_apply_compiler_options(myengine_wwise_runtime_tests)
  set_property(TARGET myengine_wwise_runtime_tests PROPERTY FOLDER "Tests")
  add_test(NAME audio_wwise_runtime COMMAND myengine_wwise_runtime_tests)
  set_tests_properties(audio_wwise_runtime PROPERTIES TIMEOUT 120)

  # W3: audio::AudioSystem on Wwise: the banks, every event of audio_bank.json, the end of a sound, the loops, the buses, the positions (no device, offline time)
  add_executable(myengine_wwise_audio_tests
    audio/WwiseAudioTests.cpp)

  # the test asks the sound engine itself (Query) what the AudioSystem has done: it has the SDK too
  target_link_libraries(myengine_wwise_audio_tests PRIVATE myengine_engine myengine_wwise)
  myengine_apply_compiler_options(myengine_wwise_audio_tests)
  set_property(TARGET myengine_wwise_audio_tests PROPERTY FOLDER "Tests")
  add_test(NAME audio_wwise COMMAND myengine_wwise_audio_tests)
  set_tests_properties(audio_wwise PROPERTIES TIMEOUT 300)

  # W4: the stability of that: 10 000 events over simulated minutes with a full pool, handles that are old or not given, a bank twice and banks that are cut,
  # 20 starts and stops (docs/measurements/wwise/stability.md)
  add_executable(myengine_wwise_stress_tests
    audio/WwiseStressTests.cpp)

  target_link_libraries(myengine_wwise_stress_tests PRIVATE myengine_engine)
  myengine_apply_compiler_options(myengine_wwise_stress_tests)
  set_property(TARGET myengine_wwise_stress_tests PROPERTY FOLDER "Tests")
  add_test(NAME audio_wwise_stress COMMAND myengine_wwise_stress_tests)
  set_tests_properties(audio_wwise_stress PROPERTIES TIMEOUT 60)
endif()

# F0: Face ID without a camera: the decisions, the alignment, the profile file and (when ONNX Runtime and the models are there) YuNet + SFace on two public domain
# portraits. Exit code 77: no ONNX Runtime, models or portraits (tools/faceid/fetch_models.ps1 -WithTestImage), the test is skipped.
add_executable(myengine_faceid_tests
  faceid/FaceIdTests.cpp)

target_link_libraries(myengine_faceid_tests PRIVATE myengine_engine)
myengine_apply_compiler_options(myengine_faceid_tests)
myengine_copy_runtime_assets(myengine_faceid_tests)
set_property(TARGET myengine_faceid_tests PROPERTY FOLDER "Tests")
add_test(NAME faceid COMMAND myengine_faceid_tests)
set_tests_properties(faceid PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)

# N0 (FlyDoom): the fly brain model on graphs built in code: LIF rate vs analytic, refractoriness, the 1.8 ms delay, Poisson
# input, spikes of Brian2 (Shiu's equations) one for one, the event-driven simulation vs a step-by-step reference, bit-identical spikes on the job system (docs/flybrain/flybrain.md)
add_executable(myengine_flybrain_tests
  flybrain/FlyBrainTests.cpp
  flybrain/BrianGolden.h
  flybrain/SyntheticGraph.h)

target_link_libraries(myengine_flybrain_tests PRIVATE myengine_flybrain myengine_engine)
myengine_apply_compiler_options(myengine_flybrain_tests)
set_property(TARGET myengine_flybrain_tests PROPERTY FOLDER "Tests")
add_test(NAME flybrain COMMAND myengine_flybrain_tests)
set_tests_properties(flybrain PROPERTIES TIMEOUT 120)

# N0: the same on the real connectome (assets/flybrain/malecns_v1.fbrain, tools/flybrain/fetch_malecns.ps1). Skipped (exit
# code 77) when the file is not downloaded
add_executable(myengine_flybrain_connectome_tests
  flybrain/FlyBrainConnectomeTests.cpp)

target_link_libraries(myengine_flybrain_connectome_tests PRIVATE myengine_flybrain myengine_engine)
myengine_apply_compiler_options(myengine_flybrain_connectome_tests)
set_property(TARGET myengine_flybrain_connectome_tests PROPERTY FOLDER "Tests")
add_test(NAME flybrain_connectome COMMAND myengine_flybrain_connectome_tests)
set_tests_properties(flybrain_connectome PROPERTIES TIMEOUT 300 SKIP_RETURN_CODE 77)

# N0: speed of the fly brain against real time, 1/2/4/8 threads (manual, not in ctest: minutes)
add_executable(myengine_flybrain_bench
  flybrain/FlyBrainBench.cpp
  flybrain/SyntheticGraph.h)

target_link_libraries(myengine_flybrain_bench PRIVATE myengine_flybrain myengine_engine psapi)
myengine_apply_compiler_options(myengine_flybrain_bench)
set_property(TARGET myengine_flybrain_bench PROPERTY FOLDER "Tests")

# V0: the 3D view of the fly brain (game/brainview): layout, flash buffer, activity of the groups, camera, the runner on a small brain, and the picture of a point
# cloud on a real DX12 device (or WARP) read back from a render target. Exit code 77: no device, desktop or shader next to the test (the rest has run).
add_executable(myengine_brainview_tests
  brainview/BrainViewTests.cpp)

target_link_libraries(myengine_brainview_tests PRIVATE myengine_brainview myengine_flybrain myengine_engine)
myengine_apply_compiler_options(myengine_brainview_tests)
myengine_copy_runtime_assets(myengine_brainview_tests)
set_property(TARGET myengine_brainview_tests PROPERTY FOLDER "Tests")
add_test(NAME brainview COMMAND myengine_brainview_tests)
set_tests_properties(brainview PROPERTIES TIMEOUT 180 SKIP_RETURN_CODE 77)

# Manual (not CTest): the fly brain on the real connectome without the editor: --bench measures the frame time of the cloud with the brain running next to it, --out saves
# frames of the picture (the evidence of V0)
add_executable(myengine_brainview_demo
  brainview/BrainViewDemo.cpp)

target_link_libraries(myengine_brainview_demo PRIVATE myengine_brainview myengine_flybrain myengine_engine)
myengine_apply_compiler_options(myengine_brainview_demo)
myengine_copy_runtime_assets(myengine_brainview_demo)
set_property(TARGET myengine_brainview_demo PROPERTY FOLDER "Tests")
# N0b: the GPU backend of the fly brain (DX12 compute, assets/shaders/flybrain.hlsl) against the CPU: the same spikes on a
# synthetic graph and on the connectome when it is downloaded. Skipped (exit code 77) without a DX12 adapter with doubles
add_executable(myengine_flybrain_gpu_tests
  flybrain/FlyBrainGpuTests.cpp
  flybrain/SyntheticGraph.h)

target_link_libraries(myengine_flybrain_gpu_tests PRIVATE myengine_flybrain myengine_engine)
myengine_apply_compiler_options(myengine_flybrain_gpu_tests)
myengine_copy_runtime_assets(myengine_flybrain_gpu_tests)
set_property(TARGET myengine_flybrain_gpu_tests PROPERTY FOLDER "Tests")
add_test(NAME flybrain_gpu COMMAND myengine_flybrain_gpu_tests)
set_tests_properties(flybrain_gpu PROPERTIES TIMEOUT 300 SKIP_RETURN_CODE 77)

# N2: the brains of the swarm (FlySwarmBrain): replicas = single brains, the batch on the GPU = on the CPU, and with the
# connectome the cut-out circuit (size, silence, looming -> escape and steering, speed of 32 flies)
add_executable(myengine_flybrain_swarm_tests
  flybrain/FlyBrainSwarmTests.cpp
  flybrain/SyntheticGraph.h)

target_link_libraries(myengine_flybrain_swarm_tests PRIVATE myengine_flybrain myengine_engine)
myengine_apply_compiler_options(myengine_flybrain_swarm_tests)
myengine_copy_runtime_assets(myengine_flybrain_swarm_tests)
set_property(TARGET myengine_flybrain_swarm_tests PROPERTY FOLDER "Tests")
add_test(NAME flybrain_swarm COMMAND myengine_flybrain_swarm_tests)
set_tests_properties(flybrain_swarm PROPERTIES TIMEOUT 300 SKIP_RETURN_CODE 77)

# N2: cuts the swarm circuit out of MaleCNS into assets/flybrain/swarm_circuit.fbrain (manual)
add_executable(myengine_flybrain_circuit
  flybrain/FlyBrainCircuitTool.cpp)

target_link_libraries(myengine_flybrain_circuit PRIVATE myengine_flybrain myengine_engine)
myengine_apply_compiler_options(myengine_flybrain_circuit)
set_property(TARGET myengine_flybrain_circuit PROPERTY FOLDER "Tests")

# N1: the fly plays DOOM (game/doomfly, docs/game/flydoom-fly.md): the eyes, the controls, the steering by the picture, damage, a brain that runs away, E1M1.
# Skipped (exit code 77) without the connectome (tools/flybrain/fetch_malecns.ps1); the game of E1M1 needs a WAD (tools/doom/fetch_freedoom.ps1, or the shareware doom1.wad)
add_executable(myengine_doom_fly_tests
  doomfly/DoomFlyTests.cpp)
target_link_libraries(myengine_doom_fly_tests PRIVATE myengine_doomfly myengine_flybrain myengine_doom myengine_engine)
target_compile_definitions(myengine_doom_fly_tests PRIVATE FLY_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
myengine_apply_compiler_options(myengine_doom_fly_tests)
set_property(TARGET myengine_doom_fly_tests PROPERTY FOLDER "Tests")
add_test(NAME doom_fly COMMAND myengine_doom_fly_tests)
set_tests_properties(doom_fly PROPERTIES TIMEOUT 900 SKIP_RETURN_CODE 77)

# V1: the menu of FlyDoom without a window: the settings file, the levels of a WAD by its name, the arguments of DOOM that start one
add_executable(myengine_doom_frontend_tests
  doomflow/DoomFrontEndTests.cpp)

target_link_libraries(myengine_doom_frontend_tests PRIVATE myengine_engine)
myengine_apply_compiler_options(myengine_doom_frontend_tests)
set_property(TARGET myengine_doom_frontend_tests PROPERTY FOLDER "Tests")
add_test(NAME doom_frontend COMMAND myengine_doom_frontend_tests)
set_tests_properties(doom_frontend PROPERTIES TIMEOUT 60)

# D1c: the swarm of enemy flies (game/doomfly FlySwarm, game/doom DoomSwarm): the pictures of the fly, the brain of the circuit at looming, a game of E1M1 with waves.
# The brain and the game need assets/flybrain/swarm_circuit.fbrain (build/tests/RelWithDebInfo/myengine_flybrain_circuit.exe) and the game a WAD
# (tools/doom/fetch_freedoom.ps1, or the shareware doom1.wad); they are skipped without them
add_executable(myengine_doom_swarm_tests
  doomfly/SwarmTests.cpp)
target_link_libraries(myengine_doom_swarm_tests PRIVATE myengine_doomfly myengine_flybrain myengine_doom myengine_engine)
target_compile_definitions(myengine_doom_swarm_tests PRIVATE FLY_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
myengine_apply_compiler_options(myengine_doom_swarm_tests)
set_property(TARGET myengine_doom_swarm_tests PROPERTY FOLDER "Tests")
add_test(NAME doom_fly_swarm COMMAND myengine_doom_swarm_tests)
set_tests_properties(doom_fly_swarm PROPERTIES TIMEOUT 900 SKIP_RETURN_CODE 77)
# D1: what the motion and looming neurons answer to on synthetic scenes (manual tool, not in ctest): myengine_doomfly_vision [gpu|cpu] [onGain] [offGain]
add_executable(myengine_doomfly_vision
  doomfly/FlyVision.cpp)
target_link_libraries(myengine_doomfly_vision PRIVATE myengine_doomfly myengine_flybrain myengine_engine)
myengine_apply_compiler_options(myengine_doomfly_vision)
set_property(TARGET myengine_doomfly_vision PROPERTY FOLDER "Tests")

# N3: the fly learns: KC -> MBON synapses weaken by the dopamine of their compartment (conditioning on a synthetic
# mushroom body, the memory file, CPU = GPU, and the PPL101 compartments of MaleCNS when the connectome is there)
add_executable(myengine_flybrain_plasticity_tests
  flybrain/FlyBrainPlasticityTests.cpp)

target_link_libraries(myengine_flybrain_plasticity_tests PRIVATE myengine_flybrain myengine_engine)
myengine_apply_compiler_options(myengine_flybrain_plasticity_tests)
myengine_copy_runtime_assets(myengine_flybrain_plasticity_tests)
set_property(TARGET myengine_flybrain_plasticity_tests PROPERTY FOLDER "Tests")
add_test(NAME flybrain_plasticity COMMAND myengine_flybrain_plasticity_tests)
set_tests_properties(flybrain_plasticity PROPERTIES TIMEOUT 300)
