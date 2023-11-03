#pragma once

#include "detail/function.hpp"
#include "detail/check_callable.hpp"

#include <type_traits>
#include <queue>
#include <mutex>

#include <boost/system/error_code.hpp>
#include <boost/asio/executor.hpp>
#include <boost/asio/execution_context.hpp>
#include <boost/asio/async_result.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/post.hpp>
#include <boost/core/noncopyable.hpp>

#if defined(BA_ASYNC_USE_BOOST_OPTIONAL) || !defined(__cpp_lib_optional)
#   include <boost/optional/optional.hpp>
#else
#   include <optional>
#endif

// Библиотека использует недокументированный boost::asio::detail::binder и move_binder
// Они широко используется во внутренностях asio, и, вероятно, его поддержка не иссякнет.
// Предпочтительнее использовать его, т.к. его поддерживают и могут добавить для него новые traits.
// Но, на случай, если он пропадет в будущих версиях boost,
// есть самописный аналог в .detail/preserved_binder.hpp, перед его использованием проверьте, что для него
// специализированы все нужные и современные traits, такие как boost::asio::associated_executor и т.д.
#include <boost/asio/detail/bind_handler.hpp>


namespace ba {
namespace async {

#if defined(BA_ASYNC_USE_BOOST_OPTIONAL) || !defined(__cpp_lib_optional)
    template <typename T>
    using optional = boost::optional<T>;
#else
    template <typename T>
    using optional = std::optional<T>;
#endif


/// Асинхронная очередь с ограничением длины (минимум 0).
/**
 * Потокобезопасная.
 * Вызывает хендлер завершения после постановки или получения элемента.
 */
template <
      typename T
    , typename Executor = boost::asio::executor
    , typename Container = std::queue<T>
    >
class Queue
{
public:
    using container_type = Container;
    using value_type = typename container_type::value_type;
    /// Тип executor'а, нужен для работы trait boost::asio::associated_executor.
    using executor_type = Executor;

    /// Создает очередь элементов типа T.
    /**
     * Исполняется на Executor ex.
     * Огрничена размером limit
     */
    explicit Queue(const executor_type& ex, std::size_t limit)
        : m_ex{ ex }
        , m_limit{ limit }
    {
        checkInvariant();
    }

    /// Создает очередь элементов типа T.
    /**
     * Аналогично с первым конструктором, но достает Executor из context,
     * например, из boost::asio::io_context.
     */
    template <typename ExecutionContext>
    explicit Queue(
          ExecutionContext& context
        , std::size_t limit
        , typename std::enable_if_t<
            std::is_convertible<ExecutionContext&, boost::asio::execution_context&>::value
            >* = 0
        )
        : Queue{ context.get_executor(), limit }
    {
    }

    /// Копироваться не умеет.
    Queue(const Queue&) = delete;
    Queue& operator=(const Queue&) = delete;

    /// Перемещаться умеет
    Queue(Queue&& other)
        : Queue(std::move(other), LockGuard{ other })
    {
    }

    /// Перемещаться умеет
    Queue& operator=(Queue&& other)
    {
        // Хоть и трудно себе представить deadlock в данном случае, на всякий 2 мьютекса лочатся атомарно.
        std::unique_lock<std::recursive_mutex> lkThis{ m_mutex, std::defer_lock };
        std::unique_lock<std::recursive_mutex> lkOther{ other.m_mutex, std::defer_lock };
        std::lock(lkThis, lkOther);

        other.checkInvariant();

        // Чистит себя.
        reset();
        // В other.m_ex остается копия, чтобы объект остался в валидном состоянии.
        m_ex = other.m_ex;
        m_limit = other.m_limit;
        m_queue = std::move(other.m_queue);
        m_pendingPush = std::move(other.m_pendingPush);
        m_pendingPop = std::move(other.m_pendingPop);
        checkInvariant();

        // other нужно очистить на случай,
        // если параметризовнный тип Container после перемещения оставляет в себе элементы.
        other.reset();

        return *this;
    }

    ~Queue()
    {
        // Отменяются все ждущие операции.
        cancel();
    }

