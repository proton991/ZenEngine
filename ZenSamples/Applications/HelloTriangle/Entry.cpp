#include "HelloTriangle.h"



int main()
{
    Application* pApp = new HelloTriangle();
    platform::WindowConfig windowConfig;
    windowConfig.resizable = true;
    pApp->Prepare(windowConfig);
    pApp->Run();
}