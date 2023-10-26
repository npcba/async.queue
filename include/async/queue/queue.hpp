#pragma once

#include <type_traits>
#include <queue>
#include <functional>
#include <mutex>

#include <boost/system/error_code.hpp>
#include <boost/asio/executor.hpp>
#include <boost/asio/execution_context.hpp>
#include <boost/asio/async_result.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/post.hpp>
#include <boost/core/noncopyable.hpp>

#ifdef __cpp_lib_optional
#   include <optional>
#else
#   include <boost/optional/optional.hpp>
#endif

// Библиотека использует недокументированный boost::asio::detail::binder и move_binder
// Они широко используется во внутренностях asio, и, вероятно, его поддержка не иссякнет.
// Предпочтительнее использовать его, т.к. его поддерживают и могут добавить для него новые traits.
// Но, на случай, если он пропадет в будущих версиях boost,
// есть самописный аналог в ./preserved_binder.hpp, перед его использованием проверьте, что для него
// специализированы все нужные и современные traits, такие как boost::asio::associated_executor и т.д.
#include <boost/asio/detail/bind_handler.hpp>


namespace ba {
namespace async {

#ifdef __cpp_lib_optional
    template <typename T>
    using optional = std::optional<T>;
#else
    template <typename T>
    using optional = boost::optional<T>;
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
        static_assert(std::is_convertible<U, value_type>::value, "val must converts to value_type");

        boost::asio::async_completion<
              PushToken
            , void(boost::system::error_code)
            > init{ token };

        // Чтобы пользователь получил вменяемую ошибку вместо портянки при ошибке в типе хендлера.
        static_assert(
              std::is_convertible<
                  decltype(init.completion_handler)
                , std::function<void(const boost::system::error_code&)>
                >::value
            , "Handler signature must be void(const boost::system::error_code&)"
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
            invokePushHandler(init.completion_handler, boost::system::error_code{});

            // Прокидываем элемент ждущему на asyncPop
            m_pendingPop.front()(
                  *this
                , boost::system::error_code{}
                , optional<value_type>(std::forward<U>(val))
                );
            m_pendingPop.pop();

            lkGuard.unlock();
            return init.result.get();
        }

        if (m_queue.size() < m_limit)
        {
            // Место есть, просто вставляем
            m_queue.push(std::forward<U>(val));
            lkGuard.unlock();

            // Сообщаем, что вставка прошла.
            invokePushHandler(init.completion_handler, boost::system::error_code{});

            return init.result.get();
        }

        // Превышен лимит, откладываем операцию вставки.

        // work нужен, чтобы лямбда держала executor и предотвращала от выхода из run,
        // пока отложенный push не будет выполнен.
        // В этот момент в очереди asio executor может быть и пусто,
        // но по логике Queue имеет отложенную операцию, до исполнения которой run должен крутиться.
        auto work = boost::asio::make_work_guard(m_ex);

        m_pendingPush.push([
              val = value_type{ std::forward<U>(val) }
            , handler{ init.completion_handler }
            , work{ std::move(work) }
            ](
              Queue& self
            , const boost::system::error_code& ec
            ) mutable {
                if (!ec)
                    self.m_queue.push(std::move(val));

                self.invokePushHandler(std::move(handler), ec);
                work.reset();
            });

        // Обязательно нужно разлочиться до вызова init.result.get(), потому что
        // в случае работы на корутинах внутри get корутина уходит в суспенд,
        // и мьютекс на неопределенное время будет заблокирован.
        lkGuard.unlock();
        return init.result.get();
    }

