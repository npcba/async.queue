#pragma once

#include "compressed_pair.hpp"

#include <type_traits>
#include <utility>

#include <boost/asio/associated_allocator.hpp>

namespace ba {
namespace async {
namespace detail {

// Связывает первый аргумент FirstArg функции F.
// Похоже на std::bind1st, но дополнительно для биндера сделана специализация boost::asio::associated_allocator (см. ниже).
// Это позволяет писать лямбды, в которые захватываются хендлеры и не терять при этом возможности получать
// ассоциировнный с хендлером аллокатор через boost::asio::associated_allocator.
//
// F - лямбда, FirstArg - первым аргументом должен быть хендлер.
// Проксирует запрос ассоциированного аллокатора к FirstArg.
template <typename F, typename FirstArg>
class AssociatedBinder
{
public:
    template <typename F_, typename FirstArg_>
    AssociatedBinder(F_&& f, FirstArg_&& firstArg)
        : m_pair{ std::forward<FirstArg_>(firstArg), std::forward<F_>(f) }
    {
    }

    template <typename... RestArgs>
    auto operator()(RestArgs&&... restArgs) const
    {
        return getF()(getFirstArg(), std::forward<RestArgs>(restArgs)...);
    }

    template <typename... RestArgs>
    auto operator()(RestArgs&&... restArgs)
    {
        return getF()(getFirstArg(), std::forward<RestArgs>(restArgs)...);
    }

    const F& getF() const noexcept
    {
        return m_pair.getSolid();
    }

    F& getF() noexcept
    {
        return m_pair.getSolid();
    }

    const FirstArg& getFirstArg() const noexcept
    {
        return m_pair.getEmpty();
    }

    FirstArg& getFirstArg() noexcept
    {
        return m_pair.getEmpty();
    }

private:
    // Сжимаем FirstArg, в данной библиотеке это пользовательский хендлер. Зачастую бывает без состояния.
    CompressedPair<FirstArg, F> m_pair;
};

template <typename F, typename FirstArg>
AssociatedBinder<std::decay_t<F>, std::decay_t<FirstArg>> bindAssociated(F&& f, FirstArg&& firstArg)
{
    return { std::forward<F>(f), std::forward<FirstArg>(firstArg) };
}

} // namespace detail
} // namespace async
} // namespace ba


// Специализация boost::asio::associated_allocator для AssociatedBinder.
// Проксирует запрос ассоциированного аллокатора к FirstArg.
template <typename F, typename FirstArg, typename Allocator>
struct boost::asio::associated_allocator<
      ba::async::detail::AssociatedBinder<F, FirstArg>
    , Allocator
    >
{
    using type = boost::asio::associated_allocator_t<FirstArg, Allocator>;

    static type get(
          const ba::async::detail::AssociatedBinder<F, FirstArg>& b
        , const Allocator& a = Allocator{}
        ) noexcept
    {
        return boost::asio::associated_allocator<FirstArg, Allocator>::get(b.getFirstArg(), a);
    }
};
