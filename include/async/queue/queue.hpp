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
      typename Elem
    , typename Executor = boost::asio::executor
    , typename Container = std::queue<Elem>
    >
class Queue
{
public:
    using container_type = Container;
    using value_type = typename container_type::value_type;
    /// Тип executor'а, нужен для работы trait boost::asio::associated_executor.
    using executor_type = Executor;

    /// Создает очередь элементов типа Elem.
    /**
     * Исполняется на Executor ex.
     * Огрничена размером limit.
     * В некоторых случаях в процессе работы Container может заполняться до размера limit + 1,
     * но снаружи этого не заметно, важно, чтобы пользовательский Container допускал такой размер.
     */
    explicit Queue(const executor_type& ex, std::size_t limit)
        : m_ex{ ex }
        , m_limit{ limit }
    {
        checkInvariant();
    }

    /// Создает очередь элементов типа Elem.
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
        m_pendingPushQueue = std::move(other.m_pendingPushQueue);
        m_pendingPopQueue = std::move(other.m_pendingPopQueue);
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
        // Важно: захват мьютекса в этом методе делать нельзя,
        // Вконце asio::async_initiate или init.result.get() происходит суспенд корутины
        // (если вызов происходит на ней).
        // Вместо этого мьютекс лочится в initPush.

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
     *     , optional<Elem> value // извлеченное значение
     * ); @endcode
     * :)
     * Если очередь пуста, извлечение элемента и вызов handler произойдет
     * после очередного вызова @ref asyncPush.
     * При отмене ожидаемой операции error == boost::asio::error::operation_aborted и
     * value == optional<Elem>{}
     */
    template <typename PopToken>
    auto asyncPop(PopToken&& token)
    {
        // Важно: захват мьютекса в этом методе делать нельзя,
        // Вконце asio::async_initiate или init.result.get() происходит суспенд корутины
        // (если вызов происходит на ней).
        // Вместо этого мьютекс лочится в initPop.

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
        return m_queue.size() >= m_limit;
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

        if (m_pendingPushQueue.empty())
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

        if (m_pendingPopQueue.empty())
            return 0;

        doPendingPop(boost::asio::error::operation_aborted);

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

    template <typename Handler>
    class PendingPopOp;
    template <typename Handler>
    class PendingPushOp;

#if BOOST_VERSION >= 107000
    class AsyncInit;
#endif

    Queue(Queue&& other, const LockGuard&)
        : m_ex{ other.m_ex } // В other.m_ex остается копия, чтобы объект остался в валидном состоянии.
        , m_limit{ other.m_limit }
        , m_queue{ std::move(other.m_queue) }
        , m_pendingPushQueue{ std::move(other.m_pendingPushQueue) }
        , m_pendingPopQueue{ std::move(other.m_pendingPopQueue) }
    {
    }

    // Инициатор вставки.
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

        // Именно тут блокируемся, а не в asyncPush.
        LockGuard lkGuard{ *this };

        // Если лимит не превышен или есть ожидающие извлечения, то вставляем, иначе откладываем.
        if (m_queue.size() < m_limit || !m_pendingPopQueue.empty())
            doPush(std::forward<U>(val), std::forward<PushHandler>(handler));
        else
            deferPush(std::forward<PushHandler>(handler), std::forward<U>(val));

        // Если есть ожидающие извлечения, выполняем извлечение.
        if (!m_pendingPopQueue.empty())
        {
            // Если ждали извлечения, значит до этого уперлись в size == 0, и теперь он стал равен 1.
            // При m_limit == 0, в очереди кратковременно появится 1 элемент, но снаружи это не заметно.
            assert(m_queue.size() == 1);
            doPendingPop();
        }
    }

    // Инициатор извлечения.
    template <typename PopHandler>
    void initPop(PopHandler&& handler)
    {
        // Чтобы пользователь получил вменяемую ошибку вместо портянки при ошибке в типе хендлера.
        static_assert(
            ba::async::detail::CheckCallable<
                  decltype(handler)
                , void(const boost::system::error_code&, optional<value_type>)
                >::value
            , "Handler signature must be 'void(const boost::system::error_code&, optional<Elem>)'"
            );

        // Именно тут блокируемся, а не в asyncPop.
        LockGuard lkGuard{ *this };

        // Если есть ождающие вставки, то вставляем.
        if (!m_pendingPushQueue.empty())
        {
            doPendingPush();
            // Если ждали вставки, значит до этого уперлись в лимит,
            // и теперь он будет кратковременно превышен на 1 элемент, но снаружи это не заметно.
            assert(m_queue.size() == m_limit + 1);
        }

        // Если очередь не пуста, то извлекаем, иначе откладываем.
        if (!m_queue.empty())
            doPop(std::forward<PopHandler>(handler));
        else
            deferPop(std::forward<PopHandler>(handler));
    }

    // Комплитер вставки.
    template <typename PushHandler>
    void completePush(PushHandler&& handler, const boost::system::error_code& ec = {})
    {
        // Сообщаем о завершении вставки обязательно через post,
        // выполнение хендлеров асинхронных операций запрещено исполнять внутри функций-инициаторов.
        boost::asio::post(
              m_ex
            , boost::asio::detail::bind_handler(std::forward<PushHandler>(handler), ec)
            );
    }

    // Комплитер извлечения.
    template <typename PopHandler>
    void completePop(
          optional<value_type>&& val
        , PopHandler&& handler
        , const boost::system::error_code& ec = {}
        )
    {
        // move_binder2 не умеет форвардить handler, только перемещает.
        // Копируем, если lvalue-ссылка, иначе форвардим на перемещение (rvalue).
        using CopyIfLValueRef = std::conditional_t<
              std::is_lvalue_reference<PopHandler>::value
            , std::decay_t<PopHandler>
            , PopHandler
            >;

        CopyIfLValueRef handlerCopy = std::forward<PopHandler>(handler);

        // move_binder2 вместо обычного, чтобы не копировать лишний раз val
        boost::asio::detail::move_binder2<
              std::decay_t<PopHandler>
            , boost::system::error_code
            , optional<value_type>
            > binder{
                  0
                , std::move(handlerCopy)
                , ec
                , std::move(val)
                };

        // Сообщаем о завершении извлечения обязательно через post,
        // выполнение хендлеров асинхронных операций запрещено исполнять внутри функций-инициаторов.
        boost::asio::post(m_ex, std::move(binder));
    }

    // Выполняет вставку, если ec == 0, затем уведомляет о завершении (ec != 0 при отмене).
    template <typename U, typename PushHandler>
    void doPush(U&& val, PushHandler&& handler)
    {
        m_queue.push(std::forward<U>(val));
        completePush(std::forward<PushHandler>(handler), boost::system::error_code{});
    }

    // Уведомляет о завершении извлечения (ec ==0) или отмене (ec != 0), затем извлекает (при ec == 0).
    template <typename PopHandler>
    void doPop(PopHandler&& handler)
    {
        completePop(std::move(m_queue.front()), std::forward<PopHandler>(handler), boost::system::error_code{});
        m_queue.pop();
    }

    template <typename Handler, typename U>
    void deferPush(Handler&& handler, U&& val)
    {
        auto work = boost::asio::make_work_guard(m_ex);
        m_pendingPushQueue.emplace(
            PendingPushOp<std::decay_t<Handler>>{
                  std::forward<U>(val)
                , std::forward<Handler>(handler)
                , std::move(work)
                }
            );
    }

    template <typename Handler>
    void deferPop(Handler&& handler)
    {
        auto work = boost::asio::make_work_guard(m_ex);
        m_pendingPopQueue.emplace(
            PendingPopOp<std::decay_t<Handler>>{
                  std::forward<Handler>(handler)
                , std::move(work)
                }
            );
    }

    // Выполняет отложенную вставку.
    void doPendingPush(const boost::system::error_code& ec = {})
    {
        m_pendingPushQueue.front()(*this, ec);
        m_pendingPushQueue.pop();
    }

    // Выполняет отложенное извлечение.
    void doPendingPop(const boost::system::error_code& ec = {})
    {
        m_pendingPopQueue.front()(*this, ec);
        m_pendingPopQueue.pop();
    }

    void checkInvariant() const
    {
        assert(m_queue.size() <= m_limit);
        assert(m_queue.empty() || m_pendingPopQueue.empty());
        assert(m_queue.size() == m_limit || m_pendingPushQueue.empty());
    }

