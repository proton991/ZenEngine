#include "GLTFViewer.h"

using namespace zen;
using namespace zen::platform;

int main()
{
    Application* app = new GLTFViewer();
    WindowConfig windowConfig;
    windowConfig.resizable = true;
    app->Prepare(windowConfig);
    app->Run();
}