#pragma once

#include <type_traits>
#include <utility>

#include <boost/config.hpp>


namespace ba {
namespace async {
namespace detail {

// Зачастую аллокторы и хендлеры не имеют состояния.
// Сжимаем их, используя EBO, либо [[no_unique_address]] из C++20.
// Первым идет тип, который более вероятно пустой.
// В текущей библиотеке Solid всегда параметризуется непустым типом.
template <
      typename Empty
    , typename Solid
#if __cplusplus < 202002L // C++20
    , typename Enable = void
#endif
    >
class CompressedPair
{
public:
    template<typename Empty_, typename Solid_>
    CompressedPair(Empty_&& empty, Solid_&& solid)
        : m_empty{ std::forward<Empty_>(empty) }
        , m_solid{ std::forward<Solid_>(solid) }
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
#if __cplusplus >= 202002L // C++20
    [[no_unique_address]]
#   ifdef BOOST_MSVC
    [[msvc::no_unique_address]]
#   endif // BOOST_MSVC
#endif // __cplusplus >= 202002L // C++20
    Empty m_empty;
    Solid m_solid;
};


#if __cplusplus < 202002L // C++20
// Специализация для случая пустого Empty и при этом не final.
template <typename Empty, typename Solid>
class CompressedPair<
      Empty
    , Solid
    , std::enable_if_t<std::is_empty<Empty>::value && !std::is_final<Empty>::value>
    >
    : private Empty
{
public:
    template<typename Empty_, typename Solid_>
    CompressedPair(Empty_&& empty, Solid_&& solid)
        : Empty{ std::forward<Empty_>(empty) }
        , m_solid{ std::forward<Solid_>(solid) }
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
#endif // __cplusplus < 202002L // C++20

} // namespace detail
} // namespace async
} // namespace ba
