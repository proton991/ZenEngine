#include "Utils/Errors.h"
#include <gtest/gtest.h>

TEST(VerificationTest, SuccessfulChecksEvaluateTheirExpressionOnce)
{
    int evaluations = 0;

    VERIFY_EXPR(++evaluations == 1);

    VERIFY_EXPR_MSG(++evaluations == 2, "unused message");

    VERIFY_EXPR_MSG_F(++evaluations == 3, "unused value {}", evaluations);

    EXPECT_EQ(evaluations, 3);
}

TEST(VerificationTest, SuccessfulChecksDoNotEvaluateDiagnosticArguments)
{
    int evaluations = 0;

    VERIFY_EXPR_MSG(true, (++evaluations, "unused message"));

    VERIFY_EXPR_MSG_F(true, "unused value {}", ++evaluations);

    EXPECT_EQ(evaluations, 0);
}

TEST(VerificationDeathTest, FailedExpressionStopsExecution)
{
    EXPECT_DEATH(VERIFY_EXPR(false), "verification failed: false.*ErrorTests.cpp:");
}

TEST(VerificationDeathTest, FailedMessageCheckReportsItsMessage)
{
    EXPECT_DEATH(VERIFY_EXPR_MSG(false, "invalid work"), "invalid work");
}

TEST(VerificationDeathTest, FailedFormattedCheckReportsItsValues)
{
    EXPECT_DEATH(VERIFY_EXPR_MSG_F(false, "invalid work {}", 42), "invalid work 42");
}

TEST(VerificationDeathTest, FormattedCheckAcceptsNoArguments)
{
    EXPECT_DEATH(VERIFY_EXPR_MSG_F(false, "invalid work"), "invalid work");
}

TEST(VerificationDeathTest, FailureStopsExecutionWithLoggingDisabled)
{
    EXPECT_DEATH(
        {
            spdlog::set_level(spdlog::level::off);

            VERIFY_EXPR(false);
        },
        "verification failed");
}
