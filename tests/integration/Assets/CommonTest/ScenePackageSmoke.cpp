#include "AssetLib/SceneLoader.h"
#include <cstdio>

int main(int argc, char** argv)
{
    int result = 2;
    if (argc != 2)
    {
        std::fprintf(stderr, "Usage: ScenePackageSmoke <prepared.zenasset>\n");
    }
    else
    {
        const zen::asset::SceneLoadResult loaded = zen::asset::LoadScene(argv[1]);
        if (loaded.scene && loaded.asset)
        {
            std::printf("Loaded %s: %zu nodes, %zu meshes, %zu vertices, %zu textures\n", argv[1], loaded.asset->nodes.size(),
                        loaded.asset->meshes.size(), loaded.scene->vertices.size(), loaded.asset->textures.size());
            result = 0;
        }
        else
        {
            std::fprintf(stderr, "%s\n", loaded.error.c_str());
            result = 1;
        }
    }
    return result;
}
