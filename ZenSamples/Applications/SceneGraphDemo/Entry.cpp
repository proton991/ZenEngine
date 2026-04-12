#include "SceneGraphDemo.h"


using namespace zen::platform;

int main()
{
    Application* pApp = new SceneGraphDemo();
    WindowConfig windowConfig;
    windowConfig.resizable = true;
    pApp->Prepare(windowConfig);
    pApp->Run();
}