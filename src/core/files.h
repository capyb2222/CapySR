#pragma once

#include <string>

namespace files {

// Queues `data` for a background thread that writes it to `path` through util::writeFile.
// A newer write to the same path replaces one still waiting, so a burst costs one write.
void writeLater(std::string path, std::string data);

// Blocks until every queued write has landed. Anything that reads a file back first calls
// this, so it never sees an older copy than the one last queued.
void flush();

}  // namespace files
