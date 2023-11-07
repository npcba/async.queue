#pragma once

#include <memory>
#include <cassert>

#include <boost/core/noncopyable.hpp>
#include <boost/asio/associated_allocator.hpp>


namespace ba {
namespace async {
namespace detail {


// Movable-only ������ std::function ��� ������������ �������� ����������� ���������.
// ���������� ������ void, ��� � ������ ���������� ������� �� ����� (�������� �� ������).
// ��� ������������� ����������� ��������� ���������� ���������������� ��������� (��� ��������� asio).
// operator() �������� ���������� ������ � ����� ��������� ���(��������� ����� ��������,
// � ������ ���������� �� � ����������).
// Asio ����������� ����� �������������, ��� allocator.deallocate ����� ������ ������ �� ������ ��������,
// � ����� ����� ��������� ������ ������ ����� allocate � deallocate ���� ��� ��������� �� ������ �������.
// ������ � ������ ���������� ������ ������������ ��� ��������:
// ������� ������ ������������ �� ������ operator() ����� ����������� ����������� ��������� �� ����,
// ��������� ��� �� ������������ ������ � ������� �������� �����.
// ������ ������ ����� ������������� ���������� �� ������������ ������� ������������� ���������� � Queue,
// � ����� ������������ � ���������� ������ ������ asio::post. ������� ���� ��� ������� �� �����.
template <typename>
class Function;

template <typename... Args>
class Function<void(Args...)>
{
public:
    // ����� ������������.
    Function(Function&&) = default;
    Function& operator=(Function&&) = default;

    // �� ����� ������������.
    Function(const Function&) = delete;
    Function& operator=(const Function&) = delete;

    // ��������� ��������� ����������, �.�. � ����������� ����� ������� ������������� ������,
    // � ����� ������ rvalue-ref.
    template <typename F, typename Alloc>
    Function(F& f) = delete;
    template <typename F, typename Alloc>
    Function(const F& f) = delete;

    // ���������������� ��������� �� rvalue-ref � ���������������� �����������.
    template <typename F>
    Function(F&& f)
    {
        auto ha = Holder<F>::getAllocator(f);
        // �������� ������ ��� ������.
        Holder<F>* holder = ha.allocate(1);
        // ����������� ��������� ������� ����������, ��� ��������, ��� ���������������� ����,
        // ������� ��������� � ������.
        if (!holder)
            throw std::bad_alloc{};

        try
        {
            // ������������ ������ �� ���������� ������.
            new(holder) Holder<F>{ std::move(f) };
        }
        catch (...)
        {
            // ���� ����������� ������ ����������, ����������� ������.
            ha.deallocate(holder, 1);
            throw;
        }

        m_callable.reset(holder);
    }

    // ������������ ����� �� ���������� ������
    void operator()(Args... args)
    {
        // ��� ������ � ������������ ����� operator(),
        // ���������� ������ � ������ ���������� ���,
        // � ���� ��������, �� ������������ ��, �������� ����� 1 ���� �� ���������� � asio.
        assert(m_callable);

        m_callable->destructibleCallOp(std::forward<Args>(args)...);
        // ����� ������, ������ ���������������, ��������� unique_ptr.
        m_callable.release();
    }

private:
    // ����������� ���������.
    struct Callable
        : boost::noncopyable
    {
        // �������� ����������� ��������� � ����� ����������� operator() (��. ������ ����).
        virtual void destructibleCallOp(Args... args) = 0;
        // �������� ����������� ��������� ��� ������ operator() (��� �����������).
        virtual void destruct() = 0;
    protected:
        // ���������� �������� ���������, �������� ���������� �������� ����.
        ~Callable() = default;
    };

    // ��������� ����������� ����������� ���������, �������������� ���������������� �����������,
    // ���� �� ����������� �� � ����������� � ������.
    template <typename F>
    class Holder
        : public Callable
    {
    public:
        Holder(F&& f)
            : m_f{ std::move(f) }
        {
        }

        void destructibleCallOp(Args... args) override
        {
            doDestruct()(std::forward<Args>(args)...);
        }

        void destruct() override
        {
            doDestruct();
        }

        static auto getAllocator(F& f)
        {
            auto a = boost::asio::get_associated_allocator(f);
            // �������� ���������������� ��������� �� ��� �������.
            typename std::allocator_traits<decltype(a)>::template rebind_alloc<Holder> ha{ a };
            return ha;
        }

    private:
        // ����� ��������� ���� ���������� �������� ����� ����������� �������� (move-copy).
        F doDestruct()
        {
            // ���������� �� ���� �������.
            F copyF = std::move(m_f);

            // ���������� ����.
            this->~Holder();
            // ����������� ����������� ������ ��-��� ����.
            getAllocator(copyF).deallocate(this, 1);

            // ���������� ����� �������� (��������� ������ �� �����).
            return copyF;
        }

        F m_f;
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
