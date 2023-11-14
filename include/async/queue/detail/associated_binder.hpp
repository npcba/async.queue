#pragma once

#include <type_traits>

#include <boost/asio/associated_allocator.hpp>

namespace ba {
namespace async {
namespace detail {

template <typename F, typename FirstArg>
class AssociatedBinder
    : private FirstArg
{
public:
    template <typename F_, typename FirstArg_>
    AssociatedBinder(F_&& f, FirstArg_&& firstArg)
        : FirstArg{ std::forward<FirstArg_>(firstArg) }, m_f{ std::forward<F_>(f) }
    {
    }
    
    template <typename... RestArgs>
    auto operator()(RestArgs&&... restArgs)
    {
        return m_f(static_cast<FirstArg&>(*this), std::forward<RestArgs>(restArgs)...);
    }

    const FirstArg& getFirstArg() const noexcept
    {
        return *this;
    }

    FirstArg& getFirstArg() noexcept
    {
        return *this;
    }

private:
    F m_f;
};

template <typename F, typename FirstArg>
AssociatedBinder<std::decay_t<F>, std::decay_t<FirstArg>> bindAssociated(F&& f, FirstArg&& firstArg)
{
    return { std::forward<F>(f), std::forward<FirstArg>(firstArg) };
}

} // namespace detail
} // namespace async
} // namespace ba


template <typename F, typename FirstArg, typename Allocator>
struct boost::asio::associated_allocator<ba::async::detail::AssociatedBinder<F, FirstArg>, Allocator>
{
    using type = boost::asio::associated_allocator_t<FirstArg, Allocator>;

    static type get(const ba::async::detail::AssociatedBinder<F, FirstArg>& b,
        const Allocator& a = Allocator{}) noexcept
    {
        return boost::asio::associated_allocator<FirstArg, Allocator>::get(b.getFirstArg(), a);
    }
};
