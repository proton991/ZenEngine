# Each library has its own include root, so a layer cannot include headers of a layer
# it does not link. Dependencies point downward: UI -> Services -> Render -> Model.

# Editor state, read-only scene queries, selection, actions and preferences.
# No window, GPU device or UI toolkit.
add_library(ZenEditorModel STATIC
  Model/Source/EditorRenderingState.cpp
  Model/Source/EditorActions.cpp
  Model/Source/EditorCamera.cpp
  Model/Source/EditorEnvironment.cpp
  Model/Source/EditorLog.cpp
  Model/Source/EditorPreferences.cpp
  Model/Source/EditorScene.cpp
  Model/Source/EditorSelection.cpp
  Model/Source/EditorText.cpp
  Model/Source/InspectorNavigation.cpp
  Model/Source/NodeInspection.cpp
  Model/Source/SceneAssets.cpp
  Model/Source/ViewAxes.cpp)
target_include_directories(ZenEditorModel PUBLIC Model/Include)
target_link_libraries(ZenEditorModel PUBLIC ZenCore)

# GPU publication, offscreen images, selection overlay, picking and previews.
# Reads the model and never changes it.
add_library(ZenEditorRender STATIC
  Rendering/Source/EditorShaders.cpp
  Rendering/Source/EditorViewport.cpp
  Rendering/Source/MeshPreviewRenderer.cpp)
target_include_directories(ZenEditorRender PUBLIC Rendering/Include)
target_link_libraries(ZenEditorRender PUBLIC ZenEditorModel)

# Transactions that span the model and render services, and the core actions.
add_library(ZenEditorServices STATIC Services/Source/EditorController.cpp)
target_include_directories(ZenEditorServices PUBLIC Services/Include)
target_link_libraries(ZenEditorServices PUBLIC ZenEditorRender)

# Native title-bar behavior; no UI toolkit types.
add_library(ZenEditorPlatform STATIC Platform/Source/EditorWindowChrome.cpp)
target_include_directories(ZenEditorPlatform PUBLIC Platform/Include)
target_link_libraries(ZenEditorPlatform PUBLIC ZenCore)

# The ImGui frontend: workspace, panels, theme and input translation.
add_library(ZenEditorUI STATIC
  ImGui/Source/EditorLayout.cpp
  ImGui/Source/EditorPanel.cpp
  ImGui/Source/EditorShortcuts.cpp
  ImGui/Source/EditorTheme.cpp
  ImGui/Source/EditorWidgets.cpp
  ImGui/Source/EditorWorkspace.cpp
  ImGui/Source/InspectorNavigationWidget.cpp
  ImGui/Source/SceneLoadingDialog.cpp
  ImGui/Source/MeshPreviewWidget.cpp
  ImGui/Source/SceneViewOverlays.cpp
  ImGui/Source/LightMoveWidget.cpp
  ImGui/Source/Panels/AssetsPanel.cpp
  ImGui/Source/Panels/CameraSettingsPanel.cpp
  ImGui/Source/Panels/HierarchyPanel.cpp
  ImGui/Source/Panels/InspectorPanel.cpp
  ImGui/Source/Panels/OutputPanel.cpp
  ImGui/Source/Panels/RenderSettingsPanel.cpp
  ImGui/Source/Panels/ScenePanel.cpp)
target_include_directories(ZenEditorUI PUBLIC ImGui/Include PRIVATE ImGui/Source)
FetchContent_GetProperties(zen_imgui SOURCE_DIR ZEN_EDITOR_IMGUI_SOURCE_DIR)
target_compile_definitions(ZenEditorUI PRIVATE ZEN_EDITOR_FONT_PATH="${ZEN_EDITOR_IMGUI_SOURCE_DIR}/misc/fonts/Roboto-Medium.ttf")
target_link_libraries(ZenEditorUI PUBLIC ZenEditorServices ZenEditorPlatform ZenImGui)

add_executable(zen_editor App/Main.cpp App/EditorApplication.cpp)
target_link_libraries(zen_editor PRIVATE ZenEditorUI)

if (WIN32)
  enable_language(RC)
  configure_file(App/ZenEditor.rc.in "${CMAKE_CURRENT_BINARY_DIR}/ZenEditor.rc" @ONLY)
  target_sources(zen_editor PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/ZenEditor.rc")
endif ()

if (BUILD_TESTING)
  add_executable(EditorInputTest Tests/EditorInputTests.cpp Tests/LightMoveWidgetTests.cpp Tests/InspectorNavigationWidgetTests.cpp Tests/SceneLoadingDialogTests.cpp)
  target_link_libraries(EditorInputTest PRIVATE ZenEditorUI gtest_main)
  target_include_directories(EditorInputTest PRIVATE ImGui/Source)
  target_compile_definitions(EditorInputTest PRIVATE ZEN_EDITOR_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/Tests/Fixtures/")
  add_test(NAME EditorInputTest COMMAND EditorInputTest)
  set_tests_properties(EditorInputTest PROPERTIES LABELS "unit" TIMEOUT 30)
  if (WIN32)
    add_executable(EditorWindowChromeTest Tests/EditorWindowChromeTests.cpp)
    target_link_libraries(EditorWindowChromeTest PRIVATE ZenEditorPlatform gtest_main ZenWindowBackendInternal)
    target_include_directories(EditorWindowChromeTest PRIVATE ${PROJECT_SOURCE_DIR}/ZenSamples/CommonTest)
    add_test(NAME EditorWindowChromeTest COMMAND EditorWindowChromeTest)
    set_tests_properties(EditorWindowChromeTest PROPERTIES LABELS "native" TIMEOUT 30)
  endif ()
  add_executable(EditorModelTest Tests/EditorModelTests.cpp Tests/InspectorNavigationTests.cpp Tests/EnvironmentTests.cpp Tests/RenderingSettingsTests.cpp)
  target_link_libraries(EditorModelTest PRIVATE ZenEditorModel gtest_main)
  target_compile_definitions(EditorModelTest PRIVATE ZEN_EDITOR_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/Tests/Fixtures/")
  add_test(NAME EditorModelTest COMMAND EditorModelTest)
  set_tests_properties(EditorModelTest PROPERTIES LABELS "unit" TIMEOUT 120)
  add_executable(EditorRenderingTest Tests/EditorRenderingTests.cpp)
  target_link_libraries(EditorRenderingTest PRIVATE ZenEditorServices gtest_main)
  target_compile_definitions(EditorRenderingTest PRIVATE
    ZEN_EDITOR_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/Tests/Fixtures/"
    ZEN_EDITOR_SHARED_FIXTURES="${PROJECT_SOURCE_DIR}/ZenSamples/CommonTest/Assets/")
  add_test(NAME EditorRenderingTest COMMAND EditorRenderingTest)
  set_tests_properties(EditorRenderingTest PROPERTIES LABELS "native;vulkan" TIMEOUT 120)
  add_executable(EditorPreviewTest Tests/EditorPreviewTests.cpp)
  target_link_libraries(EditorPreviewTest PRIVATE ZenEditorUI gtest_main)
  target_include_directories(EditorPreviewTest PRIVATE ImGui/Source)
  target_compile_definitions(EditorPreviewTest PRIVATE ZEN_EDITOR_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/Tests/Fixtures/")
  add_test(NAME EditorPreviewTest COMMAND EditorPreviewTest)
  set_tests_properties(EditorPreviewTest PROPERTIES LABELS "native;vulkan" TIMEOUT 120)
endif ()
