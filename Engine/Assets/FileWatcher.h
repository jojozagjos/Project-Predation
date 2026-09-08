#pragma once

#include <filesystem>
#include <functional>
#include <vector>

namespace pred
{

// Polling file watcher for hot reload of config, bindings, and later shaders
// and data files. Polling is deliberate: it is portable, cheap at the scale
// of a few dozen files, and never misses a change.
class FileWatcher
{
public:
    using Callback = std::function<void(const std::filesystem::path&)>;

    void Watch(const std::filesystem::path& file, Callback callback);
    void Unwatch(const std::filesystem::path& file);
    void Clear();

    // Checks watched files at most once per interval; invokes callbacks for changed files.
    void Poll(double nowSeconds);
    void SetInterval(double seconds) { m_interval = seconds; }
    size_t Count() const { return m_entries.size(); }

private:
    struct Entry
    {
        std::filesystem::path path;
        std::filesystem::file_time_type lastWrite;
        bool existed = false;
        Callback callback;
    };

    std::vector<Entry> m_entries;
    double m_lastPoll = -1.0e9;
    double m_interval = 0.5;
};

} // namespace pred
