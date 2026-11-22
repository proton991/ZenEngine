#include "Editor/Services/AssetPreprocessor.h"
#include "ZenAsset/Reader/Assembly.h"
#include <gtest/gtest.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

namespace zen::editor
{
namespace
{
std::string Text(const std::filesystem::path& path)
{
    const std::u8string text = path.generic_u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

void Finish(AssetPreprocessor& build)
{
    const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (build.IsRunning() && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        build.Poll();
    }
    EXPECT_FALSE(build.IsRunning());
}
} // namespace

TEST(AssetPreprocessor, SingleAndMultipleSourcesBuildNativeAssemblyOutsideEditor)
{
    std::error_code             error;
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path(error)
        / ("ZenPreprocess test " + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    ASSERT_FALSE(error);
    ASSERT_TRUE(std::filesystem::create_directory(directory, error));
    const std::filesystem::path source = directory / "one model.obj";
    const std::filesystem::path second = directory / "second model.obj";
    std::ofstream(source) << "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";
    std::ofstream(second) << "v 0 0 0\nv 2 0 0\nv 0 2 0\nf 1 2 3\n";
    AssetPreprocessor build;
    build.outputDirectory = Text(directory / "cooked output");
    ASSERT_TRUE(build.AddSource(Text(source)));
    EXPECT_FALSE(build.AddSource(Text(source)));
    EXPECT_FALSE(build.AddSource(Text(directory / "missing.fbx")));
    ASSERT_TRUE(build.Start()) << build.GetStatusText();
    EXPECT_FLOAT_EQ(build.GetProgress(), 0.0f);
    EXPECT_FALSE(build.Start());
    EXPECT_TRUE(build.IsRunning());
    Finish(build);
    ASSERT_TRUE(build.Succeeded()) << build.GetLog();
    EXPECT_FLOAT_EQ(build.GetProgress(), 1.0f);
    EXPECT_EQ(build.GetProgressText(), "Complete");
    EXPECT_NE(build.GetLog().find("zenasset-progress-v1 0 importing"), std::string::npos);
    EXPECT_NE(build.GetLog().find("zenasset-progress-v1 4294967295 publishing"), std::string::npos);
    zenasset::AssemblyReadResult single = zenasset::ReadAssembly(build.GetOutput());
    ASSERT_TRUE(single.status.Succeeded()) << single.status.message;
    EXPECT_EQ(single.assembly->models.size(), 1);
    EXPECT_TRUE(single.assembly->instances[0].submitLights);
    ASSERT_TRUE(build.AddSource(Text(second)));
    ASSERT_TRUE(build.Start());
    Finish(build);
    ASSERT_TRUE(build.Succeeded()) << build.GetLog();
    zenasset::AssemblyReadResult both = zenasset::ReadAssembly(build.GetOutput());
    ASSERT_TRUE(both.status.Succeeded()) << both.status.message;
    EXPECT_EQ(both.assembly->models.size(), 2);
    EXPECT_EQ(both.assembly->counts.triangles, 2);
    EXPECT_EQ(both.assembly->models[0].identity, single.assembly->models[0].identity);
    EXPECT_EQ(zenasset::VerifyAssemblyDependencies(build.GetOutput(), *both.assembly).code, zenasset::StatusCode::Success);
    {
        std::ofstream large(second, std::ios::trunc);
        large << "v 0 0 0\nv 2 0 0\nv 0 2 0\n";
        for (uint32_t face = 0; face < 200000; ++face)
        {
            large << "f 1 2 3\n";
        }
    }
    ASSERT_TRUE(build.Start());
    build.Cancel();
    EXPECT_FALSE(build.IsRunning());
    EXPECT_FALSE(build.Succeeded());
    EXPECT_LT(build.GetProgress(), 1.0f);
    EXPECT_EQ(build.GetProgressText(), "Cancelled");
    zenasset::AssemblyReadResult cancelled = zenasset::ReadAssembly(build.GetOutput());
    ASSERT_TRUE(cancelled.status.Succeeded());
    EXPECT_EQ(*cancelled.assembly, *both.assembly);
    std::ofstream(second, std::ios::trunc) << "invalid obj";
    ASSERT_TRUE(build.Start());
    Finish(build);
    EXPECT_FALSE(build.Succeeded());
    zenasset::AssemblyReadResult retained = zenasset::ReadAssembly(build.GetOutput());
    ASSERT_TRUE(retained.status.Succeeded());
    EXPECT_EQ(*retained.assembly, *both.assembly);
    single.assembly.reset();
    both.assembly.reset();
    cancelled.assembly.reset();
    retained.assembly.reset();
    std::filesystem::remove_all(directory, error);
    EXPECT_FALSE(error);
}

TEST(AssetPreprocessor, RejectsInvalidRequestBeforeStarting)
{
    AssetPreprocessor build;
    EXPECT_FALSE(build.Start());
    EXPECT_FALSE(build.IsRunning());
    EXPECT_FALSE(build.AddSource(std::string("bad\0path.obj", 12)));
    build.Cancel();
    EXPECT_FALSE(build.Succeeded());
}
} // namespace zen::editor
