#pragma once

#include <string>

namespace crash {

// On a crash or an uncaught exception, writes a minidump into `directory` and names it on
// stderr. Windows only; elsewhere it does nothing.
void install(const std::string& directory);

}  // namespace crash
