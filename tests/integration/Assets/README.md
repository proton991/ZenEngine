# Asset integration tests

ZenEngine owns the tests that open processor output through its reader, renderer and editor. Configure the engine with BUILD_TESTING, ZEN_BUILD_ASSET_TOOLS and ZEN_BUILD_ASSET_INTEGRATION_TESTS enabled. Set ZEN_ASSET_PROCESSOR_SOURCE_DIR to the processor checkout when testing local changes together.

ZenAssetEngineTests covers source import, package loading, cache behavior and scene assembly. ZenAssetTextureIntegrationTest runs texture sampling on Vulkan. EditorAssetRenderingTest opens and renders cooked scenes through editor services. Run the configured suites with `ctest --test-dir <engine-build> -L asset --output-on-failure`.

ZEN_ASSET_TEST_CORPUS defaults to the processor checkout’s tests/fixtures directory. The processor owns this shared semantic corpus and its licenses. Editor/Fixtures and Editor/Shaders belong to the renderer comparisons in this directory.
