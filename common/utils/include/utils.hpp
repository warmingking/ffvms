#ifndef __UTILS_H__
#define __UTILS_H__

namespace common
{
namespace utils
{

    template <auto fn>
    using deleter_from_fn = std::integral_constant<decltype(fn), fn>;

    template <typename T, auto fn>
    using unique_ptr_with_deleter = std::unique_ptr<T, deleter_from_fn<fn>>;

}
}
#endif // __UTILS_H__