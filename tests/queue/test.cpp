#define BOOST_TEST_MODULE LibAsync

#include <async/queue/queue.hpp>

#include <thread>
#include <atomic>
#include <vector>

#include <boost/test/included/unit_test.hpp>
#include <boost/asio/use_future.hpp>

#define BOOST_COROUTINES_NO_DEPRECATION_WARNING
#include <boost/asio/spawn.hpp>


class ThreadPool
{
public:
    ThreadPool(boost::asio::io_context&ioc, std::size_t n)
    {
        while (n--)
            m_threads.emplace_back([&ioc]() { ioc.run(); });
    }

    void join()
    {
        for (auto& th : m_threads)
            th.join();
    }

private:
    std::vector<std::thread> m_threads;
};

BOOST_AUTO_TEST_CASE(strandTest)
{
    boost::asio::io_context ioc;
    // Новый strand.
    boost::asio::strand<boost::asio::io_context::executor_type> s1{ ioc.get_executor() };
    // Deprecated strand.
    boost::asio::io_context::strand s2{ ioc };

    ba::async::Queue<int> q0{ ioc, 10 };

    q0.asyncPush(123, boost::asio::bind_executor(s1, [s1](boost::system::error_code ec) {
        BOOST_CHECK(s1.running_in_this_thread());
        }));

    q0.asyncPop(boost::asio::bind_executor(s1, [s1](boost::system::error_code ec, ba::async::optional<int> val) {
        BOOST_CHECK(s1.running_in_this_thread());
        BOOST_CHECK(123 == *val);
        }));

    ThreadPool{ ioc, 10 }.join();

    q0.asyncPush(123, s2.wrap([s2](boost::system::error_code ec) {
        BOOST_CHECK(s2.running_in_this_thread());
        }));

    q0.asyncPop(s2.wrap([s2](boost::system::error_code ec, ba::async::optional<int> val) {
        BOOST_CHECK(s2.running_in_this_thread());
        BOOST_CHECK(123 == *val);
        }));

    ioc.restart();
    ThreadPool{ ioc, 10 }.join();

    ba::async::Queue<int> q1{ s1, 10 };

    q1.asyncPop([s1](boost::system::error_code ec, ba::async::optional<int> val) {
        BOOST_CHECK(s1.running_in_this_thread());
        BOOST_CHECK(123 == *val);
        });

    q1.asyncPush(123, [s1](boost::system::error_code ec) {
        BOOST_CHECK(s1.running_in_this_thread());
        });

    ioc.restart();
    ThreadPool{ ioc, 10 }.join();

    ba::async::Queue<int> q2{ s2, 10 };

    q2.asyncPop([s2](boost::system::error_code ec, ba::async::optional<int> val) {
        BOOST_CHECK(s2.running_in_this_thread());
        BOOST_CHECK(123 == *val);
        });

    q2.asyncPush(123, [s2](boost::system::error_code ec) {
        BOOST_CHECK(s2.running_in_this_thread());
        });

    ioc.restart();
    ThreadPool{ ioc, 10 }.join();

    // Ничего не осталось в очередях.
    BOOST_CHECK(q0.empty());
    BOOST_CHECK(q1.empty());
    BOOST_CHECK(q2.empty());

    // Не осталось никого в ожидании.
    BOOST_CHECK_EQUAL(0, q0.cancel());
    BOOST_CHECK_EQUAL(0, q1.cancel());
    BOOST_CHECK_EQUAL(0, q2.cancel());
}

