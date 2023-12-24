#pragma once

#include "queue/value_factory.hpp"
#include "queue/error.hpp"
#include "queue/detail/function_queue.hpp"
#include "queue/detail/check_callable.hpp"
#include "queue/detail/associated_binder.hpp"
#include "queue/detail/compressed_pair.hpp"

#include <type_traits>
#include <queue>
#include <mutex>

#include <boost/system/error_code.hpp>
#include <boost/asio/executor.hpp>
#include <boost/asio/execution_context.hpp>
#include <boost/asio/async_result.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/post.hpp>

// Библиотека использует недокументированный boost::asio::detail::binder и move_binder
// Они широко используется во внутренностях asio, и, вероятно, его поддержка не иссякнет.
// Предпочтительнее использовать его, т.к. его поддерживают и могут добавить для него новые traits.
// Но, на случай, если он пропадет в будущих версиях boost,
// есть самописный аналог в .detail/preserved_binder.hpp, перед его использованием проверьте, что для него
// специализированы все нужные и современные traits, такие как boost::asio::associated_executor и т.д.
#include <boost/asio/detail/bind_handler.hpp>


namespace ba {
namespace async {

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
        m_closeState = other.m_closeState;
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
     * При отмене ожидаемой операции error == QueueError::OPERATION_CANCELLED.
     * При закрытии очереди error == QueueError::QUEUE_CLOSED.
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
        static_assert(std::is_convertible<U, value_type>::value, "'val' must converts to 'Elem' type");

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
     * @param token должен порождать хендлер или быть хендлером с сигнатурой:
     * @code void handler(
     *       const boost::system::error_code& error // результат операции
     *     , Elem value // извлеченное значение
     * ); @endcode
     * :)
     * Если очередь пуста, извлечение элемента и вызов handler произойдет
     * после очередного вызова @ref asyncPush.
     * При отмене ожидаемой операции error == QueueError::OPERATION_CANCELLED и value == Elem{}.
     * При закрытой и пустой очереди error == QueueError::QUEUE_CLOSED и value == Elem{}.
     */
    template <typename PopToken, typename F = ValueFactory<value_type>>
    auto asyncPop(PopToken&& token, F&& defValueFactory = F{})
    {
        // Важно: захват мьютекса в этом методе делать нельзя,
        // Вконце asio::async_initiate или init.result.get() происходит суспенд корутины
        // (если вызов происходит на ней).
        // Вместо этого мьютекс лочится в initPop.

#if BOOST_VERSION >= 107000
        return boost::asio::async_initiate<PopToken, void(boost::system::error_code, value_type)>(
            AsyncInit{ *this }, token, std::forward<F>(defValueFactory), 0
            );
#else
        boost::asio::async_completion<
              PopToken
            , void(boost::system::error_code, value_type)
            > init{ token };

        initPop(init.completion_handler, std::forward<F>(defValueFactory));
        return init.result.get();
#endif
    }

    /// Пытается синхронно вставить элемент, если это возможно.
    /// Возвращает true при успехе.
    /// Возвращает false, когда возможна только асинхронная вставка с ожиданием.
    /**
     * @param val элемент
     */
    template <typename U>
    bool tryPush(U&& val)
    {
        LockGuard lkGuard{ *this };
        if (!readyPush() || m_closeState)
            return false;

        doPush(std::forward<U>(val));
        doPendingPop();

        return true;
    }

    /// Пытается синхронно извлечь элемент.
    /// Возвращает элемент при успехе.
    /// Возвращает Elem{}, когда возможно только асинхронное извлечение с ожиданием.
    /**
     * @param success - ссылка, по которой записывается флаг успешности операции.
     */
    template <typename F = ValueFactory<value_type>>
    value_type tryPop(bool& success, F&& defValueFactory = F{})
    {
        LockGuard lkGuard{ *this };

        if (!doPendingPush() && !readyPop())
        {
            success = false;
            return std::forward<F>(defValueFactory)(QueueError::QUEUE_EMPTY);
        }

        value_type result = std::move(m_queue.front());
        doPop();

        success = true;
        return result;
    }

    /// Пытается синхронно извлечь элемент.
    /// Возвращает элемент при успехе.
    /// Возвращает Elem{}, когда возможно только асинхронное извлечение с ожиданием.
    template <typename F = ValueFactory<value_type>>
    value_type tryPop(F&& defValueFactory = F{})
    {
        bool ignoreSuccess = false;
        return tryPop(ignoreSuccess, std::forward<F>(defValueFactory));
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
        return doPendingPush(QueueError::OPERATION_CANCELLED);
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
        return doPendingPop(QueueError::OPERATION_CANCELLED);
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
        return doCancel(QueueError::OPERATION_CANCELLED);
    }

