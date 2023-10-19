#define BOOST_TEST_MODULE Libasync

#include <async/queue/queue.hpp>

#include <iostream>
#include <thread>

#include <boost/test/included/unit_test.hpp>

#include <boost/asio/steady_timer.hpp>
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
    boost::asio::strand<boost::asio::io_context::executor_type> s1{ ioc.get_executor() };
    boost::asio::io_context::strand s2{ ioc };

    ba::async::Queue<int> q0{ ioc, 10 };

    q0.asyncPush(123, boost::asio::bind_executor(s1, [s1](boost::system::error_code ec) {
        BOOST_CHECK(s1.running_in_this_thread());
        }));

    q0.asyncPop(boost::asio::bind_executor(s1, [s1](boost::system::error_code ec, boost::optional<int> val) {
        BOOST_CHECK(s1.running_in_this_thread());
        BOOST_CHECK(123 == *val);
        }));

    ThreadPool{ ioc, 10 }.join();

    q0.asyncPush(123, s2.wrap([s2](boost::system::error_code ec) {
        BOOST_CHECK(s2.running_in_this_thread());
        }));

    q0.asyncPop(s2.wrap([s2](boost::system::error_code ec, boost::optional<int> val) {
        BOOST_CHECK(s2.running_in_this_thread());
        BOOST_CHECK(123 == *val);
        }));

    ioc.restart();
    ThreadPool{ ioc, 10 }.join();

    ba::async::Queue<int> q1{ s1, 10 };

    q1.asyncPop([s1](boost::system::error_code ec, boost::optional<int> val) {
        BOOST_CHECK(s1.running_in_this_thread());
        BOOST_CHECK(123 == *val);
        });

    q1.asyncPush(123, [s1](boost::system::error_code ec) {
        BOOST_CHECK(s1.running_in_this_thread());
        });

    ioc.restart();
    ThreadPool{ ioc, 10 }.join();

    ba::async::Queue<int> q2{ s2, 10 };

    q2.asyncPop([s2](boost::system::error_code ec, boost::optional<int> val) {
        BOOST_CHECK(s2.running_in_this_thread());
        BOOST_CHECK(123 == *val);
        });

    q2.asyncPush(123, [s2](boost::system::error_code ec) {
        BOOST_CHECK(s2.running_in_this_thread());
        });

    ioc.restart();
    ThreadPool{ ioc, 10 }.join();

    BOOST_CHECK(q0.empty());
    BOOST_CHECK(q1.empty());
    BOOST_CHECK(q2.empty());

    BOOST_CHECK_EQUAL(0, q0.cancel());
    BOOST_CHECK_EQUAL(0, q1.cancel());
    BOOST_CHECK_EQUAL(0, q2.cancel());
}