BOOST_AUTO_TEST_CASE(futureTest)
{
    boost::asio::io_context ioc;
    ba::async::Queue<int> q{ ioc, 1 };

    std::future<ba::async::optional<int>> fPop = q.asyncPop(boost::asio::use_future);
    std::future<void> fPush = q.asyncPush(123, boost::asio::use_future);

    ThreadPool{ ioc, 10 }.join();

    BOOST_CHECK_NO_THROW(fPush.get());
    BOOST_CHECK_EQUAL(123, *fPop.get());
    BOOST_CHECK(q.empty());
    BOOST_CHECK_EQUAL(0, q.cancel());


    std::future<ba::async::optional<int>> fPopUnderflow = q.asyncPop(boost::asio::use_future);
    BOOST_CHECK_EQUAL(1, q.cancel());

    fPush = q.asyncPush(123, boost::asio::use_future);
    BOOST_CHECK_EQUAL(0, q.cancel());

    std::future<void> fPushOverflow = q.asyncPush(123, boost::asio::use_future);
    BOOST_CHECK_EQUAL(1, q.cancel());

    ioc.restart();
    ThreadPool{ ioc, 10 }.join();

    BOOST_CHECK_NO_THROW(fPush.get());
    BOOST_CHECK_THROW(fPushOverflow.get(), boost::system::system_error);
    BOOST_CHECK_THROW(fPopUnderflow.get(), boost::system::system_error);
    BOOST_CHECK_EQUAL(1, q.size());
    BOOST_CHECK_EQUAL(0, q.cancel());
}

