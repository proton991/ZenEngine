#include "SceneGraphDemo.h"

using namespace zen;
using namespace zen::platform;

int main()
{
    Application* app = new SceneGraphDemo();
    WindowConfig windowConfig;
    windowConfig.resizable = true;
    app->Prepare(windowConfig);
    app->Run();
}