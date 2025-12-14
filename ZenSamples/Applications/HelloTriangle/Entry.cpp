#include "HelloTriangle.h"



int main()
{
    Application* app = new HelloTriangle();
    platform::WindowConfig windowConfig;
    windowConfig.resizable = true;
    app->Prepare(windowConfig);
    app->Run();
}