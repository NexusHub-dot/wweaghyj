#pragma once
#include "replay.hpp"
#include <chrono>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif
namespace pulse {
inline void saveRecordingFile(std::filesystem::path const& path,Tape const& tape) {
    validate(tape);
    std::filesystem::create_directories(path.parent_path());
    auto temporary=path;temporary+=".tmp";
    {std::ofstream stream(temporary,std::ios::binary|std::ios::trunc);write(stream,tape);stream.flush();
     if(!stream)throw std::runtime_error("Macro save failed; previous slot is unchanged");}
    auto verified=load(temporary);
    if(verified.steps.size()!=tape.steps.size() || verified.events.size()!=tape.events.size())throw std::runtime_error("Macro verification failed");
    if(std::filesystem::exists(path)){auto backup=path;backup+=".bak";std::filesystem::copy_file(path,backup,std::filesystem::copy_options::overwrite_existing);}
#ifdef _WIN32
    if(!MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("Could not replace macro file; temporary recording retained");
#else
    std::filesystem::rename(temporary,path);
#endif
}
inline std::filesystem::path deletedPath(std::filesystem::path path) { path += ".deleted"; return path; }
inline void archiveRecording(std::filesystem::path const& path) {
    if (!std::filesystem::is_regular_file(path)) throw std::runtime_error("This slot has no saved macro to delete");
    auto deleted = deletedPath(path);
    if (std::filesystem::exists(deleted)) {
        auto older = deleted;
        older += "-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        std::filesystem::rename(deleted, older);
    }
    std::filesystem::rename(path, deleted);
}
inline void restoreRecording(std::filesystem::path const& path) {
    if (std::filesystem::exists(path)) throw std::runtime_error("This slot already has a saved macro; Undo will not overwrite it");
    auto deleted = deletedPath(path);
    if (!std::filesystem::exists(deleted)) throw std::runtime_error("No deleted macro to restore in this slot");
    (void)load(deleted); // Validate before making it the playable slot again.
    std::filesystem::rename(deleted, path);
}
inline double duration(Tape const& tape) { double seconds=0;for(auto const& s:tape.steps)seconds+=s.dt;return seconds; }
inline std::string revision(std::filesystem::path const& path) {
    if(!std::filesystem::exists(path))return {};
    return std::to_string(std::filesystem::last_write_time(path).time_since_epoch().count())+":"+std::to_string(std::filesystem::file_size(path));
}
}
