add_executable(ZenCoreTest ZenCoreTest/main.cpp)
add_executable(SmartPtrTest SmartPtrTest/main.cpp)
target_link_libraries(ZenCoreTest ZenCore)
target_link_libraries(SmartPtrTest ZenCore gtest_main)