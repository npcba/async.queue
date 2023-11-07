#pragma once

#include <memory>
#include <cassert>

#include <boost/core/noncopyable.hpp>


namespace ba {
namespace async {
namespace detail {


// Movable-only ������ std::function ��� ������������ �������� ����������� ��������� � ��������� � ���� �����������
// ���������� ������ void, ��� � ������ ���������� ������� �� ����� (�������� �� ������).
// ��� ������������� ����������� ��������� ���������� ���������������� ��������� (��� ��������� asio).
// ��� ��������� ����� �������� �� ���������� ���������, �.�. �������� �� ����� ��� ����������.
// ����� ������� ���������� �� ������ ��� �������, �� � ��� ����������.
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
    Function(F& f, const Alloc& a) = delete;
    template <typename F, typename Alloc>
    Function(const F& f, const Alloc& a) = delete;

    // ���������������� ��������� �� rvalue-ref � ���������������� �����������.
    template <typename F, typename Alloc>
    Function(F&& f, const Alloc& a)
    {
        using HolderType = Holder<F, Alloc>;

        // �������� ���������������� ��������� �� ��� �������.
        typename HolderType::allocator_type ha{ a };
        // �������� ������ ��� ������.
        HolderType* holder = ha.allocate(1);
        // ����������� ��������� ������� ����������, ��� ��������, ��� ���������������� ����,
        // ������� ��������� � ������.
        if (!holder)
            throw std::bad_alloc{};

        try
        {
            // ������������ ������ �� ���������� ������.
            new(holder) HolderType{ std::move(f), ha };
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
    template <typename F, typename Alloc>
    class Holder
        : public Callable
    {
    public:
        using allocator_type = typename std::allocator_traits<Alloc>::template rebind_alloc<Holder>;

        Holder(F&& f, const allocator_type& a)
            : m_f{ std::move(f) }, m_a{ a }
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

    private:
        // ����� ��������� ���� ���������� �������� ����� ����������� �������� (move-copy).
        F doDestruct()
        {
            // ���������� �� ���� �������.
            F copyF = std::move(m_f);
            // �������� �� ���� ���������.
            allocator_type copyA = m_a;

            // ���������� ����.
            this->~Holder();
            // ����������� ����������� ������ ��-��� ����.
            copyA.deallocate(this, 1);

            // ���������� ����� �������� (��������� ������ �� �����).
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
