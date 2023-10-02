#include "HelloTriangle.h"

using namespace zen;

int main()
{
    Application*           app = new HelloTriangle();
    platform::WindowConfig windowConfig;
    app->Prepare(windowConfig);
    app->Run();
}