#pragma once

#include <type_traits>
#include <boost/system/error_code.hpp>


namespace ba {
namespace async {

enum class QueueError : int
{
    OK = 0,
    OPERATION_CANCELLED,
    QUEUE_CLOSED
};

namespace detail {
    class QueueErrorCategory
        : public boost::system::error_category
    {
    public:
        const char* name() const noexcept override
        {
            return "ba.async.Queue error category";
        }

        std::string message(int ev) const override
        {
            switch (QueueError(ev))
            {
            case QueueError::OK:
                return "OK";
            case QueueError::OPERATION_CANCELLED:
                return "Queue operation cancelled";
            case QueueError::QUEUE_CLOSED:
                return "Queue closed";
            default:
                assert(!"Unexpected QueueError value");
                return "Unknown QueueError error";
            }
        }
    };
} // namespace detail

inline const boost::system::error_category& queueCategory()
{
    static detail::QueueErrorCategory instance;
    return instance;
}

inline boost::system::error_code make_error_code(QueueError e)
{
    return boost::system::error_code{ static_cast<int>(e), queueCategory() };
}

inline boost::system::error_condition make_error_condition(QueueError e)
{
    return boost::system::error_condition{ static_cast<int>(e), queueCategory() };
}

} // namespace async
} // namespace ba


namespace boost {
namespace system {

template<>
struct is_error_code_enum<ba::async::QueueError>
    : std::true_type
{
};

} // namespace system
} // namespace boost
