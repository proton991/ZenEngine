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
add_executable(VulkanRHIDemo VulkanRHIDemo/VulkanRHIDemo.cpp)
add_executable(ShaderUtilDemo VulkanRHIDemo/ShaderUtilDemo.cpp)

target_link_libraries(ZenCoreTest ZenCore)
target_link_libraries(ThreadPoolTest ZenCore)
target_link_libraries(SmartPtrTest ZenCore gtest_main)
target_link_libraries(CommonTest ZenCore gtest_main)
target_link_libraries(VulkanRHIDemo ZenCore)
target_link_libraries(ShaderUtilDemo ZenCore)

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
