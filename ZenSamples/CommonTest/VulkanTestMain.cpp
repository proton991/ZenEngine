#include "Graphics/RHI/RHIOptions.h"
#include <gtest/gtest.h>

int main(int argc, char** argv)
{
    testing::InitGoogleTest(&argc, argv);
    zen::RHIOptions::GetInstance().SetValidationEnabled(true);
    return RUN_ALL_TESTS();
}
