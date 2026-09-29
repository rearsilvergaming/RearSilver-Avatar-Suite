cmake_minimum_required(VERSION 3.20)
project(RearSilverAvatarSuite LANGUAGES CXX)
set(SPOUT2_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/spout2/SPOUTSDK")
add_library(RearSilverSpout2 STATIC
  "${SPOUT2_ROOT}/SpoutDirectX/SpoutDX/SpoutDX.cpp"
  "${SPOUT2_ROOT}/SpoutGL/SpoutCopy.cpp"
  "${SPOUT2_ROOT}/SpoutGL/SpoutDirectX.cpp"
  "${SPOUT2_ROOT}/SpoutGL/SpoutFrameCount.cpp"
  "${SPOUT2_ROOT}/SpoutGL/SpoutSenderNames.cpp"
  "${SPOUT2_ROOT}/SpoutGL/SpoutSharedMemory.cpp"
  "${SPOUT2_ROOT}/SpoutGL/SpoutUtils.cpp")
target_compile_features(RearSilverSpout2 PRIVATE cxx_std_17)
target_compile_definitions(RearSilverSpout2 PRIVATE SPOUT_BUILD_STATIC UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_include_directories(RearSilverSpout2 PUBLIC
  "${SPOUT2_ROOT}/SpoutDirectX/SpoutDX"
  "${SPOUT2_ROOT}/SpoutGL")
target_link_libraries(RearSilverSpout2 PUBLIC opengl32 kernel32 user32 gdi32 winspool comdlg32 comctl32 advapi32 shell32 ole32 oleaut32 uuid odbc32 odbccp32 d3d9 d3d11 dxgi version winmm)
if(MSVC)
  target_compile_options(RearSilverSpout2 PRIVATE /EHsc)
endif()
add_executable(RearSilverAvatarSuite WIN32 src/main.cpp src/settings_window.cpp src/audio_monitor.cpp src/preset_store.cpp src/layer_model.cpp src/app.rc)
set_target_properties(RearSilverAvatarSuite PROPERTIES OUTPUT_NAME "RearSilver Avatar Suite")
target_compile_features(RearSilverAvatarSuite PRIVATE cxx_std_17)
if(MSVC)
  target_compile_options(RearSilverAvatarSuite PRIVATE /utf-8 /EHsc)
endif()
target_compile_definitions(RearSilverAvatarSuite PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_include_directories(RearSilverAvatarSuite PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/third_party/webview2/include")
target_link_libraries(RearSilverAvatarSuite PRIVATE d3d11 dxgi dcomp d3dcompiler windowscodecs ole32 propsys comdlg32 gdi32 msimg32 user32 version bcrypt
  RearSilverSpout2
  "${CMAKE_CURRENT_SOURCE_DIR}/third_party/webview2/lib/x64/WebView2Loader.dll.lib")
add_custom_command(TARGET RearSilverAvatarSuite POST_BUILD
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/webview2/bin/x64/WebView2Loader.dll"
    "$<TARGET_FILE_DIR:RearSilverAvatarSuite>"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/avatar-settings.html"
    "$<TARGET_FILE_DIR:RearSilverAvatarSuite>/avatar-settings.html"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/spout2/LICENSE"
    "$<TARGET_FILE_DIR:RearSilverAvatarSuite>/spout2-license.txt"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/Branding/RearSilver Avatar Suite - Header.png"
    "$<TARGET_FILE_DIR:RearSilverAvatarSuite>/settings-header.png"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/Branding/rearsilver-avatar-suite-splash.png"
    "$<TARGET_FILE_DIR:RearSilverAvatarSuite>/splash.png"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/Sora-Variable.ttf"
    "$<TARGET_FILE_DIR:RearSilverAvatarSuite>/Sora-Variable.ttf"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/Branding/RearSilver Avatar Suite - Mascot - Idle.png"
    "$<TARGET_FILE_DIR:RearSilverAvatarSuite>/default-avatar-idle.png"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/Branding/RearSilver Avatar Suite - Mascot - Reaction.png"
    "$<TARGET_FILE_DIR:RearSilverAvatarSuite>/default-avatar-reaction.png"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/Branding/Rail Icons_RearSilver Avatar Suite - Rail - Presets.png"
    "$<TARGET_FILE_DIR:RearSilverAvatarSuite>/rail-presets.png"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/Branding/Rail Icons_RearSilver Avatar Suite - Rail - Reactions - On.png"
    "$<TARGET_FILE_DIR:RearSilverAvatarSuite>/rail-reactions-on.png"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/Branding/Rail Icons_RearSilver Avatar Suite - Rail - Reactions - Off.png"
    "$<TARGET_FILE_DIR:RearSilverAvatarSuite>/rail-reactions-off.png"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/Branding/Rail Icons_RearSilver Avatar Suite - Rail - WebSocket.png"
    "$<TARGET_FILE_DIR:RearSilverAvatarSuite>/rail-websocket.png"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/Branding/Rail Icons_RearSilver Avatar Suite - Rail - Background.png"
    "$<TARGET_FILE_DIR:RearSilverAvatarSuite>/rail-background.png"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/Branding/Rail Icons_RearSilver Avatar Suite - Rail - Tools.png"
    "$<TARGET_FILE_DIR:RearSilverAvatarSuite>/rail-tools.png"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/Branding/Rail Icons_RearSilver Avatar Suite - Rail - Settings.png"
    "$<TARGET_FILE_DIR:RearSilverAvatarSuite>/rail-settings.png")

# Preserve Diagnostic 18 as the known-good control while the production renderer is integrated.
add_executable(RearSilverAvatarDiagnostic18 WIN32 EXCLUDE_FROM_ALL src/sdl_diagnostic.cpp)
set_target_properties(RearSilverAvatarDiagnostic18 PROPERTIES OUTPUT_NAME "RearSilver Avatar Suite-Diagnostic18-Control")
target_compile_features(RearSilverAvatarDiagnostic18 PRIVATE cxx_std_17)
if(MSVC)
  target_compile_options(RearSilverAvatarDiagnostic18 PRIVATE /utf-8 /EHsc)
endif()
target_compile_definitions(RearSilverAvatarDiagnostic18 PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(RearSilverAvatarDiagnostic18 PRIVATE d3d11 dxgi d3dcompiler windowscodecs ole32 comdlg32 gdi32 user32 version)

# Disposable architecture diagnostic: one top-level window, one fixed 960x720
# DXGI capture swap chain, and a separate GDI preview.
add_executable(RearSilverAvatarArchitectureDiagnostic WIN32 EXCLUDE_FROM_ALL src/architecture_diagnostic.cpp)
set_target_properties(RearSilverAvatarArchitectureDiagnostic PROPERTIES OUTPUT_NAME "RearSilver Avatar Suite-ArchitectureDiagnostic")
target_compile_features(RearSilverAvatarArchitectureDiagnostic PRIVATE cxx_std_17)
if(MSVC)
  target_compile_options(RearSilverAvatarArchitectureDiagnostic PRIVATE /utf-8 /EHsc)
endif()
target_compile_definitions(RearSilverAvatarArchitectureDiagnostic PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(RearSilverAvatarArchitectureDiagnostic PRIVATE d3d11 dxgi comdlg32 gdi32 user32)

# Diagnostic 19: fixed composition swap chain presented through DirectComposition.
# This target tests OBS Game Capture acquisition only and is not part of production.
add_executable(RearSilverAvatarDirectCompositionDiagnostic WIN32 EXCLUDE_FROM_ALL src/directcomposition_diagnostic.cpp)
set_target_properties(RearSilverAvatarDirectCompositionDiagnostic PROPERTIES OUTPUT_NAME "RearSilver Avatar Suite-Diagnostic19-DirectComposition")
target_compile_features(RearSilverAvatarDirectCompositionDiagnostic PRIVATE cxx_std_17)
if(MSVC)
  target_compile_options(RearSilverAvatarDirectCompositionDiagnostic PRIVATE /utf-8 /EHsc)
endif()
target_compile_definitions(RearSilverAvatarDirectCompositionDiagnostic PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(RearSilverAvatarDirectCompositionDiagnostic PRIVATE d3d11 dxgi dcomp ole32 user32)
