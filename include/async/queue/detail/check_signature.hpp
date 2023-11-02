#pragma once

#include <type_traits>


namespace ba {
namespace async {
namespace detail {

template <typename F, typename Signature, typename Enable = void>
struct CheckSignature : std::false_type {};

template<typename F, typename R, typename... Args>
struct CheckSignature<
      F
    , R(Args...)
    , std::enable_if_t<
        std::is_convertible<
              std::result_of_t<F& (Args...)>
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
