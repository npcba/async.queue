#pragma once

#include <memory>
#include <cassert>

#include <boost/intrusive/slist.hpp>
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
class FunctionQueue;

template <typename... Args>
class FunctionQueue<void(Args...)>
{
public:
    FunctionQueue() = default;
    FunctionQueue(FunctionQueue&&) = default;
    FunctionQueue& operator=(FunctionQueue&&) = default;

    ~FunctionQueue()
    {
        assert(m_list.empty());
        m_list.clear_and_dispose([](Node* node) { node->dispose(); });
    }

    template <typename F>
    void push(F&& f)
    {
        using HolderType = Holder<std::decay_t<F>>;
        auto ha = HolderType::rebindAllocFrom(f);

        // Проверим на всякий, что пользовательский аллокатор соответствует
        // как минимум equality requirements.
        assert(HolderType::rebindAllocFrom(f) == ha);

        // Выделяет память под холдер.
        HolderType* holder = ha.allocate(1);
        // Стандартный аллокатор бросает исключение, нет гарантии, что пользовательский тоже,
        // приведём поведение к общему.
        if (!holder)
            throw std::bad_alloc{};

        try
        {
            // Конструирует холдер на выделенной памяти.
            ::new(static_cast<void*>(holder)) HolderType{ std::forward<F>(f) };
        }
        catch (...)
        {
            // Если конструктор бросил исключение, освобождает память.
            ha.deallocate(holder, 1);
            throw;
        }

        m_list.push_back(*holder);
    }

    void pop(Args... args)
    {
        assert(!m_list.empty());
        Node& fn = m_list.front();
        m_list.pop_front();
        // После вызова звено самоуничтожится.
        fn.disposableCall(std::forward<Args>(args)...);
    }

    bool empty() const noexcept
    {
        return m_list.empty();
    }

private:
    // Базовый класс для звена списка.
    struct Node
        : boost::intrusive::slist_base_hook<>
    {
        Node() = default;
        Node(const Node&) = delete;
        Node& operator=(const Node&) = delete;

        // Удаление внутреннего состояния и вызов внутреннего operator() (см. детали ниже).
        virtual void disposableCall(Args... args) = 0;
        // Удаление внутреннего состояния без вызова operator() (вызывается из ~FunctionQueue()).
        virtual void dispose() = 0;
    protected:
        // Деструктор извне недоступен, удаление происходит методами выше.
        ~Node() = default;
    };

    // Хранитель конкретного внутреннего состояния.
    template <typename F>
    class Holder
        : public Node
    {
    public:
        Holder(F&& f)
            : m_f{ std::move(f) }
        {
        }

        Holder(const F& f)
            : m_f{ f }
        {
        }

        void disposableCall(Args... args) override
        {
            destruct()(std::forward<Args>(args)...);
        }

        void dispose() override
        {
            destruct();
        }

        static auto rebindAllocFrom(const F& f)
        {
            auto a = boost::asio::get_associated_allocator(f, std::allocator<F>{});
            // Ребиндит пользовательский аллокатор на тип холдера.
            typename std::allocator_traits<decltype(a)>::template rebind_alloc<Holder> ha{ a };
            return ha;
        }

    private:
        // Деструктор извне недоступен, удаление происходит через destruct().
        ~Holder() = default;

        // Перед удалением себя возвращает стековую копию внутреннего функтора (move-copy)
        F destruct()
        {
            // Перемещает на стек функтор.
            F copyF = std::move(m_f);

            // Деструктит себя.
            this->~Holder();
            // Освобождает аллокатором память из-под себя.
            rebindAllocFrom(copyF).deallocate(this, 1);

            // Возвращает копию функтора (аллокатор больше не нужен).
            return copyF;
        }

        F m_f;
    };

    boost::intrusive::slist<Node, boost::intrusive::cache_last<true>, boost::intrusive::constant_time_size<false>> m_list;
};

} // namespace detail
} // namespace async
} // namespace ba
