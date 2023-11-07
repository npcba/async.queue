#pragma once

#include <memory>
#include <cassert>

#include <boost/core/noncopyable.hpp>
#include <boost/asio/associated_allocator.hpp>


namespace ba {
namespace async {
namespace detail {


// Movable-only замена std::function для полиморфного хранения разнотипных хендлеров.
// Возвращает только void, ибо в данной библиотеке другого не нужно (добавить не сложно).
// Для распределения внутреннего состояния использует пользовательский аллокатор (для поддержки asio).
// operator() вызывает внутренний объект и сразу разрушает его(повторный вызов запрещен,
// в данной библиотеке он и невозможен).
// Asio гарантирует своим пользователям, что allocator.deallocate будет вызван строго до вызова хендлера,
// а также будет выставлен барьер памяти между allocate и deallocate если они разошлись по разным потокам.
// Значит и данная библиотека должна поддерживать эту гарантию:
// Очистка памяти производится до вызова operator() путем перемещения внутреннего состояния на стек,
// удалением его из динамической памяти и вызовом стековой копии.
// Барьер памяти между инициирующими операциями из параллельных потоков гарантируется мьютексами в Queue,
// а между инициаторами и хендлерами барьер ставит asio::post. Поэтому явно его ставить не нужно.
template <typename>
class Function;

template <typename... Args>
class Function<void(Args...)>
{
public:
    // Умеет перемещаться.
    Function(Function&&) = default;
    Function& operator=(Function&&) = default;

    // Не умеет копироваться.
    Function(const Function&) = delete;
    Function& operator=(const Function&) = delete;

    // Отключены случайные перегрузки, т.к. в конструктор могут попасть универсальные ссылки,
    // а нужны только rvalue-ref.
    template <typename F, typename Alloc>
    Function(F& f) = delete;
    template <typename F, typename Alloc>
    Function(const F& f) = delete;

    // Инициализируется функтором по rvalue-ref и пользовательским аллокатором.
    template <typename F>
    Function(F&& f)
    {
        auto ha = Holder<F>::getAllocator(f);
        // Выделяет память под холдер.
        Holder<F>* holder = ha.allocate(1);
        // Стандартный аллокатор бросает исключение, нет гарантии, что пользовательский тоже,
        // приведём поведение к общему.
        if (!holder)
            throw std::bad_alloc{};

        try
        {
            // Конструирует холдер на выделенной памяти.
            new(holder) Holder<F>{ std::move(f) };
        }
        catch (...)
        {
            // Если конструктор бросил исключение, освобождает память.
            ha.deallocate(holder, 1);
            throw;
        }

        m_callable.reset(holder);
    }

    // Пробрасывает вызов на внутренний объект
    void operator()(Args... args)
    {
        // Это первый и единственный вызов operator(),
        // повторного вызова в данной библиотеке нет,
        // а если случился, то выбрасывайте ее, хендлеры более 1 раза не вызываются в asio.
        assert(m_callable);

        m_callable->destructibleCallOp(std::forward<Args>(args)...);
        // После вызова, холдер самоуничтожился, отпускаем unique_ptr.
        m_callable.release();
    }

private:
    // Виртуальный интерфейс.
    struct Callable
        : boost::noncopyable
    {
        // Удаление внутреннего состояния и вызов внутреннего operator() (см. детали ниже).
        virtual void destructibleCallOp(Args... args) = 0;
        // Удаление внутреннего состояния без вызова operator() (для деструктора).
        virtual void destruct() = 0;
    protected:
        // Деструктор вызывать запрещено, удаление происходит методами выше.
        ~Callable() = default;
    };

    // Хранитель конкретного внутреннего состояния, параметризован пользовательским аллокатором,
    // этим же аллокатором он и размещается в памяти.
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
            // Ребиндит пользовательский аллокатор на тип холдера.
            typename std::allocator_traits<decltype(a)>::template rebind_alloc<Holder> ha{ a };
            return ha;
        }

    private:
        // Перед удалением себя возвращает стековую копию внутреннего функтора (move-copy).
        F doDestruct()
        {
            // Перемещает на стек функтор.
            F copyF = std::move(m_f);

            // Деструктит себя.
            this->~Holder();
            // Освобождает аллокатором память из-под себя.
            getAllocator(copyF).deallocate(this, 1);

            // Возвращает копию функтора (аллокатор больше не нужен).
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
