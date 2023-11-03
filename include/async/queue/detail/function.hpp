#pragma once

#include <memory>
#include <cassert>

#include <boost/core/noncopyable.hpp>
#include <boost/scope_exit.hpp>


namespace ba {
namespace async {
namespace detail {

template <typename>
class Function;

template <typename R, typename... Args>
class Function<R(Args...)>
{
public:
    Function(Function&&) = default;
    Function& operator=(Function&&) = default;

    Function(const Function&) = delete;
    Function& operator=(const Function&) = delete;

    template <typename F, typename Alloc>
    Function(F& f, const Alloc& a) = delete;
    template <typename F, typename Alloc>
    Function(const F& f, const Alloc& a) = delete;

    template <typename F, typename Alloc>
    Function(F&& f, const Alloc& a)
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
                return R(doDestruct()(std::forward<Args>(args)...));
            }

            void destruct() override
            {
                doDestruct();
            }

        private:
            F doDestruct()
            {
                Holder* self = this;
                BOOST_SCOPE_EXIT_TPL(self, m_a)
                {
                    self->~Holder();
                    m_a.deallocate(self, 1);
                } BOOST_SCOPE_EXIT_END

                return std::move(m_f);
            }

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

    R operator()(Args... args)
    {
        assert(m_callable);
        if (!m_callable)
            throw std::bad_function_call();

        BOOST_SCOPE_EXIT_TPL(&m_callable)
        {
            m_callable.release();
        } BOOST_SCOPE_EXIT_END

        return (*m_callable)(std::forward<Args>(args)...);
    }

    explicit operator bool() const noexcept
    {
        return m_callable;
    }

private:
    struct Callable
        : boost::noncopyable
    {
        virtual R operator()(Args... args) = 0;
        virtual void destruct() = 0;
    protected:
        ~Callable() = default;
    };

    struct Deleter
    {
        void operator()(Callable* p) const
        {
            assert(p);
            p->destruct();
        };
    };

    std::unique_ptr<Callable, Deleter> m_callable;
};

} // namespace detail
} // namespace async
} // namespace ba