    /// Асинхронно вставляет элемент и по завершению исполняет хендлер,
    /// порожденный от token
    /**
     * @param val элемент
     * @param token должен порождать хендлер или быть хендлером с сигнатурой:
     * @code void handler(
     *     const boost::system::error_code& error // результат операции.
     * ); @endcode
     * :)
     * Если очередь заполнена, вставка элемента и вызов handler произойдет
     * после очередного вызова @ref asyncPop.
     * При отмене ожидаемой операции error == boost::asio::error::operation_aborted.
     */
    template <typename U, typename PushToken>
    auto asyncPush(U&& val, PushToken&& token)
    {
        // Можно было бы определить аргумент val как value_type, но лишившись perfect forwarding.
        // Поэтому U&& и с проверкой is_convertible.
        static_assert(std::is_convertible<U, value_type>::value, "'val' must converts to 'value_type'");
#if BOOST_VERSION >= 107000
        return boost::asio::async_initiate<PushToken, void(boost::system::error_code)>(
            AsyncInit{ *this }, token, std::forward<U>(val)
            );
#else
        boost::asio::async_completion<
              PushToken
            , void(boost::system::error_code)
            > init{ token };

        initPush(std::forward<U>(val), init.completion_handler);
        return init.result.get();
#endif
    }

    /// Асинхронно извлекает элемент и по завершению исполняет хендлер,
    /// порожденный от token
    /**
     * @param val элемент
     * @param token должен порождать хендлер или быть хендлером с сигнатурой:
     * @code void handler(
     *       const boost::system::error_code& error // результат операции
     *     , optional<T> value // извлеченное значение
     * ); @endcode
     * :)
     * Если очередь пуста, извлечение элемента и вызов handler произойдет
     * после очередного вызова @ref asyncPush.
     * При отмене ожидаемой операции error == boost::asio::error::operation_aborted и
     * value == optional<T>{}
     */
    template <typename PopToken>
    auto asyncPop(PopToken&& token)
    {
#if BOOST_VERSION >= 107000
        return boost::asio::async_initiate<PopToken, void(boost::system::error_code, optional<value_type>)>(
            AsyncInit{ *this }, token
            );
#else
        boost::asio::async_completion<
              PopToken
            , void(boost::system::error_code, optional<value_type>)
            > init{ token };

        initPop(init.completion_handler);
        return init.result.get();
#endif
    }

    /// Возвращает executor, ассоциированный с объектом.
    /// Нужен для работы trait boost::asio::associated_executor.
    executor_type get_executor() const
    {
        LockGuard lkGuard{ *this };
        return m_ex;
    }

    bool empty() const
    {
        LockGuard lkGuard{ *this };
        return m_queue.empty();
    }

    bool full() const
    {
        LockGuard lkGuard{ *this };
        return m_limit == m_queue.size();
    }

    std::size_t size() const
    {
        LockGuard lkGuard{ *this };
        return m_queue.size();
    }

    std::size_t limit() const
    {
        LockGuard lkGuard{ *this };
        return m_limit;
    }

    /// Отменяет одну ожидающую операцию вставки и возвращяет их количество (0 или 1).
    std::size_t cancelOnePush()
    {
        LockGuard lkGuard{ *this };

        if (m_pendingPush.empty())
            return 0;

        doPendingPush(boost::asio::error::operation_aborted);

        return 1;
    }

    /// Отменяет все ожидающие операции вставки и возвращяет их количество.
    std::size_t cancelPush()
    {
        LockGuard lkGuard{ *this };

        std::size_t n = 0;
        while (cancelOnePush())
            ++n;

        return n;
    }

    /// Отменяет одну ожидающую операцию извлечения и возвращяет их количество (0 или 1).
    std::size_t cancelOnePop()
    {
        LockGuard lkGuard{ *this };

        if (m_pendingPop.empty())
            return 0;

        doPendingPop(optional<value_type>{}, boost::asio::error::operation_aborted);

        return 1;
    }

    /// Отменяет все ожидающие операции извлечения и возвращяет их количество.
    std::size_t cancelPop()
    {
        LockGuard lkGuard{ *this };

        std::size_t n = 0;
        while (cancelOnePop())
            ++n;

        return n;
    }

    /// Отменяет все ожидающие операции вставки и извлечения и возвращяет их количество.
    std::size_t cancel()
    {
        LockGuard lkGuard{ *this };

        // Сначала отменяются ждущие push.
        std::size_t n = cancelPush();
        // Потом pop, пусть будет определен такой порядок для пользователя.
        return n + cancelPop();
    }

    /// Очищает очередь. Отменяет все ожидающие операции и возвращяет их количество.
    std::size_t reset()
    {
        LockGuard lkGuard{ *this };

        // У std::queue не clear :)
        m_queue = container_type{};

        return cancel();
    }

private:
    // Обертка над std::unique_lock.
    // Не только лочит мьютекс, но и проверяет инвариант Queue в конструкторе и деструкторе.
    class LockGuard;

#if BOOST_VERSION >= 107000
    class AsyncInit;
#endif

