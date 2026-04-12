#include "GLTFViewer.h"


using namespace zen::platform;

int main()
{
    Application* pApp = new GLTFViewer();
    WindowConfig windowConfig;
    windowConfig.resizable = true;
    pApp->Prepare(windowConfig);
    pApp->Run();
}