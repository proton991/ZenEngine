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
target_link_libraries(HelloTriangle ZenCore)
