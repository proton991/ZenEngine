#include "Graphics/RenderCore/V2/RenderConfig.h"
#include <gtest/gtest.h>

int main(int argc, char** argv)
{
    testing::InitGoogleTest(&argc, argv);
    zen::rc::RenderConfig::GetInstance().rhiDevice.SetValidationMode(zen::RHIValidationMode::eRequired);
    return RUN_ALL_TESTS();
}