    /// Очищает очередь. Отменяет все ожидающие операции.
    void reset()
    {
        LockGuard lkGuard{ *this };

        // У std::queue не clear :)
        m_queue = container_type{};
        doCancel(QueueError::OPERATION_CANCELLED);
        m_closeState.clear();
    }

    /// Закрывает очередь для последующей вставки, отменяет все отложенные операции.
    /// Извлечение будет успешно вплоть до опустошения очереди, дальнейшее извлечение приведет к ошибке.
    /// Если передали пустой код ошибки, то возвращает false и ничего не делает, иначе true.
    bool close(const boost::system::error_code& ec = QueueError::QUEUE_CLOSED)
    {
        // Закрываем только с кодом ошибки.
        if (!ec)
            return;

        LockGuard lkGuard{ *this };
        m_closeState = ec;
        doCancel(m_closeState);
    }

    /// Возвращает код ошибки, с которым была закрыта очередь.
    /// После создания очереди или после вызова reset возвращает пустой код.
    /// После вызова close(ec) возвращает ec.
    boost::system::error_code closeState() const
    {
        LockGuard lkGuard{ *this };
        return m_closeState;
    }

    /// Возвращает !closeState().
    bool isOpen() const
    {
        LockGuard lkGuard{ *this };
        return !m_closeState;
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
        , m_closeState{ other.m_closeState }
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
        // Лимит не превышен, или есть ожидающие извлечения из очереди с нулевым лимитом.
        return m_queue.size() < m_limit || (0 == m_limit && hasPendingPop());
    }

    bool readyPop()
    {
        // Очередь непуста, или есть ожидающие вставки в очередь с нулевым лимитом.
        return !m_queue.empty() || (0 == m_limit && hasPendingPush());
    }

    // Инициатор вставки.
    template <typename U, typename PushHandler>
    void initPush(U&& val, PushHandler&& handler)
    {
        // Чтобы пользователь получил вменяемую ошибку вместо портянки при ошибке в типе хендлера.
        static_assert(
              detail::CheckCallable<
                  decltype(handler)
                , void(const boost::system::error_code&)
                >::value
            , "Handler must support call: 'handler(boost::system::error_code{})'."
            );

        // Именно тут блокируемся, а не в asyncPush.
        LockGuard lkGuard{ *this };

        // Если очередь закрыта, завершаем с ошибкой, в закрытую очередь не вставляем.
        if (m_closeState)
        {
            completePush(std::forward<PushHandler>(handler), m_closeState);
            return;
        }

        if (readyPush())
        {
            doAsyncPush(std::forward<U>(val), std::forward<PushHandler>(handler));
            doPendingPop();
        }
        else
        {
            deferPush(std::forward<PushHandler>(handler), std::forward<U>(val));
        }
    }

