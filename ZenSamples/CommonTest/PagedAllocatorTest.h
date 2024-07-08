#pragma once

#include <gtest/gtest.h>
#include "Common/PagedAllocator.h"

// Dummy class for testing
class DummyClass
{
public:
    DummyClass() : data(0) {}
    explicit DummyClass(int d) : data(d) {}
    int getData() const
    {
        return data;
    }

private:
    int data;
};

// Fixture for PagedAllocator tests
class PagedAllocatorTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        allocator.Init();
    }

    zen::PagedAllocator<DummyClass> allocator{4000, false};
};
