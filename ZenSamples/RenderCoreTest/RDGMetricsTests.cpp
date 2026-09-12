#include "Graphics/RenderCore/V2/RenderGraph/RDGMetrics.h"
#include <gtest/gtest.h>
#include <numeric>
#include <random>

using namespace zen;
using namespace zen::rc;

TEST(MetricsLogger, CadenceTimeGateForcedCaptureAndDisable)
{
    MetricsLogger<int> logger;
    logger.Configure({true, 3, std::chrono::milliseconds(100)});
    const MetricsLogger<int>::Clock::time_point start{};
    EXPECT_TRUE(logger.TryBeginSample(start));
    EXPECT_FALSE(logger.TryBeginSample(start + std::chrono::milliseconds(200)));
    EXPECT_FALSE(logger.TryBeginSample(start + std::chrono::milliseconds(200)));
    EXPECT_TRUE(logger.TryBeginSample(start + std::chrono::milliseconds(200)));

    for (int i = 0; i < 3; ++i)
    {
        EXPECT_FALSE(logger.TryBeginSample(start + std::chrono::milliseconds(201)));
    }

    logger.RequestSample();

    EXPECT_TRUE(logger.TryBeginSample(start + std::chrono::milliseconds(201)));

    int delivered = 0;
    logger.SetSink([&](const int& sample) { delivered = sample; });
    logger.Publish(42);

    EXPECT_EQ(delivered, 42);

    logger.Configure({false, 0, std::chrono::milliseconds(-10)});
    logger.RequestSample();

    EXPECT_FALSE(logger.TryBeginSample(start));

    logger.Publish(99);

    EXPECT_EQ(delivered, 42);
    EXPECT_EQ(logger.GetOptions().sampleEvery, 1u);
    EXPECT_EQ(logger.GetOptions().minInterval.count(), 0);
}

namespace
{
constexpr int64_t transfer      = int64_t(RHIPipelineStageFlagBits::eTransfer);
constexpr int64_t compute       = int64_t(RHIPipelineStageFlagBits::eComputeShader);
constexpr int64_t fragment      = int64_t(RHIPipelineStageFlagBits::eFragmentShader);
constexpr int64_t transferWrite = int64_t(RHIAccessFlagBits::eTransferWrite);
constexpr int64_t shaderRead    = int64_t(RHIAccessFlagBits::eShaderRead);

struct BarrierCheck
{
    RDGBarrierValidator validator;
    std::vector<RDGMetricIssue> issues;
    RDGBarrierValidator::Reporter report = [&](RDGMetricIssue issue, int32_t, uint64_t) {
        issues.push_back(issue);
    };

    RDGMetricAccess Writer(bool texture = false)
    {
        RDGMetricAccess writer{};
        writer.resource = 1;
        writer.mode     = RHIAccessMode::eReadWrite;
        writer.stages   = transfer;
        writer.access   = transferWrite;
        writer.texture  = texture;
        writer.layout   = RHITextureLayout::eTransferDst;
        writer.range    = RHITextureSubResourceRange::Color(0, 0, 4, 1);
        validator.Seed(writer);

        return writer;
    }

    void Check(const RDGMetricAccess& access, std::span<const RDGMetricBarrier> barriers)
    {
        issues.clear();
        validator.Check(0, access, barriers, report);
    }

    bool Has(RDGMetricIssue issue) const
    {
        return std::find(issues.begin(), issues.end(), issue) != issues.end();
    }
};
} // namespace

