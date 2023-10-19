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
    return apply_impl(std::forward<F>(f), std::forward<Tuple>(t),
        std::make_index_sequence<
        std::tuple_size_v<std::decay_t<Tuple>>>{});
}

template <typename F, typename... Args>
class PreservedBinder
{
public:
    explicit PreservedBinder(F f, Args... args)
        : m_f{ f }, m_args{ args... }
    {
    }

    auto operator()()
    {
        return apply(m_f, m_args);
    }

    F f() const
    {
        return m_f;
    }

private:
    F m_f;
    std::tuple<std::decay_t<Args>...> m_args;
};

template <typename F, typename... Args>
auto preservedBind(F f, Args... args)
{
    return PreservedBinder<F, Args...>{ f, args... };
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
  return asio_handler_is_continuation(
      boost::asio::detail::addressof(context));
}

}

namespace boost {
namespace asio {

template <typename F, typename... Args, typename Executor>
struct associated_executor<PreservedBinder<F, Args...>, Executor>
{
  typedef typename associated_executor<F, Executor>::type type;

  static type get(const PreservedBinder<F, Args...>& b,
      const Executor& ex = Executor()) BOOST_ASIO_NOEXCEPT
  {
    return associated_executor<F, Executor>::get(b.f(), ex);
  }
};

template <typename F, typename... Args, typename Allocator>
struct associated_allocator<PreservedBinder<F, Args...>, Allocator>
{
  typedef typename associated_allocator<F, Allocator>::type type;

  static type get(const PreservedBinder<F, Args...>& b,
      const Allocator& a = Allocator()) BOOST_ASIO_NOEXCEPT
  {
    return associated_allocator<F, Allocator>::get(b.f(), a);
  }
};

template <typename F, typename... Args>
inline bool asio_handler_is_continuation(
    PreservedBinder<F, Args...>* this_handler)
{
  return handler_cont_helpers::is_continuation(
      this_handler->f());
}

} //namespace asio
} //namespace boost
