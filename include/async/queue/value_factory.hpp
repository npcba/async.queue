#pragma once

#include <tuple>
#include <utility>
#include <type_traits>

#include <boost/system/error_code.hpp>

namespace ba {
namespace async {

template <typename T, typename... Args>
class ValueFactory
{
public:
    template <typename... Args_>
    ValueFactory(Args_&&... args)
        : m_args{ std::forward<Args_>(args)... }
    {
    }

    T operator()(const boost::system::error_code&) const
    {
        return construct(m_args, std::make_index_sequence<sizeof...(Args)>{});
    }

    T operator()(const boost::system::error_code& ec)
    {
        return construct(m_args, std::make_index_sequence<sizeof...(Args)>{});
    }

private:
    template<typename Tuple, std::size_t... I>
    static T construct(Tuple& t, std::index_sequence<I...>)
    {
        return T( std::get<I>(t)... );
    }

    std::tuple<Args...> m_args;
};

// Специализация для конструирования без аргументов,
// чтобы не хранить пустой std::tuple, который делает класс ValueFactory непустым.
template <typename T>
class ValueFactory<T>
{
public:
    T operator()(const boost::system::error_code&) const
    {
        return T{};
    }
};


template <typename T, typename... Args>
ValueFactory<T, std::decay_t<Args>...> makeValueFactory(Args&&... args)
{
    return { std::forward<Args>(args)... };
}

} // namespace async
} // namespace ba