TEST(RDGBarrierValidator, DetectsMissingStagesAndAccessWithoutRejectingValidBarrier)
{
    BarrierCheck check;
    RDGMetricAccess reader = check.Writer();
    reader.mode            = RHIAccessMode::eRead;
    reader.stages          = compute;
    reader.access          = shaderRead;
    check.Check(reader, {});

    EXPECT_TRUE(check.Has(RDGMetricIssue::eMissingBarrier));

    RDGMetricBarrier barrier{1, transfer, fragment, transferWrite, shaderRead};
    check.Writer();
    check.Check(reader, {&barrier, 1});

    EXPECT_TRUE(check.Has(RDGMetricIssue::eStageCoverage));

    barrier.dstStages = compute;
    barrier.srcAccess = 0;
    check.Writer();
    check.Check(reader, {&barrier, 1});

    EXPECT_TRUE(check.Has(RDGMetricIssue::eAccessCoverage));

    barrier.srcAccess = transferWrite;
    check.Writer();
    check.Check(reader, {&barrier, 1});

    EXPECT_TRUE(check.issues.empty());
}

TEST(RDGBarrierValidator, RemembersWriterVisibilityAcrossDifferentReaderStages)
{
    BarrierCheck check;
    RDGMetricAccess reader = check.Writer();
    reader.mode            = RHIAccessMode::eRead;
    reader.stages          = compute;
    reader.access          = shaderRead;
    RDGMetricBarrier barrier{1, transfer, compute, transferWrite, shaderRead};
    check.Check(reader, {&barrier, 1});

    EXPECT_TRUE(check.issues.empty());

    check.Check(reader, {});

    EXPECT_TRUE(check.issues.empty());

    reader.stages = fragment;
    check.Check(reader, {});

    EXPECT_TRUE(check.Has(RDGMetricIssue::eMissingBarrier));

    check.Writer();
    barrier.dstStages = compute | fragment;
    reader.stages     = compute;
    check.Check(reader, {&barrier, 1});
    reader.stages = fragment;
    check.Check(reader, {});

    EXPECT_TRUE(check.issues.empty());
}

TEST(RDGBarrierValidator, ChecksTextureLayoutsRangesAndConservativeExpansion)
{
    BarrierCheck check;
    RDGMetricAccess reader = check.Writer(true);
    reader.mode            = RHIAccessMode::eRead;
    reader.stages          = compute;
    reader.access          = shaderRead;
    reader.layout          = RHITextureLayout::eShaderReadOnly;
    RDGMetricBarrier barrier{1, transfer, compute, transferWrite, shaderRead};
    barrier.oldLayout = RHITextureLayout::eGeneral;
    barrier.newLayout = reader.layout;
    barrier.range     = reader.range;
    check.Check(reader, {&barrier, 1});

    EXPECT_TRUE(check.Has(RDGMetricIssue::eLayoutMismatch));

    check.Writer(true);
    barrier.oldLayout        = RHITextureLayout::eTransferDst;
    barrier.range.levelCount = 1;
    check.Check(reader, {&barrier, 1});

    EXPECT_TRUE(check.Has(RDGMetricIssue::eRangeCoverage));

    check.Writer(true);
    barrier.range.levelCount = 4;
    reader.range.levelCount  = 1;
    check.Check(reader, {&barrier, 1});

    EXPECT_EQ(check.issues, std::vector<RDGMetricIssue>{RDGMetricIssue::eBroadTextureRange});
}

