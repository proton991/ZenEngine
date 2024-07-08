#include "Common/ThreadPool.h"
#include "Common/Errors.h"
#include <string>
using namespace std;
using namespace zen;

void foo(int threadId)
{
    LOGI("foo()! Thread ID: " + std::to_string(threadId));
}
void foo(const string& threadId)
{
    LOGI("foo()! Thread ID: " + threadId);
}
#define POOL_SIZE 5

int main(int argc, char** argv)
{
    std::atomic<int> value = 0;
    ThreadPool<void, int> threadPool;
    threadPool.Resize(POOL_SIZE);
    // enqueue and store future
    std::vector<std::future<void>> futures;
    for (int i = 0; i < POOL_SIZE; i++)
    {
        auto fut = threadPool.Push([&value](int threadId) {
            foo(threadId);
            value++;
        });
        futures.push_back(std::move(fut));
    }
    for (auto& fut : futures)
    {
        fut.get();
    }

    LOGI("value is now equal to {}", value)
    LOGI("futures' size: {}", futures.size())
}