BOOST_AUTO_TEST_CASE(futureTest)
{
    boost::asio::io_context ioc;
    ba::async::Queue<int> q{ ioc, 1 };

    std::future<boost::optional<int>> fPop = q.asyncPop(boost::asio::use_future);
    std::future<void> fPush = q.asyncPush(123, boost::asio::use_future);

    ThreadPool{ ioc, 10 }.join();

    BOOST_CHECK_NO_THROW(fPush.get());
    BOOST_CHECK_EQUAL(123, *fPop.get());
    BOOST_CHECK(q.empty());
    BOOST_CHECK_EQUAL(0, q.cancel());


    std::future<boost::optional<int>> fPopUnderflow = q.asyncPop(boost::asio::use_future);
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

    boost::asio::spawn(ioc, [&q](boost::asio::yield_context yield) {
        for (std::size_t i = 1; i <= 10'000; ++i)
            q.asyncPush(i, yield);
    });

    boost::asio::spawn(ioc, [&q](boost::asio::yield_context yield) {
        std::size_t sum = 0;
        for (std::size_t i = 1; i <= 10'000; ++i)
            BOOST_CHECK_NO_THROW(sum += q.asyncPop(yield).get());

        BOOST_CHECK_EQUAL(50005000, sum);
    });

    ThreadPool{ ioc, 10 }.join();

    BOOST_CHECK(q.empty());
    BOOST_CHECK_EQUAL(0, q.cancel());
}

BOOST_AUTO_TEST_CASE(ManyProducerTest)
{
    boost::asio::io_context ioc;
    ba::async::Queue<std::size_t> q{ioc, 15 };

    for (int j = 0; j < 10; ++j)
    {
        boost::asio::spawn(ioc, [&q](boost::asio::yield_context yield) {
            for (std::size_t i = 1; i <= 1'000; ++i)
                q.asyncPush(i, yield);
        });
    }

    boost::asio::spawn(ioc, [&q](boost::asio::yield_context yield) {
        std::size_t sum = 0;
        for (std::size_t i = 1; i <= 10'000; ++i)
            BOOST_CHECK_NO_THROW(sum += q.asyncPop(yield).get());

        BOOST_CHECK_EQUAL(5005000, sum);
    });

    ThreadPool{ ioc, 10 }.join();

    BOOST_CHECK(q.empty());
    BOOST_CHECK_EQUAL(0, q.cancel());
}

/*BOOST_AUTO_TEST_CASE(manyConsumerTest)
{
    boost::asio::io_context ioc;
    ba::async::Queue<std::size_t> q{ ioc, 15 };

    boost::asio::spawn(ioc, [&q](boost::asio::yield_context yield) {
        for (std::size_t i = 1; i <= 10'000; ++i)
            BOOST_CHECK_NO_THROW(q.asyncPush(i, yield));
    });

    std::size_t sum = 0;

    for (int j = 0; j < 10; ++j)
    {
        boost::asio::spawn(ioc, [&q, &sum](boost::asio::yield_context yield) {
            for (std::size_t i = 1; i <= 1'000; ++i)
                sum += q.asyncPop(yield).get();
        });
    }

    ThreadPool{ ioc, 10 }.join();
    BOOST_CHECK_EQUAL(50005000, sum);

    BOOST_CHECK(q.empty());
    BOOST_CHECK_EQUAL(0, q.cancel());
}*/

BOOST_AUTO_TEST_CASE(moveValueTest)
{
    struct Movable
    {
        Movable() = default;
        Movable(Movable&&) = default;
        Movable& operator=(Movable&&) = default;
        Movable& operator=(const Movable&) = delete;

        Movable(const Movable&)
        {
            BOOST_TEST_ERROR("Movement value test failed");
        }
    };

    boost::asio::io_context ioc;
    ba::async::Queue<Movable> q{ ioc, 10 };

    boost::asio::spawn(ioc, [&q](boost::asio::yield_context yield) {
        for (std::size_t i = 1; i <= 1'000; ++i)
            q.asyncPush(Movable{}, yield);
    });

    boost::asio::spawn(ioc, [&q](boost::asio::yield_context yield) {
        for (std::size_t i = 1; i <= 1'000; ++i)
            Movable m = std::move(q.asyncPop(yield).get());
    });

    ThreadPool{ ioc, 4 }.join();
}

BOOST_AUTO_TEST_CASE(moveQueueTest)
{
    boost::asio::io_context ioc;
    ba::async::Queue<int> q1{ ioc, 2 };
    ba::async::Queue<int> q2{ ioc, 10 };
    std::vector<ba::async::Queue<int>> q3;
   

    q1.asyncPush(1, [](boost::system::error_code) {});
    q1.asyncPush(2, [](boost::system::error_code) {});
    q1.asyncPush(3, [](boost::system::error_code) {});
    q1.asyncPush(4, [](boost::system::error_code) {});
    q1.asyncPush(5, [](boost::system::error_code) {});

    q1.asyncPop([&q1, &q2](boost::system::error_code, boost::optional<int>) mutable {
        q2 = std::move(q1);
    });

    q1.asyncPop([&q2, &q3](boost::system::error_code, boost::optional<int>) mutable {
        q3.push_back(std::move(q2));

        BOOST_CHECK(q3[0].full());
        BOOST_CHECK_EQUAL(1, q3[0].cancel());
    });

    ThreadPool{ ioc, 4 }.join();

    BOOST_CHECK(q1.empty());
    BOOST_CHECK_EQUAL(0, q1.cancel());

    BOOST_CHECK(q2.empty());
    BOOST_CHECK_EQUAL(0, q2.cancel());
}
