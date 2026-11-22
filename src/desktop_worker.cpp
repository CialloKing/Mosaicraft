#include "core/EditorSession.h"
#include "core/RuntimeContext.h"

#include <iostream>

int main()
{
    auto *stdoutBuffer = std::cout.rdbuf(std::cerr.rdbuf());
    std::ostream protocol(stdoutBuffer);
    const auto exitCode = mosaicraft::runEditorSession(std::cin, protocol, mosaicraft::desktopTaskCancellationFile());
    std::cout.rdbuf(stdoutBuffer);
    return exitCode;
}
