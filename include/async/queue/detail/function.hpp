#pragma once

#include <memory>
#include <cassert>
#include <functional>

#include <boost/core/noncopyable.hpp>


namespace ba {
namespace async {
namespace detail {

template <typename>
class Function;

template <typename... Args>
class Function<void(Args...)>
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
        using HolderType = Holder<F, Alloc>;

        typename HolderType::allocator_type ha{ a };
        HolderType* holder = ha.allocate(1);

        try
        {
            new(holder) HolderType{ std::move(f), ha };
        }
        catch (...)
        {
            ha.deallocate(holder, 1);
            throw;
        }

        m_callable.reset(holder);
    }

    void operator()(Args... args)
    {
        assert(m_callable);
        if (!m_callable)
            throw std::bad_function_call();

        (*m_callable)(std::forward<Args>(args)...);
        m_callable.release();
    }

    explicit operator bool() const noexcept
    {
        return m_callable;
    }

private:
    struct Callable
        : boost::noncopyable
    {
        virtual void operator()(Args... args) = 0;
        virtual void destruct() = 0;
    protected:
        ~Callable() = default;
    };

    template <typename F, typename Alloc>
    class Holder
        : public Callable
    {
    public:
        using allocator_type = typename std::allocator_traits<Alloc>::template rebind_alloc<Holder>;

        Holder(F&& f, const allocator_type& a)
            : m_f(std::move(f)), m_a{ a }
        {
        }

        void operator()(Args... args) override
        {
            doDestruct()(std::forward<Args>(args)...);
        }

        void destruct() override
        {
            doDestruct();
        }

    private:
        F doDestruct()
        {
            F copyF = std::move(m_f);
            allocator_type copyA = m_a;

            this->~Holder();
            copyA.deallocate(this, 1);

            return copyF;
        }

        F m_f;
        allocator_type m_a;
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