TEST(RDGBarrierValidator, HandlesUsageExpansionAndBroadMasks)
{
    BarrierCheck check;
    RDGMetricAccess writer = check.Writer();
    writer.access |= int64_t(RHIAccessFlagBits::eShaderWrite);
    writer.stages |= compute;
    check.validator.Seed(writer);
    RDGMetricAccess reader = writer;
    reader.mode            = RHIAccessMode::eRead;
    reader.stages          = compute | int64_t(RHIPipelineStageFlagBits::eDrawIndirect);
    reader.access          = shaderRead | int64_t(RHIAccessFlagBits::eIndirectCommandRead);
    std::array<RDGMetricBarrier, 4> barriers;

    for (size_t i = 0; i < barriers.size(); ++i)
    {
        barriers[i] = {1, transfer | compute, reader.stages,
                       i < 2 ? transferWrite : int64_t(RHIAccessFlagBits::eShaderWrite),
                       i % 2 ? shaderRead : int64_t(RHIAccessFlagBits::eIndirectCommandRead)};
    }

    check.Check(reader, barriers);

    EXPECT_TRUE(check.issues.empty());

    check.Check(reader, {});

    EXPECT_TRUE(check.issues.empty());

    check.validator.Seed(writer);
    check.Check(reader, std::span(barriers).first(3));

    EXPECT_TRUE(check.Has(RDGMetricIssue::eAccessCoverage));

    check.validator.Seed(writer);
    RDGMetricBarrier broad{1, int64_t(RHIPipelineStageFlagBits::eAllCommands),
                           int64_t(RHIPipelineStageFlagBits::eAllCommands),
                           int64_t(RHIAccessFlagBits::eMemoryWrite),
                           int64_t(RHIAccessFlagBits::eMemoryRead)};
    check.Check(reader, {&broad, 1});

    EXPECT_TRUE(check.issues.empty());
}

TEST(RDGBarrierValidator, DistinguishesUnknownImportsAndUnproducedTransients)
{
    BarrierCheck check;
    RDGMetricAccess reader{};
    reader.resource = 1;
    reader.mode     = RHIAccessMode::eRead;
    reader.stages   = compute;
    reader.access   = shaderRead;
    check.Check(reader, {});

    EXPECT_TRUE(check.Has(RDGMetricIssue::eUnknownImportState));

    check.validator.Reset();
    reader.imported = false;
    check.Check(reader, {});

    EXPECT_TRUE(check.Has(RDGMetricIssue::eReadBeforeWrite));
    EXPECT_FALSE(check.Has(RDGMetricIssue::eUnknownImportState));

    check.validator.Reset();
    check.validator.Seed(reader);
    RDGMetricBarrier unnecessary{1, compute, compute, shaderRead, shaderRead};
    reader.imported = true;
    check.Check(reader, {&unnecessary, 1});

    EXPECT_TRUE(check.Has(RDGMetricIssue::eRedundantBarrier));
}

TEST(RDGBarrierValidator, HostWrittenImportsStillRequireGPUBarriers)
{
    BarrierCheck check;
    RDGMetricAccess reader{};
    reader.resource    = 1;
    reader.mode        = RHIAccessMode::eRead;
    reader.stages      = transfer;
    reader.access      = int64_t(RHIAccessFlagBits::eTransferRead);
    reader.hostWritten = true;
    check.Check(reader, {});

    EXPECT_TRUE(check.issues.empty());

    // Host initialization must not hide a missing dependency from a later GPU writer.
    check.Writer();
    check.Check(reader, {});

    EXPECT_TRUE(check.Has(RDGMetricIssue::eMissingBarrier));

    // The declaration applies only to imported buffers, never textures or transients.
    check.validator.Reset();
    reader.imported = false;
    check.Check(reader, {});

    EXPECT_TRUE(check.Has(RDGMetricIssue::eReadBeforeWrite));

    check.validator.Reset();
    reader.imported = true;
    reader.texture  = true;
    check.Check(reader, {});

    EXPECT_TRUE(check.Has(RDGMetricIssue::eUnknownImportState));
}

