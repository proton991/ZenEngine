#include "Common/SpinLock.h"
#include <gtest/gtest.h>
#include <thread>

using namespace zen;

SpinLock spinLock;

int sharedCounter = 0;

void IncrementCounter(int increments)
{
    for (int i = 0; i < increments; ++i)
    {
        spinLock.Lock();
        ++sharedCounter;
        spinLock.Unlock();
    }
}

TEST(spin_lock_test, basic)
{

    const int numThreads          = 10;
    const int incrementsPerThread = 1000;

    std::vector<std::thread> threads;

    // Start multiple threads
    threads.reserve(numThreads);
    for (int i = 0; i < numThreads; ++i)
    {
        threads.emplace_back(IncrementCounter, incrementsPerThread);
    }

    // Join all threads
    for (auto& t : threads)
    {
        t.join();
    }

    EXPECT_EQ(sharedCounter, numThreads * incrementsPerThread);
}