#pragma once

#include <type_traits>


namespace ba {
namespace async {
namespace detail {

template <typename F, typename Signature, typename EnableIf = void>
struct CheckCallable : std::false_type {};

template<typename F, typename R, typename... Args>
struct CheckCallable<
      F
    , R(Args...)
    , std::enable_if_t<
        std::is_convertible<
#ifdef __cpp_lib_is_invocable
              std::invoke_result_t<F&, Args...>
#else
              std::result_of_t<F& (Args...)>
#endif
            , R
            >::value ||
        std::is_void<R>::value
        >
    >
    : std::true_type
{
};

} // namespace detail
} // namespace async
} // namespace ba
