#pragma once

#ifdef __cpp_lib_memory_resource
#   include <memory_resource>
#else
#   include <cstddef>
#endif
#include <memory>

#include <boost/core/null_deleter.hpp>


namespace ba {
namespace memory {

#ifdef __cpp_lib_memory_resource

using MemoryResource = std::pmr::memory_resource;

MemoryResource* getDefaultResource() noexcept
{
    return std::pmr::get_default_resource();
}

using Byte = std::byte;

template <typename T = std::byte>
using PolimorphicAllocator = std::pmr::polymorphic_allocator<T>;

#else // __cpp_lib_memory_resource

class MemoryResource
{
public:
    virtual ~MemoryResource() noexcept = default;
    void* allocate(std::size_t bytes, std::size_t alignment = alignof(std::max_align_t))
    {
        return do_allocate(bytes, alignment);
    }

    void deallocate(void* p, std::size_t bytes, std::size_t alignment = alignof(std::max_align_t))
    {
        do_deallocate(p, bytes, alignment);
    }

    bool is_equal(const MemoryResource& other) const noexcept
    {
        return do_is_equal(other);
    }

private:
    virtual void* do_allocate(std::size_t bytes, std::size_t alignment) = 0;
    virtual void do_deallocate(void* p, std::size_t bytes, std::size_t alignment) = 0;
    virtual bool do_is_equal(const MemoryResource& other) const noexcept = 0;
};

bool operator==(const MemoryResource& a, const MemoryResource& b) noexcept
{
    return &a == &b || a.is_equal(b);
}

bool operator!=(const MemoryResource& a, const MemoryResource& b) noexcept
{
    return !(a == b);
}

class StdAllocatorResource
    : public MemoryResource
{
public:
    StdAllocatorResource() noexcept = default;
    ~StdAllocatorResource() noexcept override = default;

    void* do_allocate(std::size_t bytes, std::size_t /*alignment*/) override
    {
        return std::allocator<std::max_align_t>{}.allocate(getCount(bytes));
    }

    void do_deallocate(void* p, std::size_t bytes, std::size_t /*alignment*/) override
    {
        std::allocator<std::max_align_t>{}.deallocate(static_cast<std::max_align_t*>(p), getCount(bytes));
    }

    bool do_is_equal(const MemoryResource& other) const noexcept override
    {
        return nullptr != dynamic_cast<const StdAllocatorResource*>(&other);
    }

private:
    std::size_t getCount(std::size_t bytes) const noexcept
    {
        return (bytes - 1) / sizeof(std::max_align_t) + 1;
    }
};

MemoryResource* getDefaultResource() noexcept
{
    static StdAllocatorResource resource;
    return &resource;
}

enum class Byte : unsigned char {};

template <typename T = Byte>
class PolimorphicAllocator
{
public:
    using value_type = T;

    PolimorphicAllocator() noexcept = default;
    PolimorphicAllocator(const PolimorphicAllocator& other) noexcept = default;

    template<class U>
    PolimorphicAllocator(const PolimorphicAllocator<U>& other) noexcept
        : m_resource{ other.m_resource }
    {
        assert(m_resource);
    }

    PolimorphicAllocator(MemoryResource* r) noexcept
        : m_resource{ r }
    {
        assert(m_resource);
    }

    PolimorphicAllocator& operator=(const PolimorphicAllocator&) = delete;

    T* allocate(std::size_t n)
    {
        assert(m_resource);
        void* p = m_resource->allocate(n * sizeof(T), alignof(T));
        return static_cast<T*>(p);
    }

    void deallocate(T* p, std::size_t n)
    {
        assert(m_resource);
        m_resource->deallocate(p, n * sizeof(T), alignof(T));
    }

    MemoryResource* resource() const noexcept
    {
        assert(m_resource);
        return m_resource;
    }

private:
    template <typename> friend class PolimorphicAllocator;
    MemoryResource* const m_resource = getDefaultResource();
};

template <typename T1, typename T2>
bool operator==(const PolimorphicAllocator<T1>& a, const PolimorphicAllocator<T2>& b) noexcept
{
    return *a.resource() == *b.resource();
}

template <typename T1, typename T2>
bool operator!=(const PolimorphicAllocator<T1>& a, const PolimorphicAllocator<T2>& b) noexcept
{
    return !(a == b);
}

#endif // __cpp_lib_memory_resource


template <typename T = Byte>
class OwningPolimorphicAllocator
    : public PolimorphicAllocator<T>
{
public:
    OwningPolimorphicAllocator()
        : PolimorphicAllocator<T>{}
        , m_sharedResource{ this->resource(), boost::null_deleter{} }
    {
    }

    OwningPolimorphicAllocator(std::shared_ptr<MemoryResource> resource) noexcept
        : PolimorphicAllocator<T>{ resource.get() }
        , m_sharedResource{std::move(resource)}
    {
    }

    template<class U>
    OwningPolimorphicAllocator(const OwningPolimorphicAllocator<U>& other) noexcept
        : PolimorphicAllocator<T>{ other.resource() }
        , m_sharedResource{ other.m_sharedResource }
    {
    }

    std::shared_ptr<MemoryResource> sharedResource() const noexcept
    {
        return m_sharedResource;
    }

private:
    template <typename> friend class OwningPolimorphicAllocator;
    const std::shared_ptr<MemoryResource> m_sharedResource;
};


/// 'Phoenix' pattern.
template <typename A>
class AssignableAllocatorWrapper
    : public A
{
public:
    template<typename U>
    struct rebind
    {
        using other = AssignableAllocatorWrapper<typename std::allocator_traits<A>::template rebind_alloc<U>>;
    };

    AssignableAllocatorWrapper() = default;
    AssignableAllocatorWrapper(const A& a)
        : A{ a }
    {
    }

    template<class U>
    AssignableAllocatorWrapper(const AssignableAllocatorWrapper<U>& other)
        : A{ other }
    {
    }

    AssignableAllocatorWrapper& operator=(const AssignableAllocatorWrapper& other)
    {
        static_assert(
            std::is_nothrow_destructible<A>::value && std::is_nothrow_copy_constructible<A>::value
            , "operator= can't be implemented via AssignableAllocatorWrapper"
            );

        if (this == &other)
            return *this;

        static_cast<A*>(this)->~A();
        ::new(static_cast<void*>(this)) A{ other };

        return *this;
    }
};

template <typename A>
AssignableAllocatorWrapper<A> makeAllocatorAssignable(const A& a)
{
    return { a };
}

} // namespace memory
} // namespace ba
