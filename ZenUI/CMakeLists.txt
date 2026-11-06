# Each library has its own include root, so a consumer of the neutral renderer cannot
# include ImGui or runtime-panel headers it does not link.

# Toolkit-independent draw packets, texture registry and RDG renderer.
add_library(ZenUI STATIC Renderer/Source/UIRenderer.cpp Renderer/Source/UIDrawPacket.cpp)
target_include_directories(ZenUI PUBLIC Renderer/Include)
target_link_libraries(ZenUI PUBLIC ZenCore)

# ImGui adapter: context, platform bridge, fonts and draw-packet conversion.
add_library(ZenImGui STATIC ImGui/Source/UIContext.cpp ImGui/Source/ImGuiDrawPacket.cpp ImGui/Source/ImGuiRenderer.cpp
  ImGui/Source/LightingBackendControls.cpp)
target_include_directories(ZenImGui PUBLIC ImGui/Include)
target_link_libraries(ZenImGui PUBLIC ZenUI zen_imgui PRIVATE ZenWindowBackendInternal)

# Session diagnostics are shared by editor-hosted ZenApp and the standalone app.
add_library(ZenSessionUI STATIC Runtime/Source/SessionDebugUI.cpp)
target_include_directories(ZenSessionUI PUBLIC Runtime/Include)
target_link_libraries(ZenSessionUI PUBLIC ZenImGui)
