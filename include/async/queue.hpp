#pragma once

#include "queue/detail/function_queue.hpp"
#include "queue/detail/check_callable.hpp"
#include "queue/detail/associated_binder.hpp"

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
    using Optional = boost::optional<T>;
    using NullOptT = boost::none_t;
    inline constexpr const boost::none_t& nullOpt = boost::none;
#else
    template <typename T>
    using Optional = std::optional<T>;
    using NullOptT = std::nullopt_t;
    inline constexpr const std::nullopt_t& nullOpt = std::nullopt;
#endif


/// Асинхронная очередь с ограничением длины (минимум 0).
/**
 * Потокобезопасная.
 * Вызывает хендлер завершения после постановки или получения элемента.
 */
template <
      typename Elem
    , typename Executor = boost::asio::executor
    , typename HandlerDefaultAllocator = std::allocator<unsigned char>
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
     * Для размещения отложенных операций использует ассоциированные с handler'ами аллокаторы.
     * Когда с handler не ассоциирован аллокатор, используется handlerDefAlloc.
     */
    explicit Queue(
          const executor_type& ex
        , std::size_t limit
        , const HandlerDefaultAllocator& handlerDefAlloc = HandlerDefaultAllocator{}
        )
        : m_ex{ ex }
        , m_limit{ limit }
        , m_pendingOps{ handlerDefAlloc }
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
        , const HandlerDefaultAllocator& handlerDefAlloc = HandlerDefaultAllocator{}
        , typename std::enable_if_t<
            std::is_convertible<ExecutionContext&, boost::asio::execution_context&>::value
            >* = 0
        )
        : Queue{ context.get_executor(), limit, handlerDefAlloc }
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
        if (this == &other)
            return *this;

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
        m_pendingOps = std::move(other.m_pendingOps);
        m_pendingOpsIsPushers = other.m_pendingOpsIsPushers;
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
     *     , Optional<Elem> value // извлеченное значение
     * ); @endcode
     * :)
     * Если очередь пуста, извлечение элемента и вызов handler произойдет
     * после очередного вызова @ref asyncPush.
     * При отмене ожидаемой операции error == boost::asio::error::operation_aborted и
     * value == Optional<Elem>{}
     */
    template <typename PopToken>
    auto asyncPop(PopToken&& token)
    {
        // Важно: захват мьютекса в этом методе делать нельзя,
        // Вконце asio::async_initiate или init.result.get() происходит суспенд корутины
        // (если вызов происходит на ней).
        // Вместо этого мьютекс лочится в initPop.

#if BOOST_VERSION >= 107000
        return boost::asio::async_initiate<PopToken, void(boost::system::error_code, Optional<value_type>)>(
            AsyncInit{ *this }, token
            );
#else
        boost::asio::async_completion<
              PopToken
            , void(boost::system::error_code, Optional<value_type>)
            > init{ token };

        initPop(init.completion_handler);
        return init.result.get();
#endif
    }

    template <typename U>
    bool tryPush(U&& val)
    {
        LockGuard lkGuard{ *this };
        if (!readyPush())
            return false;

        doPush(std::forward<U>(val));
        doPendingPop();

        return true;
    }

    Optional<value_type> tryPop()
    {
        LockGuard lkGuard{ *this };
        Optional<value_type> result;

        if (!doPendingPush() && !readyPop())
            return result;

        result.emplace(std::move(m_queue.front()));
        doPop();

        return result;
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
        return doPendingPush(boost::asio::error::operation_aborted);
    }

    /// Отменяет все ожидающие операции вставки и возвращяет их количество.
    std::size_t cancelPush()
    {
        LockGuard lkGuard{ *this };

        if (!m_pendingOpsIsPushers)
            return 0;

        return cancel();
    }

    /// Отменяет одну ожидающую операцию извлечения и возвращяет их количество (0 или 1).
    std::size_t cancelOnePop()
    {
        LockGuard lkGuard{ *this };
        return doPendingPop(boost::asio::error::operation_aborted);
    }

    /// Отменяет все ожидающие операции извлечения и возвращяет их количество.
    std::size_t cancelPop()
    {
        LockGuard lkGuard{ *this };

        if (m_pendingOpsIsPushers)
            return 0;

        return cancel();
    }

    /// Отменяет все ожидающие операции вставки и извлечения и возвращяет их количество.
    std::size_t cancel()
    {
        LockGuard lkGuard{ *this };

        std::size_t n = 0;
        for (; !m_pendingOps.empty(); ++n)
            m_pendingOps.pop(*this, boost::asio::error::operation_aborted);

        assert(!hasPendingPush() && !hasPendingPop());
        return n;
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
        , m_pendingOps{ std::move(other.m_pendingOps) }
        , m_pendingOpsIsPushers{ other.m_pendingOpsIsPushers }
    {
    }

    bool hasPendingPush() const noexcept
    {
        return m_pendingOpsIsPushers && !m_pendingOps.empty();
    }

    bool hasPendingPop() const noexcept
    {
        return !m_pendingOpsIsPushers && !m_pendingOps.empty();
    }

    bool readyPush()
    {
        return m_queue.size() < m_limit || 0 == m_limit && hasPendingPop();

        //m_queue.emplace(std::forward<U>(val));

        // Если есть ожидающие извлечения, выполняем извлечение.
        //if (hasPendingPop_)
        //{
            // Если ждали извлечения, значит до этого уперлись в size == 0, и теперь он стал равен 1.
            // При m_limit == 0, в очереди кратковременно появится 1 элемент, но снаружи это не заметно.
        //    assert(m_queue.size() == 1);
        //    doPendingPop();
        //}
    }

    bool readyPop()
    {
        //LockGuard lkGuard{ *this };

        // Если очередь не пуста, то извлекаем, иначе откладываем.
        //if (hasPendingPush())
        //{
        //    doPendingPush();
            // Если ждали вставки, значит до этого уперлись в лимит,
            // и теперь он будет кратковременно превышен на 1 элемент, но снаружи это не заметно.
            //assert(m_queue.size() == m_limit + 1);
        //}

        return !m_queue.empty() || 0 == m_limit && hasPendingPush();

        //Optional<value_type> val = std::move(m_queue.front());
        //m_queue.pop();
        //return true;// val;
    }

    // Инициатор вставки.
    template <typename U, typename PushHandler>
    void initPush(U&& val, PushHandler&& handler)
    {
        // Чтобы пользователь получил вменяемую ошибку вместо портянки при ошибке в типе хендлера.
        static_assert(
              async::detail::CheckCallable<
                  decltype(handler)
                , void(const boost::system::error_code&)
                >::value
            , "Handler must support call: 'handler(boost::system::error_code{})'."
            );

        // Именно тут блокируемся, а не в asyncPush.
        LockGuard lkGuard{ *this };

        // Если лимит не превышен или есть ожидающие извлечения, то вставляем, иначе откладываем.
        if (readyPush())
        {
            doAsyncPush(std::forward<U>(val), std::forward<PushHandler>(handler));
            doPendingPop();
        }
        else
            deferPush(std::forward<PushHandler>(handler), std::forward<U>(val));
    }

    // Инициатор извлечения.
    template <typename PopHandler>
    void initPop(PopHandler&& handler)
    {
        // Чтобы пользователь получил вменяемую ошибку вместо портянки при ошибке в типе хендлера.
        static_assert(
            detail::CheckCallable<
                  decltype(handler)
                , void(const boost::system::error_code&, value_type)
                >::value &&
            detail::CheckCallable<
                  decltype(handler)
                , void(const boost::system::error_code&, NullOptT)
                >::value
            , "Handler must support two calls: 'handler(boost::system::error_code{}, Elem{})' and "
              "'handler(boost::system::error_code{}, nullOpt)' "
              "(you can simply use one overload with Optional<Elem> as second argument)."
            );

        // Именно тут блокируемся, а не в asyncPop.
        LockGuard lkGuard{ *this };

        // Если очередь не пуста, то извлекаем, иначе откладываем.
        if (doPendingPush() || readyPop())
            doAsyncPop(std::forward<PopHandler>(handler));
        else
            deferPop(std::forward<PopHandler>(handler));
    }

    // Комплитер вставки.
    template <typename PushHandler>
    void completePush(PushHandler&& handler, const boost::system::error_code& ec)
    {
        // Сообщаем о завершении вставки обязательно через post,
        // выполнение хендлеров асинхронных операций запрещено исполнять внутри функций-инициаторов.
        boost::asio::post(
              m_ex
            , boost::asio::detail::bind_handler(std::forward<PushHandler>(handler), ec)
            );
    }

    // Комплитер извлечения.
    template <typename PopHandler, typename U>
    void completePop(PopHandler&& handler, const boost::system::error_code& ec, U&& val)
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
            , std::decay_t<U>
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

    template <typename U>
    void doPush(U&& val)
    {
        assert(m_queue.size() <= m_limit);
        m_queue.emplace(std::forward<U>(val));
    }
    
    // Выполняет вставку, если ec == 0, затем уведомляет о завершении (ec != 0 при отмене).
    template <typename U, typename PushHandler>
    void doAsyncPush(U&& val, PushHandler&& handler)
    {
        doPush(std::forward<U>(val));
        completePush(std::forward<PushHandler>(handler), boost::system::error_code{});
    }

    void doPop()
    {
        assert(!m_queue.empty());
        m_queue.pop();
    }
    
    // Уведомляет о завершении извлечения (ec ==0) или отмене (ec != 0), затем извлекает (при ec == 0).
    template <typename PopHandler>
    void doAsyncPop(PopHandler&& handler)
    {
        assert(!m_queue.empty());
        completePop(
              std::forward<PopHandler>(handler)
            , boost::system::error_code{}
            , std::move(m_queue.front())
            );
        doPop();
    }

    template <typename Handler, typename U>
    void deferPush(Handler&& handler, U&& val)
    {
        assert(!hasPendingPop());

        // work нужен, чтобы функтор держал executor и предотвращал от выхода из run,
        // пока отложенная операция не будет выполнена.
        // В этот момент в очереди asio executor может быть и пусто,
        // но по логике Queue имеет отложенную операцию, до исполнения которой run должен крутиться.
        auto work = boost::asio::make_work_guard(m_ex);

        m_pendingOps.push(detail::bindAssociated(
            [work{ std::move(work) }, val{ value_type(std::forward<U>(val)) }]
            (Handler& h, Queue& self, const boost::system::error_code& ec) mutable
            {
                if (ec) // Если отмена, то только уведомляем об отмене ожидающей вставки.
                    self.completePush(std::move(h), ec);
                else
                    self.doAsyncPush(std::move(val), std::move(h));
            }
            , std::move(handler))
        );
        m_pendingOpsIsPushers = true;

        assert(!hasPendingPop() && hasPendingPush());
    }

    template <typename Handler>
    void deferPop(Handler&& handler)
    {
        assert(!hasPendingPush());

        // work нужен, чтобы функтор держал executor и предотвращал от выхода из run,
        // пока отложенная операция не будет выполнена.
        // В этот момент в очереди asio executor может быть и пусто,
        // но по логике Queue имеет отложенную операцию, до исполнения которой run должен крутиться.
        auto work = boost::asio::make_work_guard(m_ex);

        m_pendingOps.push(detail::bindAssociated(
            [work{ std::move(work) }]
            (Handler& h, Queue& self, const boost::system::error_code& ec) mutable
            {
                if (ec)
                    self.completePop(std::move(h), ec, NullOptT(nullOpt));
                else
                    self.doAsyncPop(std::move(h));
            }
            , std::move(handler))
        );
        m_pendingOpsIsPushers = false;

        assert(!hasPendingPush() && hasPendingPop());
    }

    // Выполняет отложенную вставку.
    bool doPendingPush(const boost::system::error_code& ec = {})
    {
        if (!hasPendingPush())
            return false;

        assert(hasPendingPush() && !hasPendingPop());
        m_pendingOps.pop(*this, ec);
        assert(!hasPendingPop());

        return true;
    }

    // Выполняет отложенное извлечение.
    bool doPendingPop(const boost::system::error_code& ec = {})
    {
        if (!hasPendingPop())
            return false;

        assert(hasPendingPop() && !hasPendingPush());
        m_pendingOps.pop(*this, ec);
        assert(!hasPendingPush());

        return true;
    }

    void checkInvariant() const
    {
        assert(m_queue.size() <= m_limit);
        assert(m_queue.size() == m_limit || !hasPendingPush());
        assert(m_queue.empty() || !hasPendingPop());
    }

private:
    mutable std::recursive_mutex m_mutex;
    executor_type m_ex;
    std::size_t m_limit = 0;
    container_type m_queue;

    // Когда очередь переполнена, здесь хранятся отложенные операции вставки.
    // Когда очерель пустая, и хотят извлечь, здесь хранятся отложенные операции извлечения.
    detail::FunctionQueue<
          void(Queue&, const boost::system::error_code&)
        , HandlerDefaultAllocator
        > m_pendingOps;

    // Флажок, который указывает, что именно хранится в очереди: отложенные вставки или извлечения.
    bool m_pendingOpsIsPushers = false;
};


// Обертка над std::unique_lock.
// Не только лочит мьютекс, но и проверяет инвариант Queue в конструкторе и деструкторе.
template <
      typename Elem
    , typename Executor
    , typename HandlerDefaultAllocator
    , typename Container
    >
class Queue<
      Elem
    , Executor
    , HandlerDefaultAllocator
    , Container
    >::LockGuard
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
// Обертка-функтор для asio::async_initiate, чтобы не городить лямбду
// и выставить из нее get_executor() непонятно зачем.
template <
      typename Elem
    , typename Executor
    , typename HandlerDefaultAllocator
    , typename Container
    >
class Queue<
      Elem
    , Executor
    , HandlerDefaultAllocator
    , Container
    >::AsyncInit
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
