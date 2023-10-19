#pragma once

#include <type_traits>
#include <queue>
#include <functional>
#include <mutex>

#include <boost/optional/optional.hpp>
#include <boost/system/error_code.hpp>
#include <boost/asio/executor.hpp>
#include <boost/asio/execution_context.hpp>
#include <boost/asio/async_result.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/detail/bind_handler.hpp> // "preserved_binder.h"
#include <boost/core/noncopyable.hpp>


namespace ba {
namespace async {

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

    explicit Queue(const executor_type& ex, std::size_t limit)
        : m_ex{ ex }
        , m_limit{ limit }
    {
        checkInvariant();
    }

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

    Queue(const Queue&) = delete;
    Queue& operator=(const Queue&) = delete;

    Queue(Queue&& other)
        : Queue(std::move(other), LockGuard{ other })
    {
    }

    Queue& operator=(Queue&& other)
    {
        std::unique_lock<std::recursive_mutex> lkThis{ m_mutex, std::defer_lock };
        std::unique_lock<std::recursive_mutex> lkOther{ other.m_mutex, std::defer_lock };
        std::lock(lkThis, lkOther);

        other.checkInvariant();

        reset();
        m_ex = other.m_ex; // оставляем копию
        m_limit = other.m_limit;
        m_queue = std::move(other.m_queue);
        m_pendingPush = std::move(other.m_pendingPush);
        m_pendingPop = std::move(other.m_pendingPop);
        checkInvariant();

        other.reset();

        return *this;
    }

    ~Queue()
    {
        cancel();
    }

    template <typename U, typename PushToken>
    auto asyncPush(U&& val, PushToken&& token)
    {
        static_assert(std::is_convertible<U, value_type>::value, "val must converts to value_type");

        boost::asio::async_completion<
              PushToken
            , void(boost::system::error_code)
            > init{ token };

        static_assert(
              std::is_convertible<
                  decltype(init.completion_handler)
                , std::function<void(const boost::system::error_code&)>
                >::value
            , "Handler signature must be void(const boost::system::error_code&)"
            );

        LockGuard lkGuard{ *this };

        if (m_queue.empty() && !m_pendingPop.empty()) // голодная очередь
        {
            invokePushHandler(init.completion_handler, boost::system::error_code{});

            m_pendingPop.front()(
                  *this
                , boost::system::error_code{}
                , boost::make_optional(std::forward<U>(val))
                );
            m_pendingPop.pop();

            lkGuard.unlock();
            return init.result.get();
        }

        if (m_queue.size() < m_limit)
        {
            m_queue.push(std::forward<U>(val));
            lkGuard.unlock();

            invokePushHandler(init.completion_handler, boost::system::error_code{});

            return init.result.get();
        }

        // Превышен лимит, откладываем
        auto work = boost::asio::make_work_guard(m_ex);

        m_pendingPush.push([
              val{ std::forward<U>(val) }
            , handler{ init.completion_handler }
            , work{ std::move(work) }
            ](
              Queue& self
            , const boost::system::error_code& ec
            ) mutable {
                if (!ec)
                    self.m_queue.push(std::move(val));

                self.invokePushHandler(std::move(handler), ec);
            });

        lkGuard.unlock();
        return init.result.get();
    }

    template <typename PopToken>
    auto asyncPop(PopToken&& token)
    {
        boost::asio::async_completion<
              PopToken
            , void(boost::system::error_code, boost::optional<value_type>&&)
            > init{ token };

        static_assert(
              std::is_convertible<
                  decltype(init.completion_handler)
                , std::function<void(const boost::system::error_code&, boost::optional<value_type>&&)>
                >::value
            , "Handler signature must be void(const boost::system::error_code&, boost::optional<T>&&)"
            );

        LockGuard lkGuard{ *this };

        if (m_queue.empty() && !m_pendingPush.empty())
        {
            m_pendingPush.front()(*this, boost::system::error_code{});
            m_pendingPush.pop();

            invokePopHandler(
                  init.completion_handler
                , boost::system::error_code{}
                , std::move(m_queue.front())
                );
            m_queue.pop();

            lkGuard.unlock();
            return init.result.get();
        }

        if (m_queue.empty())
        {
            auto work = boost::asio::make_work_guard(m_ex);
            m_pendingPop.push([
                  handler{ init.completion_handler }
                , work{ std::move(work) }
                ](
                  Queue& self
                , const boost::system::error_code& ec
                , boost::optional<value_type>&& val
                ) mutable {
                    self.invokePopHandler(std::move(handler), ec, std::move(val));
                });

            lkGuard.unlock();
            return init.result.get();
        }

        invokePopHandler(
              init.completion_handler
            , boost::system::error_code{}
            , std::move(m_queue.front())
            );
        m_queue.pop();

        if (!m_pendingPush.empty())
        {
            m_pendingPush.front()(*this, boost::system::error_code{});
            m_pendingPush.pop();
        }

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

    std::size_t cancelOnePush()
    {
        LockGuard lkGuard{ *this };

        if (m_pendingPush.empty())
            return 0;

        m_pendingPush.front()(*this, boost::asio::error::operation_aborted);
        m_pendingPush.pop();

        return 1;
    }

    std::size_t cancelPush()
    {
        LockGuard lkGuard{ *this };

        std::size_t n = 0;
        while (cancelOnePush())
            ++n;

        return n;
    }

    std::size_t cancelOnePop()
    {
        LockGuard lkGuard{ *this };

        if (m_pendingPop.empty())
            return 0;

        m_pendingPop.front()(*this, boost::asio::error::operation_aborted, boost::none);
        m_pendingPop.pop();

        return 1;
    }

    std::size_t cancelPop()
    {
        LockGuard lkGuard{ *this };

        std::size_t n = 0;
        while (cancelOnePop())
            ++n;

        return n;
    }

    std::size_t cancel()
    {
        LockGuard lkGuard{ *this };

        std::size_t n = cancelPush(); // сначала отменяем ждущие push
        return n + cancelPop(); // потом pop, пусть будет определен такой порядок для пользователя
    }

    std::size_t reset()
    {
        LockGuard lkGuard{ *this };

        container_type empty;
        std::swap(m_queue, empty); // clear
        return cancel();
    }

private:
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
        : m_ex{ other.m_ex } // оставляем копию
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
        , boost::optional<value_type>&& val
        )
    {
        auto handlerCopy = std::forward<PopHandler>(handler);

        boost::asio::detail::move_binder2<
              decltype(handlerCopy)
            , boost::system::error_code
            , boost::optional<value_type>
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
    executor_type m_ex;
    std::size_t m_limit = 0;
    container_type m_queue;

    std::queue<
        std::function<void(Queue&, const boost::system::error_code&)>
        > m_pendingPush;

    std::queue<
        std::function<void(Queue&, const boost::system::error_code&, boost::optional<value_type>&&)>
        > m_pendingPop;

    mutable std::recursive_mutex m_mutex;
};

} // namespace async
} // namespace ba