BOOST_AUTO_TEST_CASE(contentTest)
{
    boost::asio::io_context ioc;
    ba::async::Queue<std::size_t> q{ ioc, 10 };

    // Одна корутина толкает.
    boost::asio::spawn(ioc, [&q](boost::asio::yield_context yield) {
        // Вставляем от 1 до 10 000.
        for (std::size_t i = 1; i <= 10'000; ++i)
            q.asyncPush(i, yield);
    });

    // Другая тянет.
    boost::asio::spawn(ioc, [&q](boost::asio::yield_context yield) {
        std::size_t sum = 0;
        // Суммируем все извлеченное.
        for (std::size_t i = 1; i <= 10'000; ++i)
            BOOST_CHECK_NO_THROW(sum += q.asyncPop(yield).value());

        // Проверяем полученную сумму.
        BOOST_CHECK_EQUAL(50005000, sum);
    });

    // Синхронизации между корутинами намерено нет.
    // 10 потоков в пуле.
    ThreadPool{ ioc, 10 }.join();

    BOOST_CHECK(q.empty());
    BOOST_CHECK_EQUAL(0, q.cancel());
}

BOOST_AUTO_TEST_CASE(ManyProducerTest)
{
    boost::asio::io_context ioc;
    ba::async::Queue<std::size_t> q{ioc, 15 };

    // 10 параллельных вставлятелей.
    for (int j = 0; j < 10; ++j)
    {
        boost::asio::spawn(ioc, [&q](boost::asio::yield_context yield) {
            for (std::size_t i = 1; i <= 1'000; ++i)
                q.asyncPush(i, yield);
        });
    }

    // 1 извлекатель.
    boost::asio::spawn(ioc, [&q](boost::asio::yield_context yield) {
        std::size_t sum = 0;
        for (std::size_t i = 1; i <= 10'000; ++i)
            BOOST_CHECK_NO_THROW(sum += q.asyncPop(yield).value());

        BOOST_CHECK_EQUAL(5005000, sum);
    });

    ThreadPool{ ioc, 10 }.join();

    BOOST_CHECK(q.empty());
    BOOST_CHECK_EQUAL(0, q.cancel());
}

BOOST_AUTO_TEST_CASE(manyConsumerTest)
{
    boost::asio::io_context ioc;
    ba::async::Queue<std::size_t> q{ ioc, 15 };

    // 1 вставлятель.
    boost::asio::spawn(ioc, [&q](boost::asio::yield_context yield) {
        for (std::size_t i = 1; i <= 10'000; ++i)
            BOOST_CHECK_NO_THROW(q.asyncPush(i, yield));
    });

    std::atomic_size_t sum;
    sum = 0;

    // 10 параллельных извлекателей.
    for (int j = 0; j < 10; ++j)
    {
        boost::asio::spawn(ioc, [&q, &sum](boost::asio::yield_context yield) {
            for (std::size_t i = 1; i <= 1'000; ++i)
                sum += q.asyncPop(yield).value();
        });
    }

    ThreadPool{ ioc, 10 }.join();
    BOOST_CHECK_EQUAL(50005000, sum);

    BOOST_CHECK(q.empty());
    BOOST_CHECK_EQUAL(0, q.cancel());
}

BOOST_AUTO_TEST_CASE(moveValueTest)
{
    // Проверка, что элементы очереди всегда перемещается, но не копируются.
    // (compile-time test)
    struct Movable
    {
        Movable() = default;
        Movable(Movable&&) = default;
        Movable& operator=(Movable&&) = default;

        Movable& operator=(const Movable&) = delete;
        Movable(const Movable&) = delete;
    };

    boost::asio::io_context ioc;
    ba::async::Queue<Movable> q{ ioc, 10 };

    q.asyncPush(Movable{}, [](boost::system::error_code) {});
    q.asyncPop([](boost::system::error_code, ba::async::optional<Movable> val) {});

    boost::asio::spawn(ioc, [&q](boost::asio::yield_context yield) {
        q.asyncPush(Movable{}, yield);
    });

    boost::asio::spawn(ioc, [&q](boost::asio::yield_context yield) {
        Movable m = std::move(q.asyncPop(yield).value());
    });

    std::future<void> fPush = q.asyncPush(Movable{}, boost::asio::use_future);
    std::future<ba::async::optional<Movable>> fPop = q.asyncPop(boost::asio::use_future);

    ThreadPool{ ioc, 4 }.join();

    fPush.get();
    ba::async::optional<Movable> val = fPop.get();
}

#if BOOST_VERSION >= 107000
BOOST_AUTO_TEST_CASE(moveHandlerTest)
{
    // Проверка, что хендлеры всегда перемещаются, но не копируются.
    // (compile-time test)
    struct MovableHandler
    {
        MovableHandler() = default;
        MovableHandler(MovableHandler&&) = default;
        MovableHandler& operator=(MovableHandler&&) = default;

        MovableHandler& operator=(const MovableHandler&) = delete;
        MovableHandler(const MovableHandler&) = delete;

        void operator()(boost::system::error_code) const {}
        void operator()(boost::system::error_code, ba::async::optional<int>) const {}
    };

    boost::asio::io_context ioc;
    ba::async::Queue<int> q{ ioc, 10 };

    q.asyncPush(123, MovableHandler{});
    q.asyncPop(MovableHandler{});

    ThreadPool{ ioc, 2 }.join();
}
#endif

BOOST_AUTO_TEST_CASE(moveQueueTest)
{
    // Проверка, что move-конструктор и assignment корректны.
    boost::asio::io_context ioc;

    // Исходная очередь с лимитом 2
    ba::async::Queue<int> q1{ ioc, 2 };

    // Присвивать ее будем очереди с другим лимитом, для проверки, что лимит стал как в исходной.
    ba::async::Queue<int> q2{ ioc, 10 };
    std::vector<ba::async::Queue<int>> q3;

    // 5 раз вставляем, 2 извлекаем, чтобы очередь была заполнена и одна вставка осталась в ожидании.
    
    q1.asyncPush(1, [](boost::system::error_code) {});
    q1.asyncPush(2, [](boost::system::error_code) {});
    q1.asyncPush(3, [](boost::system::error_code) {});
    q1.asyncPush(4, [](boost::system::error_code) {});
    q1.asyncPush(5, [](boost::system::error_code) {});

    q1.asyncPop([&q1, &q2](boost::system::error_code, ba::async::optional<int>) mutable {
        q2 = std::move(q1);
    });

    q1.asyncPop([&q2, &q3](boost::system::error_code, ba::async::optional<int>) mutable {
        // Делаем именно в хендлере, что бы поймать состояние полной очереди и ожидающей операции вставки.
        // После ThreadPool::join нельзя, он зависнет на ожидающей операции вставки.

        q3.push_back(std::move(q2));

        BOOST_CHECK(q3[0].full());
        BOOST_CHECK_EQUAL(2, q3[0].limit()); // Лимит от исходной очереди.
        BOOST_CHECK_EQUAL(1, q3[0].cancel()); // В ожидании одна операция, которую и почистили (тредпул не зависнет).
    });

    ThreadPool{ ioc, 4 }.join();

    // Исходная и промежуточная очереди пусты.

    BOOST_CHECK(q1.empty());
    BOOST_CHECK_EQUAL(0, q1.cancel());

    BOOST_CHECK(q2.empty());
    BOOST_CHECK_EQUAL(0, q2.cancel());
}

template <typename T>
class handler_allocator
{
public:
    using value_type = T;

    explicit handler_allocator() = default;

    template <typename U>
    handler_allocator(const handler_allocator<U>& other) noexcept
    {
    }

    bool operator==(const handler_allocator& other) const noexcept
    {
        return true;
    }

    bool operator!=(const handler_allocator& other) const noexcept
    {
        return false;
    }

    T* allocate(std::size_t n) const
    {
        //std::cout << std::this_thread::get_id() << ": allocate: " << typeid(T).name() << std::endl << std::endl << std::flush;
        return (T*)::operator new(sizeof(T) * n);
    }

    void deallocate(T* p, std::size_t) const
    {
        //std::cout << std::this_thread::get_id() << ": deallocate: " << typeid(*p).name() << std::endl << std::endl << std::flush;
        return operator delete(p);
    }
};

BOOST_AUTO_TEST_CASE(allocatorTest)
{
    struct Handler
    {
        Handler() = default;
        Handler(const Handler&)
        {
            auto a = 1;
        }

        Handler(Handler&&)
        {
            auto a = 1;
        }

        using allocator_type = handler_allocator<Handler>;

        allocator_type get_allocator() const noexcept
        {
            return allocator_type{};
        }

        void operator()(boost::system::error_code ec)
        {
            //std::cout << std::this_thread::get_id() << ": void operator()(boost::system::error_code ec)" << std::endl << std::endl << std::flush;
            auto e = ec;
        }

        void operator()(boost::system::error_code ec, ba::async::optional<int>)
        {
            //std::cout << std::this_thread::get_id() << ": void operator()(boost::system::error_code ec, ba::async::optional<int>)" << std::endl << std::endl << std::flush;
            auto e = ec;
        }
    };

    boost::asio::io_context ioc{ 10 };
    ba::async::Queue<int> q{ ioc, 0 };
    ioc.post([&q]{
        q.asyncPush(1, Handler{});
        q.asyncPop(Handler{});
    });

    ThreadPool(ioc, 10).join();
}

#if defined(__cpp_impl_coroutine) && defined(__cpp_lib_coroutine) && BOOST_VERSION >= 107000
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/use_awaitable.hpp>

BOOST_AUTO_TEST_CASE(cpp20CoroTest)
{
    boost::asio::io_context ioc;
    ba::async::Queue<std::size_t> q{ ioc, 10 };

    // Одна корутина толкает.
    co_spawn(ioc, [&q]() -> boost::asio::awaitable<void> {
        // Вставляем от 1 до 10 000.
        for (std::size_t i = 1; i <= 10'000; ++i)
            co_await q.asyncPush(i, boost::asio::use_awaitable);
    }, boost::asio::detached);

    // Другая тянет.
    co_spawn(ioc, [&q]() -> boost::asio::awaitable<void> {
        std::size_t sum = 0;
        // Суммируем все извлеченное.
        for (std::size_t i = 1; i <= 10'000; ++i)
            BOOST_CHECK_NO_THROW(sum += (co_await q.asyncPop(boost::asio::use_awaitable)).value());

        // Проверяем полученную сумму.
        BOOST_CHECK_EQUAL(50005000, sum);
    }, boost::asio::detached);

    // Синхронизации между корутинами намерено нет.
    // 10 потоков в пуле.
    ThreadPool{ ioc, 10 }.join();

    BOOST_CHECK(q.empty());
    BOOST_CHECK_EQUAL(0, q.cancel());
}
#endif // defined(__cpp_impl_coroutine) && defined(__cpp_lib_coroutine) && BOOST_VERSION >= 107000