private:
    mutable std::recursive_mutex m_mutex;
    executor_type m_ex;
    std::size_t m_limit = 0;
    container_type m_queue;

    // Здесь хранятся ожидающие операции вставки, когда очередь переолнена.
    std::queue<
        detail::Function<void(Queue&, const boost::system::error_code&)>
        > m_pendingPushQueue;

    // Здесь хранятся ожидающие операции извлечения, когда очередь пуста.
    std::queue<
        detail::Function<void(Queue&, const boost::system::error_code&)>
        > m_pendingPopQueue;
};


// Обертка над std::unique_lock.
// Не только лочит мьютекс, но и проверяет инвариант Queue в конструкторе и деструкторе.
template <typename Elem, typename Executor, typename Container>
class Queue<Elem, Executor, Container>::LockGuard
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


// Отложенная операция извлечения.
// В отличие от лямбды выставляет нужный нам метод get_allocator().
template <typename Elem, typename Executor, typename Container>
template <typename Handler>
class Queue<Elem, Executor, Container>::PendingPopOp
{
public:
    PendingPopOp(PendingPopOp&&) = default;

    template <typename H>
    PendingPopOp(H&& handler, boost::asio::executor_work_guard<executor_type>&& work)
        : m_handler{ std::forward<H>(handler) }, m_work{ std::move(work) }
    {
    }

