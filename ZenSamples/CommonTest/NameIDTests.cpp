#include "Templates/NameID.h"
#include <gtest/gtest.h>

using namespace zen;

// These pairs share a 32-bit FNV-1a hash.
TEST(NameIDTest, CollidingNamesKeepDistinctIdsAndStrings)
{
    const NameID costarring("costarring");

    const NameID liquid("liquid");

    const NameID declinate("declinate");

    const NameID macallums("macallums");

    EXPECT_NE(costarring, liquid);

    EXPECT_NE(declinate, macallums);

    EXPECT_STREQ(costarring.CStr(), "costarring");

    EXPECT_STREQ(liquid.CStr(), "liquid");

    EXPECT_STREQ(declinate.CStr(), "declinate");

    EXPECT_STREQ(macallums.CStr(), "macallums");
}

TEST(NameIDTest, CollidingNamesResolveToTheirExistingIds)
{
    const NameID altarage("altarage");

    const NameID zinke("zinke");

    EXPECT_EQ(NameID("zinke"), zinke);

    EXPECT_EQ(NameID("altarage"), altarage);

    EXPECT_EQ(NameID(std::string("zinke")), zinke);

    EXPECT_EQ(std::hash<NameID>()(altarage), std::hash<NameID>()(zinke));
}
