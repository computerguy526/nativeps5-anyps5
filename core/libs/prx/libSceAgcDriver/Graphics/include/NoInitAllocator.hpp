#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_NOINITALLOCATOR_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_NOINITALLOCATOR_HPP

#include <memory>
#include <new>
#include <type_traits>
#include <utility>

namespace AgcDriver::Graphics {

// std::allocator whose argument-less construct() default-initializes, so vector::resize on a trivial type leaves new elements
// uninitialized: for a buffer that is overwritten right after growing, this skips the zero pass.
template<typename T>
struct NoInitAllocator : std::allocator<T> {
    template<typename U>
    struct rebind {
        using other = NoInitAllocator<U>;
    };
    NoInitAllocator() = default;
    template<typename U>
    NoInitAllocator(const NoInitAllocator<U>&) noexcept {}
    template<typename U>
    void construct(U* at) noexcept(std::is_nothrow_default_constructible_v<U>) {
        ::new (static_cast<void*>(at)) U;
    }
    template<typename U, typename... Args>
    void construct(U* at, Args&&... args) {
        ::new (static_cast<void*>(at)) U(std::forward<Args>(args)...);
    }
};

}

#endif
