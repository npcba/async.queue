#pragma once

#include <memory>
#include <cassert>

#include <boost/core/noncopyable.hpp>
#include <boost/scope_exit.hpp>


namespace ba {
namespace async {
namespace detail {

template <typename R, typename... Args>
class function
{
public:
    function(function&&) = default;
    function& operator=(function&&) = default;

    function(const function&) = delete;
    function& operator=(const function&) = delete;

    template <typename F, typename Alloc>
    function(F& f, const Alloc& a) = delete;
    template <typename F, typename Alloc>
    function(const F& f, const Alloc& a) = delete;

    template <typename F, typename Alloc>
    function(F&& f, const Alloc& a)
    {
        class Holder
            : public Callable
        {
        public:
            using allocator_type = typename std::allocator_traits<Alloc>::template rebind_alloc<Holder>;

            Holder(F&& f, const allocator_type& a)
                : m_f(std::move(f)), m_a{ a }
            {
            }

            R operator()(Args... args) override
            {
                return R(m_f(std::forward<Args>(args)...));
            }

            void destruct() override
            {
                allocator_type copyA = m_a;
                this->~Holder();
                copyA.deallocate(this, 1);
            }

        private:
            F m_f;
            allocator_type m_a;
        };

        typename Holder::allocator_type ha{ a };
        Holder* p = ha.allocate(1);

        bool constructed = false;
        BOOST_SCOPE_EXIT_TPL(p, &constructed, &ha)
        {
            if (!constructed)
                ha.deallocate(p, 1);
        } BOOST_SCOPE_EXIT_END

        new(p) Holder{ std::move(f), ha };
        constructed = true;
        m_callable.reset(p);
    }

    R operator()(Args... args) const
    {
        return (*m_callable)(std::forward<Args>(args)...);
    }

private:
    struct Callable;
    struct Deleter;

    std::unique_ptr<Callable, Deleter> m_callable;
};

template <typename R, typename... Args>
struct function<R, Args...>::Callable
    : boost::noncopyable
{
    virtual R operator()(Args... args) = 0;
    virtual void destruct() = 0;
protected:
    ~Callable() = default;
};

template <typename R, typename... Args>
struct function<R, Args...>::Deleter
{
    void operator()(Callable* p) const
    {
        assert(p);
        p->destruct();
    };
};

} // namespace detail
} // namespace async
} // namespace ba
