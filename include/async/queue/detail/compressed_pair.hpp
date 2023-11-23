#pragma once

#include <type_traits>
#include <utility>

namespace ba {
namespace async {
namespace detail {

template <typename Empty, typename Solid, typename Enable = void>
class CompressedPair
{
public:
    template<typename Empty_, typename Solid_>
    CompressedPair(Empty_&& empty, Solid_&& solid)
        : m_empty{ std::forward<Empty_>(empty) }, m_solid{ std::forward<Solid_>(solid) }
    {
    }

    const Empty& getEmpty() const noexcept
    {
        return m_empty;
    }

    Empty& getEmpty() noexcept
    {
        return m_empty;
    }

    const Solid& getSolid() const noexcept
    {
        return m_solid;
    }

    Solid& getSolid() noexcept
    {
        return m_solid;
    }

private:
    Empty m_empty;
    Solid m_solid;
};

template <typename Empty, typename Solid>
class CompressedPair<Empty, Solid, std::enable_if_t<std::is_empty<Empty>::value && !std::is_final<Empty>::value>>
    : private Empty
{
public:
    template<typename Empty_, typename Solid_>
    CompressedPair(Empty_&& empty, Solid_&& solid)
        : Empty{ std::forward<Empty_>(empty) }, m_solid{ std::forward<Solid_>(solid) }
    {
    }

    const Empty& getEmpty() const noexcept
    {
        return *this;
    }

    Empty& getEmpty() noexcept
    {
        return *this;
    }

    const Solid& getSolid() const noexcept
    {
        return m_solid;
    }

    Solid& getSolid() noexcept
    {
        return m_solid;
    }

private:
    Solid m_solid;
};

} // namespace detail
} // namespace async
} // namespace ba