    // Инициатор извлечения.
    template <typename PopHandler, typename F>
    void initPop(PopHandler&& handler, F&& defValueFactory)
    {
        // Чтобы пользователь получил вменяемую ошибку вместо портянки при ошибке в типе хендлера.
        static_assert(
            detail::CheckCallable<
                  decltype(handler)
                , void(const boost::system::error_code&, value_type)
                >::value
            , "Handler must support call: 'handler(boost::system::error_code{}, Elem{})'."
            );

        // Именно тут блокируемся, а не в asyncPop.
        LockGuard lkGuard{ *this };

        // Сначала пытаемся выполнить отложенную вставку, если есть.
        if (doPendingPush() || readyPop())
        {
            doAsyncPop(std::forward<PopHandler>(handler));
            return;
        }

        // Извлекать нечего, откладываем при открытой очереди или завершаем с ошибкой при закрытой.
        if (!m_closeState)
            deferPop(std::forward<PopHandler>(handler), std::forward<F>(defValueFactory));
        else
            completePop(
                  std::forward<PopHandler>(handler)
                , m_closeState
                , std::forward<F>(defValueFactory)(m_closeState)
                );
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
        // Копируем, если lvalue-ссылка || const rvalue-ссылка, иначе форвардим на перемещение (rvalue).
        using CopyIfNotMovable = std::conditional_t<
              std::is_lvalue_reference<PopHandler>::value || std::is_const<PopHandler>::value
            , std::decay_t<PopHandler>
            , PopHandler&&
            >;

        CopyIfNotMovable h = std::forward<PopHandler>(handler);

        // move_binder2 вместо обычного, чтобы не копировать лишний раз val
        boost::asio::detail::move_binder2<
              std::decay_t<PopHandler>
            , boost::system::error_code
            , U
            > binder{
                  0
                , std::move(h)
                , ec
                , std::forward<U>(val)
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

    template <typename PushHandler, typename U>
    void deferPush(PushHandler&& handler, U&& val)
    {
        assert(!hasPendingPop());

        // work нужен, чтобы функтор держал executor и предотвращал от выхода из run,
        // пока отложенная операция не будет выполнена.
        // В этот момент в очереди asio executor может быть и пусто,
        // но по логике Queue имеет отложенную операцию, до исполнения которой run должен крутиться.
        auto work = boost::asio::make_work_guard(m_ex);

        m_pendingOps.push(detail::bindAssociated(
            [work{ std::move(work) }, val{ value_type(std::forward<U>(val)) }]
            (PushHandler& h, Queue& self, const boost::system::error_code& ec) mutable
            {
                if (ec) // Если отмена, то только уведомляем об отмене ожидающей вставки.
                    self.completePush(std::move(h), ec);
                else
                    self.doAsyncPush(std::move(val), std::move(h));
            }
            , std::forward<PushHandler>(handler))
        );
        m_pendingOpsIsPushers = true;

        assert(!hasPendingPop() && hasPendingPush());
    }

    template <typename PopHandler, typename F>
    void deferPop(PopHandler&& handler, F&& defValueFactory)
    {
        assert(!hasPendingPush());

        // work нужен, чтобы функтор держал executor и предотвращал от выхода из run,
        // пока отложенная операция не будет выполнена.
        // В этот момент в очереди asio executor может быть и пусто,
        // но по логике Queue имеет отложенную операцию, до исполнения которой run должен крутиться.
        auto capture = detail::makeCompressedPair(
              std::forward<F>(defValueFactory)
            , boost::asio::make_work_guard(m_ex)
            );

        m_pendingOps.push(detail::bindAssociated(
            [capture{ std::move(capture) }]
            (std::decay_t<PopHandler>& h, Queue& self, const boost::system::error_code& ec) mutable
            {
                F& defValueFactory = capture.getEmpty();
                if (ec)
                    self.completePop(std::move(h), ec, std::move(defValueFactory)(ec));
                else
                    self.doAsyncPop(std::move(h));
            }
            , std::forward<PopHandler>(handler))
        );
        m_pendingOpsIsPushers = false;

        assert(!hasPendingPush() && hasPendingPop());
    }

    // Если есть отложенная вставка,то выполняет ее и возвращает true, иначе возвращает false.
    bool doPendingPush(const boost::system::error_code& ec = {})
    {
        if (!hasPendingPush())
            return false;

        assert(hasPendingPush() && !hasPendingPop());
        m_pendingOps.pop(*this, ec);
        assert(!hasPendingPop());

        return true;
    }

    // Если есть отложенное извлечение,то выполняет его и возвращает true, иначе возвращает false.
    bool doPendingPop(const boost::system::error_code& ec = {})
    {
        if (!hasPendingPop())
            return false;

        assert(hasPendingPop() && !hasPendingPush());
        m_pendingOps.pop(*this, ec);
        assert(!hasPendingPush());

        return true;
    }

    std::size_t doCancel(const boost::system::error_code& ec)
    {
        std::size_t n = 0;
        for (; !m_pendingOps.empty(); ++n)
            m_pendingOps.pop(*this, ec);

        assert(!hasPendingPush() && !hasPendingPop());
        return n;
    }

    void checkInvariant() const
    {
        assert(m_queue.size() <= m_limit);
        assert(m_queue.size() == m_limit || !hasPendingPush());
        assert(m_queue.empty() || !hasPendingPop());
        assert(!m_closeState || m_pendingOps.empty());
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
    boost::system::error_code m_closeState;
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

    explicit AsyncInit(Queue& self) noexcept
        : m_self{ self }
    {
    }

    // Чтобы не копипастить AsyncInit отдельно для push и pop, в одном классе перегрузим 2 operator(),
    // для отличия сигнатур для pop добавлен фиктивный аргумент int.

    template <typename PushHandler, typename U>
    void operator()(PushHandler&& handler, U&& val) const
    {
        m_self.initPush(std::forward<U>(val), std::forward<PushHandler>(handler));
    }

    template <typename PopHandler, typename F>
    void operator()(PopHandler&& handler, F&& defValueFactory, int) const
    {
        m_self.initPop(std::forward<PopHandler>(handler), std::forward<F>(defValueFactory));
    }

private:
    Queue& m_self;
};
#endif // BOOST_VERSION >= 107000

} // namespace async
} // namespace ba