    /// Асинхронно извлекает элемент и по завершению исполняет хендлер,
    /// порожденный от token
    /**
     * @param val элемент
     * @param token должен порождать хендлер или быть хендлером с сигнатурой:
     * @code void handler(
     *       const boost::system::error_code& error // результат операции
     *     , optional<T>&& value // извлеченное значение
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
        boost::asio::async_completion<
              PopToken
            , void(boost::system::error_code, optional<value_type>&&)
            > init{ token };

        // Чтобы пользователь получил вменяемую ошибку вместо портянки при ошибке в типе хендлера.
        static_assert(
              std::is_convertible<
                  decltype(init.completion_handler)
                , std::function<void(const boost::system::error_code&, optional<value_type>&&)>
                >::value
            , "Handler signature must be void(const boost::system::error_code&, optional<T>&&)"
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
            m_pendingPush.front()(*this, boost::system::error_code{});
            m_pendingPush.pop();
            assert(1 == m_queue.size());

            // Сообщаем, что извлечение прошло.
            invokePopHandler(
                  init.completion_handler
                , boost::system::error_code{}
                , std::move(m_queue.front())
                );
            // И извлекаем этот единственный элемент.
            m_queue.pop();

            lkGuard.unlock();
            return init.result.get();
        }

        if (m_queue.empty())
        {
            // Очередь пустая, но никто не ждет вставки, откладываем операцию извлечения.

            // work нужен, чтобы лямбда держала executor и предотвращала от выхода из run,
            // пока отложенный push не будет выполнен.
            // В этот момент в очереди asio executor может быть и пусто,
            // но по логике Queue имеет отложенную операцию, до исполнения которой run должен крутиться.
            auto work = boost::asio::make_work_guard(m_ex);
            m_pendingPop.push([
                  handler{ init.completion_handler }
                , work{ std::move(work) }
                ](
                  Queue& self
                , const boost::system::error_code& ec
                , optional<value_type>&& val
                ) mutable {
                    self.invokePopHandler(std::move(handler), ec, std::move(val));
                    work.reset();
                });

            lkGuard.unlock();
            return init.result.get();
        }

        // Очередь не пустая, сообщаем об извлечении.
        invokePopHandler(
              init.completion_handler
            , boost::system::error_code{}
            , std::move(m_queue.front())
            );
        // Извлекаем.
        m_queue.pop();

        if (!m_pendingPush.empty())
        {
            // Есть ждущие вставки, исполняем отложенную вставку, т.к. освободилось место.
            m_pendingPush.front()(*this, boost::system::error_code{});
            m_pendingPush.pop();
        }

        // Обязательно нужно разлочиться до вызова init.result.get(), потому что
        // в случае работы на корутинах внутри get корутина уходит в суспенд,
        // и мьютекс на неопределенное время будет заблокирован.
        lkGuard.unlock();
        return init.result.get();
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

    std::size_t limit() const noexcept
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

        m_pendingPush.front()(*this, boost::asio::error::operation_aborted);
        m_pendingPush.pop();

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

        m_pendingPop.front()(*this, boost::asio::error::operation_aborted, optional<value_type>{});
        m_pendingPop.pop();

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
    class LockGuard
        : boost::noncopyable
    {
    public:
        explicit LockGuard(const Queue& self)
            : m_self{ self }, m_lk{ self.m_mutex }
        {
            m_self.checkInvariant();
        }

        ~LockGuard() noexcept
        {
            if (m_lk)
                m_self.checkInvariant();
        }

        void unlock()
        {
            m_self.checkInvariant();
            m_lk.unlock();
        }

    private:
        const Queue& m_self;
        std::unique_lock<std::recursive_mutex> m_lk;
    };

    Queue(Queue&& other, const LockGuard&)
        : m_ex{ other.m_ex } // В other.m_ex остается копия, чтобы объект остался в валидном состоянии.
        , m_limit{ other.m_limit }
        , m_queue{ std::move(other.m_queue) }
        , m_pendingPush{ std::move(other.m_pendingPush) }
        , m_pendingPop{ std::move(other.m_pendingPop) }
    {
    }

    template <typename PushHandler>
    void invokePushHandler(PushHandler&& handler, const boost::system::error_code& ec)
    {
        boost::asio::post(
              m_ex
            , boost::asio::detail::bind_handler(std::forward<PushHandler>(handler), ec)
            );
    }

    template <typename PopHandler>
    void invokePopHandler(
          PopHandler&& handler
        , const boost::system::error_code& ec
        , optional<value_type>&& val
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

    void checkInvariant() const noexcept
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
        std::function<void(Queue&, const boost::system::error_code&)>
        > m_pendingPush;

    // Здесь хранятся ожидающие операции извлечения, когда очередь пуста.
    std::queue<
        std::function<void(Queue&, const boost::system::error_code&, optional<value_type>&&)>
        > m_pendingPop;

    // TODO: m_pendingPush можно переделать на пары std::function и value_type, чтобы не хранить элементы внутри лямбды.
    // Лямбда оборачивается в std::function, а она в свою очередь требует от содержимого CopyConstructible,
    // что в свою очередь требует CopyConstructible от value_type.
};

} // namespace async
} // namespace ba
