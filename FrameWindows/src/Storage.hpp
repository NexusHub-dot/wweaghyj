#pragma once
#include "Core.hpp"
#include <filesystem>
namespace fwl {
std::filesystem::path dataPath(std::string const& fingerprint);
bool save(Run const& run, std::string& error);
std::optional<Run> load(std::string const& fingerprint, std::string& error);
bool exportCSV(Run const& run, std::string& error);
}
