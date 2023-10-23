#pragma once

#include <tuple>
#include <utility>
#include <type_traits>

#include <boost/asio/associated_allocator.hpp>
#include <boost/asio/associated_executor.hpp>
#include <boost/asio/handler_continuation_hook.hpp>


namespace ba {
namespace async {

template<typename F, typename Tuple, std::size_t... I>
constexpr decltype(auto)
apply_impl(F&& f, Tuple&& t, std::index_sequence<I...>)
{
    return std::forward<F>(f)(std::get<I>(std::forward<Tuple>(t))...);
}

template <typename F, typename Tuple>
constexpr decltype(auto)
apply(F&& f, Tuple&& t)
{
    return apply_impl(
          std::forward<F>(f)
        , std::forward<Tuple>(t)
        , std::make_index_sequence<std::tuple_size<std::decay_t<Tuple>>::value>{}
        );
}

template <typename F, typename... Args>
class PreservedBinder
{
public:
    explicit PreservedBinder(F&& f, Args&&... args)
        : m_handler{ std::forward<F>(f) }, m_args{ std::forward<Args>(args)... }
    {
    }

    auto operator()() const
    {
        return apply(m_handler, m_args);
    }

    auto operator()()
    {
        return apply(m_handler, m_args);
    }

    const F& get_inner() const
    {
        return m_handler;
    }

    F& get_inner()
    {
        return m_handler;
    }

private:
    std::decay_t<F> m_handler;
    std::tuple<std::decay_t<Args>...> m_args;
};

template <typename F, typename... Args>
auto preservedBind(F&& f, Args&&... args)
{
    return PreservedBinder<F, Args...>{ std::forward<F>(f), std::forward<Args>(args)... };
}

} // namespace async
} // namespace ba

// Calls to asio_handler_is_continuation must be made from a namespace that
// does not contain overloads of this function. This namespace is defined here
// for that purpose.
namespace handler_cont_helpers {

template <typename Context>
inline bool is_continuation(Context& context)
{
  using boost::asio::asio_handler_is_continuation;
  return asio_handler_is_continuation(std::addressof(context));
}

} // namespace handler_cont_helpers

namespace boost {
namespace asio {

template <typename F, typename... Args, typename Executor>
struct associated_executor<ba::async::PreservedBinder<F, Args...>, Executor>
{
    typedef typename associated_executor<F, Executor>::type type;

    static type get(const ba::async::PreservedBinder<F, Args...>& b,
        const Executor& ex = Executor()) BOOST_ASIO_NOEXCEPT
    {
        return associated_executor<F, Executor>::get(b.get_inner(), ex);
    }
};

template <typename F, typename... Args, typename Allocator>
struct associated_allocator<ba::async::PreservedBinder<F, Args...>, Allocator>
{
    typedef typename associated_allocator<F, Allocator>::type type;

    static type get(const ba::async::PreservedBinder<F, Args...>& b,
      const Allocator& a = Allocator()) BOOST_ASIO_NOEXCEPT
    {
        return associated_allocator<F, Allocator>::get(b.get_inner(), a);
    }
};

template <typename F, typename... Args>
inline bool asio_handler_is_continuation(
    ba::async::PreservedBinder<F, Args...>* this_handler)
{
    assert(this_handler);
    return handler_cont_helpers::is_continuation(this_handler->get_inner());
}

} //namespace asio
} //namespace boost
