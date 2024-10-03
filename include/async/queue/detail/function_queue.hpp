#pragma once

#include "compressed_pair.hpp"

#include <memory>
#include <cassert>

#include <boost/intrusive/slist.hpp>
#include <boost/asio/associated_allocator.hpp>


namespace ba {
namespace async {
namespace detail {


// Односвязный список для полиморфного хранения разнотипных функций с общей сигнатурой.
// Сигнатура с возвращаемым значением только void, ибо в данной библиотеке другого не нужно (добавить не сложно).
// Для распределения внутреннего состояния используется пользовательский аллокатор (для поддержки asio).
// Если пользовательская функция не имеет ассоциированного аллокатора, то будет использован DefaultAllocator.
// operator() вызывает внутренний объект и сразу разрушает его(повторный вызов запрещен,
// в данной библиотеке он и невозможен).
// Asio гарантирует своим пользователям, что allocator.deallocate будет вызван строго до вызова хендлера,
// а также будет выставлен барьер памяти между allocate и deallocate если они разошлись по разным потокам.
// Значит и данная библиотека должна поддерживать эту гарантию:
// Очистка памяти производится до вызова operator() путем перемещения внутреннего состояния на стек,
// удалением его из динамической памяти и вызовом стековой копии.
// Барьер памяти между инициирующими операциями из параллельных потоков гарантируется мьютексами в Queue,
// а между инициаторами и хендлерами барьер ставит asio::post. Поэтому явно его ставить не нужно.
template <typename Signature, typename DefautlAllocator>
class FunctionQueue;

template <typename... Args, typename DefaultAllocator>
class FunctionQueue<void(Args...), DefaultAllocator>
{
public:
    explicit FunctionQueue(const DefaultAllocator& defAlloc = DefaultAllocator{})
        : m_data{ defAlloc, ListType{} }
    {
    }

    FunctionQueue(FunctionQueue&&) = default;

    FunctionQueue& operator=(FunctionQueue&& other) noexcept
    {
        static_assert(
              std::is_move_assignable<DefaultAllocator>::value
            , "DefaultAllocator is not move-assignable. Use AssignableAllocatorWrapper<DefaultAllocator> if you want it."
            );

        if (this == &other)
            return *this;

        // Сначала нужно почистить старым аллокатором.
        clear();
        // Только потом присваивать новый список с новым аллокатором.
        m_data = std::move(other.m_data);
        assert(other.getList().empty());

        return *this;
    }

    ~FunctionQueue() noexcept
    {
        clear();
    }

    template <typename F>
    void push(F&& f)
    {
        using HolderType = Holder<std::decay_t<F>>;
        auto ha = HolderType::rebindAllocFrom(f, getDefAlloc());

        // Проверим на всякий, что пользовательский аллокатор соответствует
        // как минимум equality requirements.
        assert(HolderType::rebindAllocFrom(f, getDefAlloc()) == ha);

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

        getList().push_back(*holder);
    }

    // Убирает функцию из начала очереди и вызывает одноразовый operator().
    void pop(Args... args)
    {
        assert(!getList().empty());

        Node& fn = getList().front();
        getList().pop_front();

        // После вызова звено самоуничтожится.
        fn.disposableCall(getDefAlloc(), std::forward<Args>(args)...);
    }

    bool empty() const noexcept
    {
        return getList().empty();
    }

private:
    // Базовый класс для звена списка.
    // Содержит управляющие структуры типа указателя на следующее звено в slist_base_hook
    class Node;

    // Хранитель реального типа функции, вставляемой в список.
    template <typename F>
    class Holder;

    // В качестве списка используется boost::intrusive::slist, который не выделяет динамической памяти.
    // Все нужные управляющие структуры содержатся в Node, поэтому память под них и Holder выделяется одним куском.
    // Это позволяет гарантировать пользователю, что любые динамические данные связанные
    // с конкретным пользовательским хендлером размещаются его же аллокатором.
    using ListType = boost::intrusive::slist<
          Node
        , boost::intrusive::cache_last<true>
        , boost::intrusive::constant_time_size<false>
        >;

    // clear сделан для успокоения совести для надежности деструктора и operaror=,
    // охватывающий класс Queue всегда отменяет очередь отложенных операций в своем деструкторе или operaror=.
    void clear() noexcept
    {
        assert(getList().empty());

        auto disposer = [&alloc{ getDefAlloc() }](Node* node) {
            node->dispose(alloc);
        };

        getList().clear_and_dispose(disposer);
    }

    ListType& getList() noexcept
    {
        return m_data.getSolid();
    }

    const ListType& getList() const noexcept
    {
        return m_data.getSolid();
    }

    DefaultAllocator& getDefAlloc() noexcept
    {
        return m_data.getEmpty();
    }

    // Сжимаем аллокатор, зачастую он без состояния.
    CompressedPair<DefaultAllocator, ListType> m_data;
};


// Базовый класс для звена списка.
// Содержит управляющие структуры типа указателя на следующее звено в slist_base_hook
template <typename... Args, typename DefaultAllocator>
class FunctionQueue<void(Args...), DefaultAllocator>::Node
    : public boost::intrusive::slist_base_hook<>
{
public:
    Node() = default;
    Node(const Node&) = delete;
    Node& operator=(const Node&) = delete;

    // Удаление внутреннего состояния и вызов внутреннего operator() (см. детали ниже).
    virtual void disposableCall(const DefaultAllocator& defAlloc, Args... args) = 0;
    // Удаление внутреннего состояния без вызова operator() (вызывается из clear()).
    virtual void dispose(const DefaultAllocator& defAlloc) = 0;
protected:
    // Деструктор извне недоступен, удаление происходит методами выше.
    ~Node() = default;
};


// Хранитель реального типа функции, вставляемой в список.
template <typename... Args, typename DefaultAllocator>
template <typename F>
class FunctionQueue<void(Args...), DefaultAllocator>::Holder final
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

    void disposableCall(const DefaultAllocator& defAlloc, Args... args) override
    {
        destruct(defAlloc)(std::forward<Args>(args)...);
    }

    void dispose(const DefaultAllocator& defAlloc) override
    {
        destruct(defAlloc);
    }

    // Возвращает аллокатор ассоциированный с f, и сразу перепривязанный на тип Holder.
    static auto rebindAllocFrom(const F& f, const DefaultAllocator& defAlloc) noexcept
    {
        // Получает ассоциированный аллокатор (если нет его, то дефолтный).
        auto a = boost::asio::get_associated_allocator(f, defAlloc);
        // Ребиндит пользовательский аллокатор на тип холдера.
        typename std::allocator_traits<decltype(a)>::template rebind_alloc<Holder> ha{ a };
        return ha;
    }

private:
    // Деструктор извне недоступен, удаление происходит через destruct().
    ~Holder() = default;

    // Перед удалением себя возвращает стековую копию внутреннего функтора (move-copy).
    F destruct(const DefaultAllocator& defAlloc)
    {
        // Перемещает на стек функтор.
        F copyF = std::move(m_f);

        // Деструктит себя.
        this->~Holder();
        // Освобождает аллокатором память из-под себя.
        // Аллокатор здесь не хранится, вычисляем его так же, как и при выделении памяти,
        // ведь rebindAllocFrom должен возвращать эквивалентные аллокаторы.
        rebindAllocFrom(copyF, defAlloc).deallocate(this, 1);

        // Возвращает копию функтора (аллокатор больше не нужен).
        return copyF;
    }

private:
    F m_f;
};

} // namespace detail
} // namespace async
} // namespace ba
