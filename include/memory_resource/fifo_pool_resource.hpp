#pragma once

#include "memory_resource.hpp"

#include <memory>
#include <list>


namespace ba {
namespace memory {

namespace detail {
    struct Deleter
    {
        void operator()(void* p) const
        {
            upstreamResource->deallocate(p, bytes, alignof(std::max_align_t));
        }

        std::size_t bytes = 0;
        MemoryResource* upstreamResource = 0;
    };

    class Chunk
    {
    public:
        explicit Chunk(MemoryResource* upstreamResource)
            : storage{ nullptr, {0, upstreamResource} }
        {
        }

        Chunk(const Chunk&) = delete;
        Chunk& operator=(const Chunk&) = delete;

        void* get(std::size_t bytes)
        {
            if (bytes > bytesAvailable())
                upstreamRealloc(bytes);

            return storage.get();
        }

    private:
        std::size_t bytesAvailable() const noexcept
        {
            return storage.get_deleter().bytes;
        }
        
        void upstreamRealloc(std::size_t bytes)
        {
            Deleter& deleter = storage.get_deleter();
            void* p = deleter.upstreamResource->allocate(bytes, alignof(std::max_align_t));

            storage.reset(p);
            deleter.bytes = bytes;
        }

        std::unique_ptr<void, Deleter> storage;
    };
} // namespace detail

class FifoPoolResourse
    : public MemoryResource
{
public:
    FifoPoolResourse() noexcept = default;
    FifoPoolResourse(const FifoPoolResourse&) = delete;
    FifoPoolResourse& operator=(const FifoPoolResourse&) = delete;
    ~FifoPoolResourse() noexcept override = default;

    FifoPoolResourse(MemoryResource* upstream) noexcept
        : m_upstream{ upstream }
    {
    }

    void release() noexcept
    {
        list.clear();
        firstFree = list.end();
    }

    MemoryResource* upstream_resource() const noexcept
    {
        return m_upstream;
    }

private:
    void* do_allocate(std::size_t bytes, std::size_t /*alignment*/) override
    {
        if (list.end() == firstFree)
            firstFree = list.emplace(list.end(), m_upstream);

        void* p;

        if (bytes > m_maxSize)
            p = m_upstream->allocate(bytes, alignof(std::max_align_t));
        else
            p = firstFree->get(bytes);

        ++firstFree;

        return p;
    }

    void do_deallocate(void* p, std::size_t bytes, std::size_t /*alignment*/) override
    {
        assert(!list.empty());
        assert(list.begin() != firstFree);

        list.splice(firstFree, list, list.begin());
        --firstFree;

        if (bytes > m_maxSize)
            return m_upstream->deallocate(p, bytes, alignof(std::max_align_t));

    }

    bool do_is_equal(const MemoryResource& other) const noexcept override
    {
        return this == &other;
    }

    std::list<detail::Chunk> list;
    std::list<detail::Chunk>::iterator firstFree = list.end();
    const std::size_t m_maxSize = 256;
    MemoryResource* m_upstream = getDefaultResource();
};

} // namespace memory
} // namespace ba