    Queue(Queue&& other, const LockGuard&)
        : m_ex{ other.m_ex } // В other.m_ex остается копия, чтобы объект остался в валидном состоянии.
        , m_limit{ other.m_limit }
        , m_queue{ std::move(other.m_queue) }
        , m_pendingPush{ std::move(other.m_pendingPush) }
        , m_pendingPop{ std::move(other.m_pendingPop) }
    {
    }

    template <typename U, typename PushHandler>
    void initPush(U&& val, PushHandler&& handler)
    {
        // Чтобы пользователь получил вменяемую ошибку вместо портянки при ошибке в типе хендлера.
        static_assert(
              ba::async::detail::CheckCallable<
                  decltype(handler)
                , void(const boost::system::error_code&)
                >::value
            , "Handler signature must be 'void(const boost::system::error_code&)'"
            );

        // Начинаем блокироваться тут.
        LockGuard lkGuard{ *this };

        if (m_queue.empty() && !m_pendingPop.empty())
        {
            // Голодная очередь, ждут появления элемента.
            // В идеале в программе так должно быть большую часть времени,
            // когда разгребают быстрее, чем накидывают.
            // В данном случае нет необходимости класть в очередь, чтобы сразу достать обратно.

            // Сообщаем, что вставка прошла.
            completePush(std::forward<PushHandler>(handler));

            // Прокидываем элемент ждущему на asyncPop
            doPendingPop(std::forward<U>(val));

            return;
        }

        if (m_queue.size() < m_limit)
        {
            // Место есть, просто вставляем
            doPush(std::forward<U>(val), std::forward<PushHandler>(handler));

            return;
        }

        // Превышен лимит, откладываем операцию вставки.
        deferPush(std::forward<PushHandler>(handler), std::forward<U>(val));
    }

    template <typename PopHandler>
    void initPop(PopHandler&& handler)
    {
        // Чтобы пользователь получил вменяемую ошибку вместо портянки при ошибке в типе хендлера.
        static_assert(
            ba::async::detail::CheckCallable<
                  decltype(handler)
                , void(const boost::system::error_code&, optional<value_type>)
                >::value
            , "Handler signature must be 'void(const boost::system::error_code&, optional<T>)'"
            );

        // Начинаем блокироваться тут.
        LockGuard lkGuard{ *this };

        if (m_queue.empty() && !m_pendingPush.empty())
        {
            // Очередь пуста, но есть ожидающий вставки (такое может быть при очереди с лимитом 0)

            // Особенность: в данном методе прокинуть между ожидающим вставки и ожидающим извлечения
            // в обход очереди как в asyncPush не получается
            // (нет доступа к val, сохраненному в лямбде в m_pendingPush).
            // Поэтому даем вставке исполниться (тем самым вставляя в очередь, даже если у нее лимит 0).
            doPendingPush();
            assert(1 == m_queue.size());

            // Извлекаем и уведомляем вызовом хендлера.
            doPop(std::forward<PopHandler>(handler));

            return;
        }

        if (m_queue.empty())
        {
            // Очередь пустая, но никто не ждет вставки, откладываем операцию извлечения.
            deferPop(std::forward<PopHandler>(handler));

            return;
        }

        // Очередь не пустая, извлекаем и сообщаем об извлечении.
        doPop(std::forward<PopHandler>(handler));

        if (!m_pendingPush.empty())
        {
            // Есть ждущие вставки, исполняем отложенную вставку, т.к. освободилось место.
            doPendingPush();
        }
    }

    template <typename PushHandler>
    void completePush(PushHandler&& handler, const boost::system::error_code& ec = {})
    {
        boost::asio::post(
              m_ex
            , boost::asio::detail::bind_handler(std::forward<PushHandler>(handler), ec)
            );
    }

    template <typename PopHandler>
    void completePop(
          optional<value_type>&& val
        , PopHandler&& handler
        , const boost::system::error_code& ec = {}
        )
    {
        auto handlerCopy = std::forward<PopHandler>(handler);

        // move_binder2 вместо обычного, чтобы не копировать лишний раз val
        boost::asio::detail::move_binder2<
              decltype(handlerCopy)
            , boost::system::error_code
            , optional<value_type>
            > binder{
                  0
                , std::move(handlerCopy)
                , ec
                , std::move(val)
                };

        boost::asio::post(m_ex, std::move(binder));
    }

    template <typename U, typename PushHandler>
    void doPush(U&& val, PushHandler&& handler, const boost::system::error_code& ec = {})
    {
        if (!ec)
            m_queue.push(std::forward<U>(val));

        completePush(std::forward<PushHandler>(handler), ec);
    }