TEST(RDGBarrierValidator, ValidatesOrderingAndScheduleMembership)
{
    const std::array<RDGMetricOrderAccess, 3> accesses{
        {{0, 1, true, 1}, {1, 1, false, 1}, {2, 1, true, 2}}};
    std::vector<RDGMetricIssue> issues;
    const std::function<void(RDGMetricIssue, int32_t, int32_t, uint64_t)> report =
        [&](RDGMetricIssue issue, int32_t, int32_t, uint64_t) {
            issues.push_back(issue);
        };
    const std::array<int32_t, 3> valid{0, 1, 2};
    RDGBarrierValidator::CheckOrder(3, valid, accesses, report);

    EXPECT_TRUE(issues.empty());

    const std::array<int32_t, 3> wrong{1, 0, 2};
    RDGBarrierValidator::CheckOrder(3, wrong, accesses, report);

    EXPECT_NE(std::find(issues.begin(), issues.end(), RDGMetricIssue::eOrdering), issues.end());

    issues.clear();
    const std::array<int32_t, 4> corrupt{0, 0, 3, -1};
    RDGBarrierValidator::CheckOrder(3, corrupt, accesses, report);

    EXPECT_NE(std::find(issues.begin(), issues.end(), RDGMetricIssue::eInvalidSchedule),
              issues.end());
}

TEST(RDGBarrierValidator, KeepsVisibilityAcrossGraphsWithoutReusingNodeIds)
{
    BarrierCheck check;
    check.validator.BeginGraph();
    RDGMetricAccess reader = check.Writer();
    reader.mode            = RHIAccessMode::eRead;
    reader.stages          = compute;
    reader.access          = shaderRead;
    RDGMetricBarrier barrier{1, transfer, compute, transferWrite, shaderRead};
    check.Check(reader, {&barrier, 1});

    ASSERT_TRUE(check.issues.empty());

    check.validator.BeginGraph();
    check.Check(reader, {});

    EXPECT_TRUE(check.issues.empty());

    reader.stages    = fragment;
    int32_t previous = 42;
    check.validator.Check(0, reader, {}, [&](RDGMetricIssue issue, int32_t node, uint64_t) {
        EXPECT_EQ(issue, RDGMetricIssue::eMissingBarrier);
        previous = node;
    });

    EXPECT_EQ(previous, -1); // A previous graph's node 0 is not this graph's node 0.

    check.validator.Forget(reader.resource);

    EXPECT_FALSE(check.validator.HasState(reader.resource));

    check.Check(reader, {});

    EXPECT_TRUE(check.Has(RDGMetricIssue::eUnknownImportState));
}

TEST(RDGBarrierValidator, SeparatesExecutionOrderFromMemoryStageCoverage)
{
    BarrierCheck check;
    RDGMetricAccess reader{};
    reader.resource = 1;
    reader.mode     = RHIAccessMode::eRead;
    reader.stages   = int64_t(RHIPipelineStageFlagBits::eVertexInput);
    reader.access   = int64_t(RHIAccessFlagBits::eVertexAttributeRead);
    check.validator.Seed(reader);
    RDGMetricAccess writer = reader;
    writer.mode            = RHIAccessMode::eReadWrite;
    writer.stages          = compute;
    writer.access          = int64_t(RHIAccessFlagBits::eShaderWrite);
    // The fragment stage includes earlier vertex input in its execution scope: WAR needs no
    // memory availability or visibility operation.
    RDGMetricBarrier execution{1, fragment, compute, 0, 0};
    check.Check(writer, {&execution, 1});

    EXPECT_TRUE(check.issues.empty());

    check.Writer();
    // Destination vertex input orders fragment execution, but does not make shader reads
    // visible at fragment. Logical stage expansion must never widen a memory access scope.
    reader.stages = fragment;
    reader.access = shaderRead;
    RDGMetricBarrier wrongMemoryScope{1, transfer, int64_t(RHIPipelineStageFlagBits::eVertexInput),
                                      transferWrite, shaderRead};
    check.Check(reader, {&wrongMemoryScope, 1});

    EXPECT_FALSE(check.Has(RDGMetricIssue::eStageCoverage));
    EXPECT_TRUE(check.Has(RDGMetricIssue::eAccessCoverage));
}

