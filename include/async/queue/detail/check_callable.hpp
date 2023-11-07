#pragma once

#include <type_traits>


namespace ba {
namespace async {
namespace detail {

// Trait проверяет на возможность объекта типа F быть вызванным с аргументами типа Args.
// По-умолчанию не может быть вызван (false).
template <typename F, typename Signature, typename EnableIf = void>
struct CheckCallable : std::false_type {};

// Сециализация, для корректно-вызываемого F (true).
template<typename F, typename R, typename... Args>
struct CheckCallable<
      F
    , R(Args...)
    , std::enable_if_t<
        std::is_convertible<
#ifdef __cpp_lib_is_invocable
              std::invoke_result_t<F&, Args...> // since C++17
#else
              std::result_of_t<F& (Args...)>    // deprecated in C++17, deleted in C++20
#endif
            , R
            >::value ||                         // Результат либо конверируется в R, либо void.
        std::is_void<R>::value
        >
    >
    : std::true_type
{
};

} // namespace detail
} // namespace async
} // namespace ba
