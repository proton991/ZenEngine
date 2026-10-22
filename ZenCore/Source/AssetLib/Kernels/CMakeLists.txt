# Asset kernels hash, validate and decode gigabytes of source and cache data per
# import. Debug builds keep symbols and the debug runtime but compile these few
# loops optimized, so loading large scenes stays usable while debugging.
foreach (language C CXX)
    if (MSVC)
        string(REGEX REPLACE "/O[d1-2x]|/Ob[0-3]|/RTC[1csu]+" "" CMAKE_${language}_FLAGS_DEBUG "${CMAKE_${language}_FLAGS_DEBUG}")
        string(APPEND CMAKE_${language}_FLAGS_DEBUG " /O2 /Ob2")
    else ()
        string(REPLACE "-O0" "" CMAKE_${language}_FLAGS_DEBUG "${CMAKE_${language}_FLAGS_DEBUG}")
        string(APPEND CMAKE_${language}_FLAGS_DEBUG " -O2")
    endif ()
endforeach ()

add_library(zen_asset_kernels STATIC AssetKernels.cpp AssetKernels.h StbImage.c)
target_compile_features(zen_asset_kernels PRIVATE cxx_std_20)
target_link_libraries(zen_asset_kernels PRIVATE stb)
set_target_properties(zen_asset_kernels PROPERTIES POSITION_INDEPENDENT_CODE ON)