TEST(RDGBarrierValidator, ChecksMergedIndirectAndShaderReadsAtTheirOwnStages)
{
    BarrierCheck check;
    RDGMetricAccess reader     = check.Writer();
    reader.mode                = RHIAccessMode::eRead;
    const int64_t indirect     = int64_t(RHIPipelineStageFlagBits::eDrawIndirect);
    const int64_t indirectRead = int64_t(RHIAccessFlagBits::eIndirectCommandRead);
    reader.stages              = compute | indirect;
    reader.access              = shaderRead | indirectRead;
    const std::array<RDGMetricBarrier, 2> barriers{{
        {1, transfer, indirect, transferWrite, indirectRead},
        {1, transfer, compute, transferWrite, shaderRead},
    }};
    check.Check(reader, barriers);

    EXPECT_TRUE(check.issues.empty());

    check.Check(reader, {});

    EXPECT_TRUE(check.issues.empty());

    check.Writer();
    check.Check(reader, std::span(barriers).first(1));

    EXPECT_TRUE(check.Has(RDGMetricIssue::eAccessCoverage));
}

TEST(RDGBarrierValidator, CombinesSeparateGraphicsAndComputeDestinationBarriers)
{
    BarrierCheck check;
    RDGMetricAccess reader = check.Writer();
    reader.mode            = RHIAccessMode::eRead;
    reader.stages          = compute | fragment;
    reader.access          = shaderRead;
    const std::array<RDGMetricBarrier, 2> barriers{{
        {1, transfer, fragment, transferWrite, shaderRead},
        {1, transfer, compute, transferWrite, shaderRead},
    }};
    check.Check(reader, barriers);
    EXPECT_TRUE(check.issues.empty());
    check.Check(reader, {});
    EXPECT_TRUE(check.issues.empty());
}

TEST(RDGBarrierValidator, VersionOrderingMatchesIndependentMemoryOracleWithRenumberedNodes)
{
    std::mt19937 random(0x3a2026);
    constexpr uint32_t nodes = 8;

    for (uint32_t trial = 0; trial < 256; ++trial)
    {
        std::array<int32_t, nodes> identities;
        std::iota(identities.begin(), identities.end(), 0);
        std::shuffle(identities.begin(), identities.end(), random);
        std::vector<RDGMetricOrderAccess> declarations;
        std::array<int32_t, 3> latest{};

        for (const int node : identities)
        {
            for (uint64_t resource = 0; resource < latest.size(); ++resource)
            {
                if (random() % 3 == 0)
                {
                    continue;
                }

                const bool writes = random() % 4 == 0;

                if (writes)
                {
                    ++latest[resource];
                }

                declarations.push_back({node, resource, writes, latest[resource]});
            }
        }

        std::array<int32_t, nodes> order = identities;

        for (uint32_t permutation = 0; permutation < 32; ++permutation)
        {
            if (permutation != 0)
            {
                std::shuffle(order.begin(), order.end(), random);
            }

            // Independent oracle: execute against one current value per allocation. Writes
            // must replace their predecessor; reads must observe exactly the requested value.
            std::array<int32_t, 3> memory{};
            bool expectedValid = true;

            for (const int node : order)
            {
                for (RDGMetricOrderAccess const& access : declarations)
                {
                    if (access.node == node)
                    {
                        expectedValid &=
                            memory[access.resource] == access.version - int32_t(access.writes);

                        if (access.writes)
                        {
                            memory[access.resource] = access.version;
                        }
                    }
                }
            }

            std::shuffle(declarations.begin(), declarations.end(), random);
            bool actualValid = true;
            RDGBarrierValidator::CheckOrder(nodes, order, declarations,
                                            [&](RDGMetricIssue issue, int32_t, int32_t, uint64_t) {
                                                EXPECT_EQ(issue, RDGMetricIssue::eOrdering);
                                                actualValid = false;
                                            });

            ASSERT_EQ(actualValid, expectedValid)
                << "trial=" << trial << " permutation=" << permutation;
        }
    }
}

