#include "Platform/CommandLine.h"
#include "Templates/HeapVector.h"
#include <windows.h>
#include <iostream>
#include <string>

namespace zen::platform
{
int RunUTF8Main(int argc, wchar_t** argv, int (*entry)(int, char**))
{
    HeapVector<std::string> arguments;
    bool                    valid = true;
    for (int index = 0; index < argc && valid; ++index)
    {
        const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argv[index], -1, nullptr, 0, nullptr, nullptr);
        valid          = size > 0;
        if (valid)
        {
            std::string argument(static_cast<size_t>(size), '\0');
            valid = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argv[index], -1, argument.data(), size, nullptr, nullptr)
                 == size;
            argument.resize(static_cast<size_t>(size - 1));
            arguments.push_back(std::move(argument));
        }
    }
    int result = 2;
    if (valid)
    {
        HeapVector<char*> pointers;
        for (std::string& argument : arguments)
        {
            pointers.push_back(argument.data());
        }
        pointers.push_back(nullptr);
        result = entry(argc, pointers.data());
    }
    else
    {
        std::cerr << "Command line contains invalid Unicode\n";
    }
    return result;
}
} // namespace zen::platform
