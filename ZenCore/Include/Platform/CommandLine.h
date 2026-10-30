#pragma once

namespace zen::platform
{
#if defined(_WIN32)
// Windows CRT narrow argv uses the active code page. Runtime setup paths and
// native process arguments use UTF-8, matching the engine's filesystem APIs.
int RunUTF8Main(int argc, wchar_t** argv, int (*entry)(int, char**));
#endif
} // namespace zen::platform