TEST(RDGBarrierValidator, AllReadersOfOneValueCanReorderButMustPrecedeOverwrite)
{
    const std::array<RDGMetricOrderAccess, 6> declarations{{
        {0, 7, false, 0},
        {1, 7, false, 0},
        {2, 7, false, 0},
        {3, 7, false, 0},
        {4, 7, false, 0},
        {5, 7, true, 1},
    }};
    std::vector<std::pair<int32_t, int32_t>> failures;
    const std::function<void(RDGMetricIssue, int32_t, int32_t, uint64_t)> report =
        [&](RDGMetricIssue issue, int32_t node, int32_t prior, uint64_t resource) {
            EXPECT_EQ(issue, RDGMetricIssue::eOrdering);
            EXPECT_EQ(resource, 7u);
            failures.emplace_back(node, prior);
        };
    const std::array<int32_t, 6> valid{4, 2, 0, 3, 1, 5};
    RDGBarrierValidator::CheckOrder(6, valid, declarations, report);

    EXPECT_TRUE(failures.empty());

    const std::array<int32_t, 6> reversed{5, 4, 3, 2, 1, 0};
    RDGBarrierValidator::CheckOrder(6, reversed, declarations, report);

    EXPECT_EQ(failures,
              (std::vector<std::pair<int32_t, int32_t>>{{5, 0}, {5, 1}, {5, 2}, {5, 3}, {5, 4}}));
}

TEST(RDGBarrierValidator, VersionOrderMatchesIndependentValueConstraints)
{
    std::mt19937 random(0x3b0);
    std::array<RDGMetricOrderAccess, 10> declarations;
    declarations[0] = {0, 1, false, 0};

    for (int32_t i = 1; i < 10; ++i)
    {
        declarations[i] = {i, 1, (i - 1) % 3 == 0, (i - 1) / 3 + 1};
    }

    std::array<int32_t, 10> order;
    std::iota(order.begin(), order.end(), 0);

    for (uint32_t trial = 0; trial < 4096; ++trial)
    {
        if (trial != 0)
        {
            std::shuffle(order.begin(), order.end(), random);
        }

        std::array<uint32_t, 10> positions;

        for (uint32_t i = 0; i < order.size(); ++i)
        {
            positions[order[i]] = i;
        }

        bool expected = true;

        for (const RDGMetricOrderAccess& a : declarations)
        {
            for (const RDGMetricOrderAccess& b : declarations)
            {
                if ((a.version < b.version && b.writes) ||
                    (a.version == b.version && a.writes && !b.writes))
                {
                    expected &= positions[a.node] < positions[b.node];
                }
            }
        }

        // Declaration order is unrelated to explicit version/value order.
        std::shuffle(declarations.begin(), declarations.end(), random);
        bool actual = true;
        RDGBarrierValidator::CheckOrder(
            10, order, declarations,
            [&](RDGMetricIssue, int32_t, int32_t, uint64_t) { actual = false; });

        ASSERT_EQ(actual, expected) << "trial=" << trial;
    }
}

TEST(RDGBarrierValidator, VersionOrderRejectsMissingDuplicateAndUnversionedProducers)
{
    for (uint32_t kind = 0; kind < 3; ++kind)
    {
        std::vector<RDGMetricOrderAccess> accesses{{0, 1, false, 1}};

        if (kind != 0)
        {
            accesses.push_back({1, 1, true, 1});
            accesses.push_back({2, 1, true, kind == 1 ? 1 : -1});
        }

        const std::array<int32_t, 3> order{1, 0, 2};
        std::vector<RDGMetricIssue> issues;
        RDGBarrierValidator::CheckOrder(
            3, order, accesses,
            [&](RDGMetricIssue issue, int32_t, int32_t, uint64_t) { issues.push_back(issue); });

        EXPECT_NE(std::find(issues.begin(), issues.end(),
                            kind == 0 ? RDGMetricIssue::eReadBeforeWrite :
                                        RDGMetricIssue::eInvalidSchedule),
                  issues.end());
    }
}
