add_executable(ZenCoreTest ZenCoreTest/main.cpp)
add_executable(SmartPtrTest SmartPtrTest/SharedPtrTests.cpp SmartPtrTest/UniquePtrTests.cpp)
target_link_libraries(ZenCoreTest ZenCore)
target_link_libraries(SmartPtrTest ZenCore gtest_main)

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

target_link_libraries(HelloTriangle ZenCore)
target_link_libraries(GLTFViewer ZenCore)
