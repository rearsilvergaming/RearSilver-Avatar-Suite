cmake_minimum_required(VERSION 3.20)
project(RearSilverAvatarSuite LANGUAGES CXX)
include(CTest)
set(RS_BUILD_CHANNEL "Development Build" CACHE STRING "Build channel shown by Avatar Suite")
set(RS_BUILD_VERSION "0.1.0-dev" CACHE STRING "Version shown by Avatar Suite")
set(RS_BUILD_ID "" CACHE STRING "Unique build identifier")
option(RS_BUILD_EXPIRY_ENABLED "Enforce the configured beta expiry" OFF)
set(RS_BUILD_EXPIRY_DATE "2026-10-31" CACHE STRING "Beta expiry date in YYYY-MM-DD format")
set(RS_BUILD_EXPIRY_DISPLAY "31 October 2026" CACHE STRING "Human-readable beta expiry date")
set(RS_ARTIFACT_DIR "" CACHE PATH "Artifact directory associated with this build profile")
option(RS_UPDATE_CHECK_ENABLED "Enable update detection in this build variant" OFF)
set(RS_UPDATE_BASE_URL "" CACHE STRING "HTTPS origin serving RearSilver Avatar Suite update manifests")
set(RS_UPDATE_DOWNLOAD_TOKEN "$ENV{RS_UPDATE_DOWNLOAD_TOKEN}")
if(NOT RS_BUILD_EXPIRY_DATE MATCHES "^[0-9][0-9][0-9][0-9]-[0-9][0-9]-[0-9][0-9]$")
  message(FATAL_ERROR "RS_BUILD_EXPIRY_DATE must use YYYY-MM-DD format")
endif()
string(TIMESTAMP RS_BUILD_DATE "%Y-%m-%d" UTC)
if(NOT RS_BUILD_ID)
  string(TIMESTAMP RS_BUILD_ID "%Y%m%d-%H%M%S" UTC)
endif()
if(RS_BUILD_EXPIRY_ENABLED)
  set(RS_BUILD_EXPIRY_ENABLED_VALUE 1)
else()
  set(RS_BUILD_EXPIRY_ENABLED_VALUE 0)
endif()
if(RS_UPDATE_CHECK_ENABLED)
  set(RS_UPDATE_CHECK_ENABLED_VALUE 1)
else()
  set(RS_UPDATE_CHECK_ENABLED_VALUE 0)
endif()
if(RS_UPDATE_CHECK_ENABLED AND RS_BUILD_CHANNEL STREQUAL "Private Beta" AND NOT RS_UPDATE_DOWNLOAD_TOKEN)
  message(FATAL_ERROR "Private Beta update builds require RS_UPDATE_DOWNLOAD_TOKEN at configure time; never store it in CMakePresets.json or Git.")
