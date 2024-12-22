add_executable(ZenCoreTest ZenCoreTest/main.cpp)
add_executable(ThreadPoolTest ThreadPoolTest/main.cpp)
add_executable(CommonTest
    CommonTest/SmallVectorTests.cpp
    CommonTest/MemoryTests.cpp
    CommonTest/LockTests.cpp
    CommonTest/PagedAllocatorTest.h
    CommonTest/PagedAllocatorTest.cpp
)
add_executable(SmartPtrTest
    SmartPtrTest/SharedPtrTests.cpp
    SmartPtrTest/UniquePtrTests.cpp
    SmartPtrTest/IntrusivePtrTests.cpp
    SmartPtrTest/RefCountPtrTests.cpp
)

# VulkanRHI (v2) Exmaples
add_executable(hello_triangle
    VulkanRHIDemo/HelloTriangle/HelloTriangle.cpp
    VulkanRHIDemo/HelloTriangle/HelloTriangle.h
    VulkanRHIDemo/Application.h
    VulkanRHIDemo/Application.cpp
)
target_link_libraries(hello_triangle ZenCore)

add_executable(gears
        VulkanRHIDemo/Gears/Gears.cpp
        VulkanRHIDemo/Gears/Gears.h
        VulkanRHIDemo/Application.h
        VulkanRHIDemo/Application.cpp
)
target_link_libraries(gears ZenCore)

add_executable(push_constants
    VulkanRHIDemo/PushConstants/PushConstants.cpp
    VulkanRHIDemo/PushConstants/PushConstants.h
    VulkanRHIDemo/Application.h
    VulkanRHIDemo/Application.cpp
)
target_link_libraries(push_constants ZenCore)

add_executable(specialization_constants
    VulkanRHIDemo/SpecializationConstants/SpecializationConstants.cpp
    VulkanRHIDemo/SpecializationConstants/SpecializationConstants.h
    VulkanRHIDemo/Application.h
    VulkanRHIDemo/Application.cpp
)
target_link_libraries(specialization_constants ZenCore)

add_executable(offscreen
        VulkanRHIDemo/Offscreen/Offscreen.cpp
        VulkanRHIDemo/Offscreen/Offscreen.h
        VulkanRHIDemo/Application.h
        VulkanRHIDemo/Application.cpp
)
target_link_libraries(offscreen ZenCore)

add_executable(shadowmapping
    VulkanRHIDemo/ShadowMapping/ShadowMapping.cpp
    VulkanRHIDemo/ShadowMapping/ShadowMapping.h
    VulkanRHIDemo/Application.h
    VulkanRHIDemo/Application.cpp
)
target_link_libraries(shadowmapping ZenCore)

add_executable(scene_renderer_demo
        VulkanRHIDemo/SceneRenderer/SceneRendererDemo.cpp
        VulkanRHIDemo/SceneRenderer/SceneRendererDemo.h)
target_link_libraries(scene_renderer_demo ZenCore)

target_link_libraries(ZenCoreTest ZenCore)
target_link_libraries(ThreadPoolTest ZenCore)
target_link_libraries(SmartPtrTest ZenCore gtest_main)
target_link_libraries(CommonTest ZenCore gtest_main)


# Applications
add_executable(HelloTriangle
    Applications/Application.h
    Applications/HelloTriangle/HelloTriangle.cpp
    Applications/HelloTriangle/HelloTriangle.h
    Applications/HelloTriangle/Entry.cpp
)
add_executable(GLTFViewer
    Applications/Application.h
    Applications/GLTFViewer/GLTFViewer.cpp
    Applications/GLTFViewer/GLTFViewer.h
    Applications/GLTFViewer/Entry.cpp
)

add_executable(SceneGraphDemo
    Applications/Application.h
    Applications/SceneGraphDemo/SceneGraphDemo.cpp
    Applications/SceneGraphDemo/SceneGraphDemo.h
    Applications/SceneGraphDemo/Entry.cpp
)

target_link_libraries(HelloTriangle ZenCore)
target_link_libraries(GLTFViewer ZenCore)
target_link_libraries(SceneGraphDemo ZenCore)