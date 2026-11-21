#include "EditorSession.h"

#include <iostream>

int main()
{
    auto *stdoutBuffer = std::cout.rdbuf(std::cerr.rdbuf());
    std::ostream protocol(stdoutBuffer);
    const auto exitCode = runEditorSession(std::cin, protocol);
    std::cout.rdbuf(stdoutBuffer);
    return exitCode;
}
