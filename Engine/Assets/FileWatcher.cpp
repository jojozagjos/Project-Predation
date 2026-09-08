#include "Engine/Assets/FileWatcher.h"

#include "Engine/Core/Log.h"

#include <algorithm>

namespace pred
{

void FileWatcher::Watch(const std::filesystem::path& file, Callback callback)
{
    Entry entry;
    entry.path = file;
    entry.callback = std::move(callback);
    std::error_code ec;
    entry.existed = std::filesystem::exists(file, ec);
    if (entry.existed)
    {
        entry.lastWrite = std::filesystem::last_write_time(file, ec);
    }
    m_entries.push_back(std::move(entry));
    PRED_LOG_DEBUG(Asset, "Watching {}", file.string());
}

void FileWatcher::Unwatch(const std::filesystem::path& file)
{
    std::erase_if(m_entries, [&file](const Entry& entry) { return entry.path == file; });
}

void FileWatcher::Clear()
{
    m_entries.clear();
}

void FileWatcher::Poll(double nowSeconds)
{
    if (nowSeconds - m_lastPoll < m_interval)
    {
        return;
    }
    m_lastPoll = nowSeconds;

    for (Entry& entry : m_entries)
    {
        std::error_code ec;
        const bool exists = std::filesystem::exists(entry.path, ec);
        if (!exists)
        {
            entry.existed = false;
            continue;
        }
        const auto writeTime = std::filesystem::last_write_time(entry.path, ec);
        if (ec)
        {
            continue;
        }
        if (!entry.existed || writeTime != entry.lastWrite)
        {
            entry.existed = true;
            entry.lastWrite = writeTime;
            PRED_LOG_INFO(Asset, "File changed: {}", entry.path.string());
            if (entry.callback)
            {
                entry.callback(entry.path);
            }
        }
    }
}

} // namespace pred