    template <typename PopHandler>
    void doPop(PopHandler&& handler, const boost::system::error_code& ec = {})
    {
        if (ec)
        {
            completePop(optional<value_type>{}, std::forward<PopHandler>(handler), ec);
            return;
        }

        completePop(std::move(m_queue.front()), std::forward<PopHandler>(handler), ec);
        m_queue.pop();
    }

    template <typename Handler>
    void deferPush(Handler&& handler, value_type val)
    {
        // work нужен, чтобы лямбда держала executor и предотвращала от выхода из run,
        // пока отложенный push не будет выполнен.
        // В этот момент в очереди asio executor может быть и пусто,
        // но по логике Queue имеет отложенную операцию, до исполнения которой run должен крутиться.
        auto work = boost::asio::make_work_guard(m_ex);
        m_pendingPush.emplace([
              val{ std::move(val) }
            , handler{ std::forward<Handler>(handler) }
            , work{ std::move(work) }
            ](
              Queue& self
            , const boost::system::error_code& ec
            ) mutable {
                self.doPush(std::move(val), std::move(handler), ec);
            }
            , boost::asio::get_associated_allocator(handler)
            );
    }

    template <typename Handler>
    void deferPop(Handler&& handler)
    {
        // work нужен, чтобы лямбда держала executor и предотвращала от выхода из run,
        // пока отложенный push не будет выполнен.
        // В этот момент в очереди asio executor может быть и пусто,
        // но по логике Queue имеет отложенную операцию, до исполнения которой run должен крутиться.
        auto work = boost::asio::make_work_guard(m_ex);
        m_pendingPop.emplace([
              handler{ std::forward<Handler>(handler) }
            , work{ std::move(work) }
            ](
              Queue& self
            , optional<value_type>&& val
            , const boost::system::error_code& ec
            ) mutable {
                self.completePop(std::move(val), std::move(handler), ec);
            }
            , boost::asio::get_associated_allocator(handler)
            );
    }

    void doPendingPush(const boost::system::error_code& ec = {})
    {
        m_pendingPush.front()(*this, ec);
        m_pendingPush.pop();
    }

    void doPendingPop(optional<value_type>&& val, const boost::system::error_code& ec = {})
    {
        // Прокидываем элемент ждущему на asyncPop
        m_pendingPop.front()(*this, std::move(val), ec);
        m_pendingPop.pop();
    }

    void checkInvariant() const
    {
        assert(m_queue.size() <= m_limit);
        assert(m_queue.empty() || m_pendingPop.empty());
        assert(m_queue.size() == m_limit || m_pendingPush.empty());
    }

private:
    mutable std::recursive_mutex m_mutex;
    executor_type m_ex;
    std::size_t m_limit = 0;
    container_type m_queue;

    // Здесь хранятся ожидающие операции вставки, когда очередь переолнена.
    std::queue<
        detail::Function<void(Queue&, const boost::system::error_code&)>
        > m_pendingPush;

    // Здесь хранятся ожидающие операции извлечения, когда очередь пуста.
    std::queue<
        detail::Function<void(Queue&, optional<value_type>&&, const boost::system::error_code&)>
        > m_pendingPop;
};

// Обертка над std::unique_lock.
// Не только лочит мьютекс, но и проверяет инвариант Queue в конструкторе и деструкторе.
template <typename T, typename Executor, typename Container>
class Queue<T, Executor, Container>::LockGuard
    : boost::noncopyable
{
public:
    explicit LockGuard(const Queue& self)
        : m_self{ self }, m_lk{ self.m_mutex }
    {
        m_self.checkInvariant();
    }

    ~LockGuard()
    {
        m_self.checkInvariant();
    }

private:
    const Queue& m_self;
    std::lock_guard<std::recursive_mutex> m_lk;
};

#if BOOST_VERSION >= 107000
template <typename T, typename Executor, typename Container>
class Queue<T, Executor, Container>::AsyncInit
{
public:
    using executor_type = typename Queue::executor_type;

    AsyncInit(Queue& self)
        : m_self{ self }
    {
    }

    template <typename PushHandler, typename U>
    void operator()(PushHandler&& handler, U&& val) const
    {
        m_self.initPush(std::forward<U>(val), std::forward<PushHandler>(handler));
    }

    template <typename PopHandler>
    void operator()(PopHandler&& handler) const
    {
        m_self.initPop(std::forward<PopHandler>(handler));
    }

    executor_type get_executor() const
    {
        return m_self.get_executor();
    }

private:
    Queue& m_self;
};
#endif // BOOST_VERSION >= 107000

} // namespace async
} // namespace ba