    using allocator_type = boost::asio::associated_allocator_t<Handler>;
    allocator_type get_allocator() const
    {
        return boost::asio::associated_allocator<Handler>::get(m_handler);
    }

    void operator()(Queue& self, const boost::system::error_code& ec)
    {
        if (ec) // Если отмена, то только уведомляем об отмене ожидающего извлечения.
            self.completePop(optional<value_type>{}, std::move(m_handler), ec);
        else // Иначе извлекаем как обычно.
            self.doPop(std::move(m_handler));
    }

protected:
    Handler m_handler;
    // work нужен, чтобы функтор держал executor и предотвращал от выхода из run,
    // пока отложенная операция не будет выполнена.
    // В этот момент в очереди asio executor может быть и пусто,
    // но по логике Queue имеет отложенную операцию, до исполнения которой run должен крутиться.
    boost::asio::executor_work_guard<executor_type> m_work;
};


// Отложенная операция вставки.
// В отличие от лямбды выставляет нужный нам метод get_allocator().
// Отнаследован от PendingPopOp, чтобы не копипастить одно и то же, хранит все то же самое + value_type.
template <typename Elem, typename Executor, typename Container>
template <typename Handler>
class Queue<Elem, Executor, Container>::PendingPushOp
    : public PendingPopOp<Handler>
{
public:
    template <typename U, typename H>
    PendingPushOp(U&& val, H&& handler, boost::asio::executor_work_guard<executor_type>&& work)
        : PendingPopOp<Handler>{ std::forward<H>(handler), std::move(work) }
        , m_val{ std::forward<U>(val) }
    {
    }

    void operator()(Queue& self, const boost::system::error_code& ec)
    {
        if (ec) // Если отмена, то только уведомляем об отмене ожидающей вставки.
            self.completePush(std::move(this->m_handler), ec);
        else // Иначе вставляем как обычно.
            self.doPush(std::move(m_val), std::move(this->m_handler));
    }

private:
    value_type m_val;
};


#if BOOST_VERSION >= 107000
// Обертка-функтор для asio::async_initiate, чтобы не городить лямбду
// и выставить из нее get_executor() непонятно зачем.
template <typename Elem, typename Executor, typename Container>
class Queue<Elem, Executor, Container>::AsyncInit
{
public:
    // В документации asio::async_initiate ничего не сказано о возможной поддержке
    // executor_type / get_executor() в типе инициатора, но тем не менее разработчики asio
    // в своих компонентах выставляют эти имена из соответствующих инициаторов.
    // Выставим и мы, не жалко, поскольку они и так есть у нас.
    using executor_type = typename Queue::executor_type;
    executor_type get_executor() const
    {
        return m_self.get_executor();
    }

    AsyncInit(Queue& self)
        : m_self{ self }
    {
    }

    // Чтобы не копипастить AsyncInit отдельно для push и pop, в одном классе перегрузим 2 operator(),
    // благо они отличаются по количеству аргументов.

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

private:
    Queue& m_self;
};
#endif // BOOST_VERSION >= 107000

} // namespace async
} // namespace ba