endif()
configure_file(src/rs_build_config.hpp.in "${CMAKE_CURRENT_BINARY_DIR}/generated/rs_build_config.hpp" @ONLY)
configure_file(src/version.rc.in "${CMAKE_CURRENT_BINARY_DIR}/generated/version.rc" @ONLY)
file(GLOB BUILT_IN_LAYER_PNGS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/assets/Built In Layers/*.png")
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
add_executable(RearSilverAvatarSuite WIN32 src/main.cpp src/settings_window.cpp src/update_service.cpp src/update_version.cpp src/update_download.cpp src/audio_monitor.cpp src/preset_store.cpp src/layer_model.cpp src/websocket_server.cpp src/app.rc "${CMAKE_CURRENT_BINARY_DIR}/generated/version.rc")
add_executable(RearSilverAvatarAutomation src/automation_sender.cpp)
set_target_properties(RearSilverAvatarAutomation PROPERTIES OUTPUT_NAME "RearSilver Avatar Suite Automation")
target_compile_features(RearSilverAvatarAutomation PRIVATE cxx_std_17)
target_compile_definitions(RearSilverAvatarAutomation PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(RearSilverAvatarAutomation PRIVATE winhttp crypt32)
add_dependencies(RearSilverAvatarSuite RearSilverAvatarAutomation)
add_executable(RearSilverAvatarUpdater WIN32 src/updater/updater_main.cpp)
set_target_properties(RearSilverAvatarUpdater PROPERTIES OUTPUT_NAME "RearSilver-Avatar-Suite-Updater")
target_compile_features(RearSilverAvatarUpdater PRIVATE cxx_std_17)
target_compile_definitions(RearSilverAvatarUpdater PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(RearSilverAvatarUpdater PRIVATE shell32 advapi32 bcrypt)
add_dependencies(RearSilverAvatarSuite RearSilverAvatarUpdater)
set_target_properties(RearSilverAvatarSuite PROPERTIES OUTPUT_NAME "RearSilver Avatar Suite")
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/assets/avatar-settings.html"
  "${CMAKE_CURRENT_BINARY_DIR}/avatar-settings.html" COPYONLY)
target_compile_features(RearSilverAvatarSuite PRIVATE cxx_std_17)
if(MSVC)
  target_compile_options(RearSilverAvatarAutomation PRIVATE /utf-8 /EHsc)
  target_compile_options(RearSilverAvatarUpdater PRIVATE /utf-8 /EHsc)
  target_compile_options(RearSilverAvatarSuite PRIVATE /utf-8 /EHsc)
endif()
target_compile_definitions(RearSilverAvatarSuite PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_include_directories(RearSilverAvatarSuite PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/third_party/webview2/include" "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(RearSilverAvatarSuite PRIVATE d3d11 dxgi dcomp d3dcompiler windowscodecs ole32 propsys comdlg32 gdi32 msimg32 user32 version bcrypt ws2_32 winhttp windowsapp advapi32
  RearSilverSpout2
  "${CMAKE_CURRENT_SOURCE_DIR}/third_party/webview2/lib/x64/WebView2Loader.dll.lib")
if(BUILD_TESTING)
  add_executable(RearSilverAvatarUpdateManifestTests tests/update_manifest_tests.cpp src/update_service.cpp src/update_version.cpp)
  target_compile_features(RearSilverAvatarUpdateManifestTests PRIVATE cxx_std_17)
  target_include_directories(RearSilverAvatarUpdateManifestTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_compile_definitions(RearSilverAvatarUpdateManifestTests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
  target_link_libraries(RearSilverAvatarUpdateManifestTests PRIVATE winhttp windowsapp)
  if(MSVC)
    target_compile_options(RearSilverAvatarUpdateManifestTests PRIVATE /utf-8 /EHsc)
  endif()
  add_test(NAME update-manifest-validation COMMAND RearSilverAvatarUpdateManifestTests)
  add_executable(RearSilverAvatarUpdateDownloadTests tests/update_download_tests.cpp src/update_download.cpp)
  target_compile_features(RearSilverAvatarUpdateDownloadTests PRIVATE cxx_std_17)
  target_include_directories(RearSilverAvatarUpdateDownloadTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_compile_definitions(RearSilverAvatarUpdateDownloadTests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
  target_link_libraries(RearSilverAvatarUpdateDownloadTests PRIVATE winhttp bcrypt)
  if(MSVC)
    target_compile_options(RearSilverAvatarUpdateDownloadTests PRIVATE /utf-8 /EHsc)
  endif()
  add_test(NAME update-download-validation COMMAND RearSilverAvatarUpdateDownloadTests)
endif()
add_custom_command(TARGET RearSilverAvatarSuite POST_BUILD
  COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:RearSilverAvatarSuite>/built-in-layers"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different ${BUILT_IN_LAYER_PNGS} "$<TARGET_FILE_DIR:RearSilverAvatarSuite>/built-in-layers"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/webview2/bin/x64/WebView2Loader.dll"
    "$<TARGET_FILE_DIR:RearSilverAvatarSuite>"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:RearSilverAvatarUpdater>" "$<TARGET_FILE_DIR:RearSilverAvatarSuite>"
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
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/Branding/RearSilver Avatar Suite - Badge.png"
    "$<TARGET_FILE_DIR:RearSilverAvatarSuite>/settings-badge.png"
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
    "$<TARGET_FILE_DIR:RearSilverAvatarSuite>/rail-settings.png"
  COMMAND "${CMAKE_COMMAND}" -E make_directory
    "$<TARGET_FILE_DIR:RearSilverAvatarSuite>/integrations/sammi"
  COMMAND "${CMAKE_COMMAND}" -E make_directory
    "$<TARGET_FILE_DIR:RearSilverAvatarSuite>/integrations/mixitup"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
    "${CMAKE_CURRENT_SOURCE_DIR}/integrations/sammi/RearSilver Avatar Suite.sef"
    "${CMAKE_CURRENT_SOURCE_DIR}/integrations/sammi/RearSilver Avatar Suite - Icon 128.png"
    "${CMAKE_CURRENT_SOURCE_DIR}/integrations/sammi/README.md"
    "$<TARGET_FILE_DIR:RearSilverAvatarSuite>/integrations/sammi"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
    "$<TARGET_FILE:RearSilverAvatarAutomation>"
    "${CMAKE_CURRENT_SOURCE_DIR}/integrations/mixitup/README.md"
    "$<TARGET_FILE_DIR:RearSilverAvatarSuite>/integrations/mixitup"
  COMMAND_EXPAND_LISTS)
if(RS_ARTIFACT_DIR)
  add_custom_command(TARGET RearSilverAvatarSuite POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E remove_directory "${RS_ARTIFACT_DIR}/app"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${RS_ARTIFACT_DIR}/app/built-in-layers"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${RS_ARTIFACT_DIR}/app/integrations/sammi"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${RS_ARTIFACT_DIR}/app/integrations/mixitup"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
      "$<TARGET_FILE:RearSilverAvatarSuite>"
      "${RS_ARTIFACT_DIR}/app/RearSilver Avatar Suite.exe"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
      "$<TARGET_FILE_DIR:RearSilverAvatarSuite>/WebView2Loader.dll"
      "$<TARGET_FILE_DIR:RearSilverAvatarSuite>/RearSilver-Avatar-Suite-Updater.exe"
      "$<TARGET_FILE_DIR:RearSilverAvatarSuite>/avatar-settings.html"
      "$<TARGET_FILE_DIR:RearSilverAvatarSuite>/spout2-license.txt"
      "$<TARGET_FILE_DIR:RearSilverAvatarSuite>/settings-header.png"
      "$<TARGET_FILE_DIR:RearSilverAvatarSuite>/settings-badge.png"
      "$<TARGET_FILE_DIR:RearSilverAvatarSuite>/splash.png"
      "$<TARGET_FILE_DIR:RearSilverAvatarSuite>/Sora-Variable.ttf"
      "$<TARGET_FILE_DIR:RearSilverAvatarSuite>/default-avatar-idle.png"
      "$<TARGET_FILE_DIR:RearSilverAvatarSuite>/default-avatar-reaction.png"
      "$<TARGET_FILE_DIR:RearSilverAvatarSuite>/rail-presets.png"
      "$<TARGET_FILE_DIR:RearSilverAvatarSuite>/rail-reactions-on.png"
      "$<TARGET_FILE_DIR:RearSilverAvatarSuite>/rail-reactions-off.png"
      "$<TARGET_FILE_DIR:RearSilverAvatarSuite>/rail-websocket.png"
      "$<TARGET_FILE_DIR:RearSilverAvatarSuite>/rail-background.png"
      "$<TARGET_FILE_DIR:RearSilverAvatarSuite>/rail-tools.png"
      "$<TARGET_FILE_DIR:RearSilverAvatarSuite>/rail-settings.png"
      "${RS_ARTIFACT_DIR}/app"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
      ${BUILT_IN_LAYER_PNGS}
      "${RS_ARTIFACT_DIR}/app/built-in-layers"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
      "${CMAKE_CURRENT_SOURCE_DIR}/integrations/sammi/RearSilver Avatar Suite.sef"
      "${CMAKE_CURRENT_SOURCE_DIR}/integrations/sammi/RearSilver Avatar Suite - Icon 128.png"
      "${CMAKE_CURRENT_SOURCE_DIR}/integrations/sammi/README.md"
      "${RS_ARTIFACT_DIR}/app/integrations/sammi"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
      "$<TARGET_FILE:RearSilverAvatarAutomation>"
      "${CMAKE_CURRENT_SOURCE_DIR}/integrations/mixitup/README.md"
      "${RS_ARTIFACT_DIR}/app/integrations/mixitup"
    COMMAND_EXPAND_LISTS)
endif()

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
