#pragma once

#include <algorithm>
#include <cstddef>
#include <thread>
#include <vector>

namespace pred
{

// Runs `work(begin, end)` over [0, count) split across the machine's cores, and returns when all of it
// is done. For loops whose iterations share nothing but what they read -- sampling a shape on a grid,
// painting every vertex of a mesh -- and are long enough to be worth a thread each: below `grain`
// iterations a piece, it simply runs on the caller.
template <typename Work>
void ParallelFor(size_t count, Work&& work, size_t grain = 256)
{
    const size_t cores = std::max<size_t>(std::thread::hardware_concurrency(), 1);
    const size_t pieces = std::min(cores, std::max<size_t>(count / std::max<size_t>(grain, 1), 1));
    if (pieces <= 1)
    {
        work(size_t{0}, count);
        return;
    }
    std::vector<std::thread> threads;
    threads.reserve(pieces - 1);
    const size_t each = (count + pieces - 1) / pieces;
    for (size_t p = 1; p < pieces; ++p)
    {
        const size_t begin = p * each;
        const size_t end = std::min(count, begin + each);
        if (begin < end)
        {
            threads.emplace_back([&work, begin, end] { work(begin, end); });
        }
    }
    work(size_t{0}, std::min(count, each));
    for (std::thread& thread : threads)
    {
        thread.join();
    }
}

} // namespace pred
