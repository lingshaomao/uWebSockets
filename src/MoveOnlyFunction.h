/*
MIT License

Copyright (c) 2020 Oleg Fatkhiev

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

/* Sources fetched from https://github.com/ofats/any_invocable on 2021-02-19. */

#ifndef _ANY_INVOKABLE_H_
#define _ANY_INVOKABLE_H_

#include <functional>
#include <memory>
#include <type_traits>

// clang-format off
/*
namespace std {
  template<class Sig> class any_invocable; // never defined

  template<class R, class... ArgTypes>
  class any_invocable<R(ArgTypes...) cv ref noexcept(noex)> {
  public:
    using result_type = R;

    // SECTION.3, construct/copy/destroy
    any_invocable() noexcept;
    any_invocable(nullptr_t) noexcept;
    any_invocable(any_invocable&&) noexcept;
    template<class F> any_invocable(F&&);

    template<class T, class... Args>
      explicit any_invocable(in_place_type_t<T>, Args&&...);
    template<class T, class U, class... Args>
      explicit any_invocable(in_place_type_t<T>, initializer_list<U>, Args&&...);

    any_invocable& operator=(any_invocable&&) noexcept;
    any_invocable& operator=(nullptr_t) noexcept;
    template<class F> any_invocable& operator=(F&&);
    template<class F> any_invocable& operator=(reference_wrapper<F>) noexcept;

    ~any_invocable();

    // SECTION.4, any_invocable modifiers
    void swap(any_invocable&) noexcept;

    // SECTION.5, any_invocable capacity
    explicit operator bool() const noexcept;

    // SECTION.6, any_invocable invocation
    R operator()(ArgTypes...) cv ref noexcept(noex);

    // SECTION.7, null pointer comparisons
    friend bool operator==(const any_invocable&, nullptr_t) noexcept;

    // SECTION.8, specialized algorithms
    friend void swap(any_invocable&, any_invocable&) noexcept;
  };
}
*/
// clang-format on

namespace ofats {

namespace any_detail {

/*
用于创建具有指定大小和对齐要求的未初始化存储:
第一个参数是所需存储的大小: sizeof(void*) 返回指针的大小，通常是 4 字节（32 位系统）或 8 字节（64 位系统）。
第二个参数是对齐要求: sizeof(void*) * 2 计算出所需的存储大小，即两个指针的大小。
*/
using buffer = std::aligned_storage_t<sizeof(void*) * 2, alignof(void*)>;

template <class T>
inline constexpr bool is_small_object_v =
    sizeof(T) <= sizeof(buffer) && alignof(buffer) % alignof(T) == 0 &&
    std::is_nothrow_move_constructible_v<T>;
    // 使用 std::is_nothrow_move_constructible_v 检查 T 是否可以无抛出地移动构造。这确保 T 的移动构造函数不会抛出异常

/*
union 的大小等于其最大成员的大小。在这个例子中，buffer 的大小是 sizeof(void*) * 2，因此 union 的大小也是 sizeof(void*) * 2。
union 的对齐要求等于其成员中最大的对齐要求。在这个例子中，buffer 的对齐要求是 alignof(void*)，因此 union 的对齐要求也是 alignof(void*)。

共享内存：union 的所有成员共享同一块内存空间，这意味着 storage 的大小等于其最大成员的大小。在这种情况下，storage 的大小将是 sizeof(buffer)。
减少内存占用：如果你需要在不同情况下存储不同类型的数据（例如，有时存储一个指针，有时存储一个小对象），使用 union 可以避免为每个数据类型分配单独的内存，从而节省内存。
多用途容器：storage 可以在不同情况下存储不同类型的数据，提供了一种灵活的方式来管理不同类型的数据。
动态切换：你可以在运行时决定 storage 存储的是指针还是小对象，而不必为每种情况定义不同的数据结构。
快速访问：由于 union 的成员共享同一块内存，访问 storage 的成员通常非常快，因为不需要额外的内存寻址操作。
减少内存分配：对于小对象，可以直接在 buffer 中构造和销毁对象，避免了动态内存分配和释放的开销。
*/
union storage {
  void* ptr_ = nullptr;
  buffer buf_;
};

enum class action { destroy, move };

/*
多态行为：通过模板和静态多态性，可以在不使用虚函数的情况下实现多态行为。这意味着你可以在编译时选择不同的实现，而不需要运行时的动态绑定。
多种操作：handle 函数可以处理多种操作（如销毁和移动），并且可以通过添加新的 case 语句轻松扩展支持的操作类型
避免虚函数：使用静态多态性而不是虚函数可以避免虚拟表查找的开销，提高运行时性能。
内联优化：编译器可以更好地优化静态成员函数的调用，因为这些函数在编译时是已知的。

为什么嵌套 handler_base 在 handler_traits 中？
模板参数：handler_traits 接受 R 和 ArgTypes... 作为模板参数，这使得 handler_traits 可以处理不同返回类型和参数类型的函数。
扩展性：如果将来需要处理不同类型的函数（例如，返回值类型不同或参数列表不同），可以通过 handler_traits 的模板参数轻松扩展。
 */
template <class R, class... ArgTypes>
struct handler_traits {
  template <class Derived>
  struct handler_base {
    static void handle(action act, storage* current, storage* other = nullptr) {
      switch (act) {
        case (action::destroy):
          Derived::destroy(*current);
          break;
        case (action::move):
          Derived::move(*current, *other);
          break;
      }
    }
  };

  template <class T>
  struct small_handler : handler_base<small_handler<T>> {
    template <class... Args>
    static void create(storage& s, Args&&... args) {
      new (static_cast<void*>(&s.buf_)) T(std::forward<Args>(args)...);
    }
    /*
    static_cast<void*>(&s.buf_)：将 s.buf_ 的地址转换为 void* 类型。这是因为 placement new 需要一个 void* 类型的指针来指定内存位置。
    new (address) T(args...)：这是 placement new 的语法，用于在指定的内存位置（address）构造一个 T 类型的对象。
    std::forward<Args>(args)...：使用完美转发将参数 args 转发给 T 的构造函数。完美转发确保了参数的原始类型和值类别（lvalue 或 rvalue）得以保留。
    */

    static void destroy(storage& s) noexcept {
      T& value = *static_cast<T*>(static_cast<void*>(&s.buf_));
      value.~T();
    }
    /*
    static_cast<void*>(&s.buf_)：将 s.buf_ 的地址转换为 void* 类型。
    static_cast<T*>(...)：将 void* 类型的指针转换为 T* 类型的指针。
    *static_cast<T*>(...)：解引用 T* 类型的指针，得到一个 T& 类型的引用 value。
    */

    static void move(storage& dst, storage& src) noexcept {
      create(dst, std::move(*static_cast<T*>(static_cast<void*>(&src.buf_))));
      destroy(src);
    }
    /*
    static_cast<void*>(&src.buf_)：将 src.buf_ 的地址转换为 void* 类型。
    static_cast<T*>(...)：将 void* 类型的指针转换为 T* 类型的指针。
    *static_cast<T*>(...)：解引用 T* 类型的指针，得到一个 T& 类型的引用。
    std::move(...)：将 T& 类型的引用转换为右值引用，以便进行移动构造。
    create(dst, ...)：在 dst 的 buf_ 中创建一个新的 T 对象，使用 src 中的对象进行移动构造。

    为什么要使用 std::move？
    转换为右值引用：
      std::move 将一个左值（lvalue）转换为右值引用（rvalue reference）。这对于调用移动构造函数或移动赋值运算符至关重要。
      在 C++ 中，移动构造函数和移动赋值运算符通常接受右值引用参数，以便进行资源转移而不是复制。
    资源转移：
      移动构造函数和移动赋值运算符的目标是尽可能高效地转移资源，而不是复制资源。通过使用 std::move，我们可以明确告诉编译器我们希望进行资源转移而不是复制。
      例如，对于一个包含动态分配内存的对象，移动构造函数可以简单地交换指针，而不需要复制整个内存块。
    避免不必要的复制：
      如果不使用 std::move，编译器可能会选择调用复制构造函数或复制赋值运算符，这会导致不必要的复制操作，增加了时间和空间开销。
      使用 std::move 确保了编译器选择正确的构造函数或赋值运算符，从而提高性能。
    */

    static R call(storage& s, ArgTypes... args) {
      return std::invoke(*static_cast<T*>(static_cast<void*>(&s.buf_)),
                         std::forward<ArgTypes>(args)...);
    }
    /*
    static_cast<void*>(&s.buf_)：将 s.buf_ 的地址转换为 void* 类型。
    static_cast<T*>(...)：将 void* 类型的指针转换为 T* 类型的指针。
    *static_cast<T*>(...)：解引用 T* 类型的指针，得到一个 T& 类型的引用。
    std::invoke(..., ...)：调用对象的方法。std::invoke 是一个通用的调用机制，可以调用函数、函数对象、成员函数等。
    std::forward<ArgTypes>(args)...：使用完美转发将参数 args 转发给对象的方法，确保参数的原始类型和值类别得以保留。
    */
  };

  template <class T>
  struct large_handler : handler_base<large_handler<T>> {
    template <class... Args>
    static void create(storage& s, Args&&... args) {
      s.ptr_ = new T(std::forward<Args>(args)...);
    }

    static void destroy(storage& s) noexcept { delete static_cast<T*>(s.ptr_); }

    static void move(storage& dst, storage& src) noexcept {
      dst.ptr_ = src.ptr_;
    }

    static R call(storage& s, ArgTypes... args) {
      return std::invoke(*static_cast<T*>(s.ptr_),
                         std::forward<ArgTypes>(args)...);
    }
  };

  template <class T>
  using handler = std::conditional_t<is_small_object_v<T>, small_handler<T>,
                                     large_handler<T>>;
  /*
  如果 is_small_object_v<T> 为 true，则选择 small_handler<T>。
  如果 is_small_object_v<T> 为 false，则选择 large_handler<T>。
  */
};

template <class T>
struct is_in_place_type : std::false_type {};
/*
泛化模板：
这个模板定义了 is_in_place_type 的默认行为。
对于任何类型 T，is_in_place_type<T> 都继承自 std::false_type。
std::false_type 是一个空类，其 value 成员变量为 false。
*/

template <class T>
struct is_in_place_type<std::in_place_type_t<T>> : std::true_type {};
/*
特化模板：
这个模板专门处理 std::in_place_type_t<T> 类型。
当 T 是 std::in_place_type_t<U> 的实例时，is_in_place_type<std::in_place_type_t<T>> 继承自 std::true_type。
std::true_type 是一个空类，其 value 成员变量为 true。
*/

template <class T>
inline constexpr auto is_in_place_type_v = is_in_place_type<T>::value;
/*
作用：提供了一个布尔常量表达式，方便在编译时检查类型。
行为：is_in_place_type_v<T> 是一个布尔常量，表示 T 是否是 std::in_place_type_t 的实例
*/
/*
为什么要这样写？
类型安全：
  通过泛化模板和特化模板的结合，可以在编译时准确地检测某个类型是否是 std::in_place_type_t 的实例。
  这种设计确保了类型安全，避免了在运行时出现类型错误。
编译时优化：
  编译器可以在编译时确定 is_in_place_type<T>::value 的值，从而进行优化。
  这种优化可以减少运行时的开销，提高程序的性能。
代码清晰：
  通过明确的泛化和特化模板，代码的意图更加清晰，易于理解和维护。
  其他开发者可以很容易地理解这个类型特征的作用和用法。

通过这种方式，is_in_place_type 类型特征可以用于检测某个类型是否是 std::in_place_type_t 的实例，
这在使用 std::variant 或其他需要在构造时指定类型的场景中非常有用。这种设计确保了类型安全、编译时优化和代码清晰。
*/


/*
any_invocable_impl 是一个实现泛型可调用对象（类似于 std::function）的类模板。
它允许存储和调用任意类型的可调用对象（如函数、lambda 表达式、函数对象等），并且提供了类型安全和高效的内存管理。
*/
template <class R, bool is_noexcept, class... ArgTypes>
class any_invocable_impl {
  template <class T>
  using handler =
      typename any_detail::handler_traits<R, ArgTypes...>::template handler<T>; // 从 any_detail::handler_traits 获取处理函数

  using storage = any_detail::storage; // 用于存储实际的可调用对象
  using action = any_detail::action;
  using handle_func = void (*)(any_detail::action, any_detail::storage*,
                               any_detail::storage*);         // 处理函数的类型，用于执行各种操作（如移动和销毁）
  using call_func = R (*)(any_detail::storage&, ArgTypes...); // 调用函数的类型，用于调用存储的可调用对象

 public:
  using result_type = R;

  any_invocable_impl() noexcept = default;
  any_invocable_impl(std::nullptr_t) noexcept {}
  any_invocable_impl(any_invocable_impl&& rhs) noexcept { // 移动构造函数
    if (rhs.handle_) {
      handle_ = rhs.handle_;
      handle_(action::move, &storage_, &rhs.storage_);
      call_ = rhs.call_;
      rhs.handle_ = nullptr;
    }
  }

  any_invocable_impl& operator=(any_invocable_impl&& rhs) noexcept {
    any_invocable_impl{std::move(rhs)}.swap(*this);
    return *this;
    /*
    创建临时对象：
      any_invocable_impl{std::move(rhs)}
      这行代码创建了一个临时的 any_invocable_impl 对象，该对象通过移动构造函数从 rhs 移动资源。
      std::move(rhs) 将 rhs 转换为右值引用，使其可以被移动。
    交换资源：
      .swap(*this)
      这行代码调用临时对象的 swap 方法，将临时对象的资源与当前对象的资源交换。
      交换后，当前对象 (*this) 持有了 rhs 的资源，而临时对象持有了当前对象原来的资源。

    为啥不能这样写：this->swap(rhs):
      直接使用 this->swap(rhs) 来实现移动赋值运算符虽然看起来更简单，但在某些情况下可能会出现问题，
      特别是当 *this 和 rhs 是同一个对象时。这种情况下，直接交换会导致未定义行为。让我们详细解释为什么不能这样写，并展示如何避免这些问题。
      使用临时对象和 swap 方法可以避免自赋值问题，因为 swap 方法不会改变对象的内容，只是交换资源。
    */
  }
  any_invocable_impl& operator=(std::nullptr_t) noexcept {
    destroy();
    return *this;
  }
  /*
  通过专门的赋值运算符 operator=(std::nullptr_t)，可以明确地将对象设为空（即不持有任何可调用对象），
  避免了使用其他方式（如 operator=(any_invocable_impl&&)）可能导致的歧义, 
  返回当前对象的引用 return *this; 支持链式赋值操作，例如 a = b = nullptr;
  */

  ~any_invocable_impl() { destroy(); }

  void swap(any_invocable_impl& rhs) noexcept {
    if (handle_) {
      if (rhs.handle_) {
        storage tmp;
        handle_(action::move, &tmp, &storage_);
        rhs.handle_(action::move, &storage_, &rhs.storage_);
        handle_(action::move, &rhs.storage_, &tmp);
        std::swap(handle_, rhs.handle_);
        std::swap(call_, rhs.call_);
      } else {
        // 如果右侧对象 rhs 没有资源（即 rhs.handle_ 为 nullptr），则调用 rhs 的 swap 方法，将 rhs 的资源交换给当前对象 *this
        rhs.swap(*this);
      }
    } else if (rhs.handle_) {
      // 如果当前对象 *this 没有资源（即 handle_ 为 nullptr），但右侧对象 rhs 持有资源，则将右侧对象的资源移动到当前对象的存储中
      rhs.handle_(action::move, &storage_, &rhs.storage_);
      handle_ = rhs.handle_;
      call_ = rhs.call_;
      rhs.handle_ = nullptr;
    }
  }

  // noexcept 标记确保该操作不会抛出异常，这对于异常安全的代码非常重要
  explicit operator bool() const noexcept { return handle_ != nullptr; }

 protected:
  template <class F, class... Args> // 定义了一个模板函数，其中 F 是要存储的可调用对象的类型，Args 是传递给构造函数的参数包
  void create(Args&&... args) {
    using hdl = handler<F>; // handler<F> 是一个特定于可调用对象类型的处理类，负责创建和管理可调用对象的资源
    hdl::create(storage_, std::forward<Args>(args)...);
    handle_ = &hdl::handle;
    call_ = &hdl::call;
  }

  void destroy() noexcept {
    if (handle_) {
      handle_(action::destroy, &storage_, nullptr);
      handle_ = nullptr;
    }
  }

  R call(ArgTypes... args) noexcept(is_noexcept) {
    return call_(storage_, std::forward<ArgTypes>(args)...);
  }

  /* 这些友元函数使得 any_invocable_impl 对象可以像指针一样与 nullptr 进行比较 */
  /*
  1. 为什么使用 friend 关键字？
  通过 friend 使得比较或运算符能够直接访问类的内部状态，而不需要公开该状态。
  访问私有成员：any_invocable_impl 类的私有成员（如 handle_ 和 call_）需要在比较操作中被访问。通过将这些比较操作声明为友元函数，这些函数可以直接访问 any_invocable_impl 的私有成员。
  外部定义：这些比较操作不是 any_invocable_impl 类的成员函数，而是独立的全局函数。通过使用 friend 关键字，可以在类内部声明这些函数，并使它们能够访问类的私有成员。

  非成员函数的形式更符合常规的比较运算符定义方式

  2. 为什么 const any_invocable_impl& f 有时在参数 1，有时在参数 2？
  在 C++ 中，重载比较运算符时，通常需要考虑两个方向的比较：

  any_invocable_impl 对象与 nullptr 比较。
  nullptr 与 any_invocable_impl 对象比较。
  为了确保这两个方向的比较都能正确工作，需要定义两个重载版本的比较运算符。这样可以确保无论 nullptr 出现在哪个位置，都能正确进行比较。

  friend bool operator==(const any_invocable_impl& f, std::nullptr_t) noexcept: 这个重载允许你以 f == nullptr 的方式来进行比较。
  friend bool operator==(std::nullptr_t, const any_invocable_impl& f) noexcept: 这个重载允许你以 nullptr == f 的方式进行比较。
  这样做的目的是为了让用户能够以任意顺序使用 == 运算符进行比较，不用担心顺序是否符合 f == nullptr 还是 nullptr == f，两种形式都会被支持。
  */

  /*
  这两个函数用于检查 any_invocable_impl 对象是否等于 nullptr。它们的工作原理是：
    将 any_invocable_impl 对象隐式转换为布尔值;
    如果转换后的布尔值为 false，则对象被视为 nullptr
  */
  friend bool operator==(const any_invocable_impl& f, std::nullptr_t) noexcept {
    return !f;
  }
  friend bool operator==(std::nullptr_t, const any_invocable_impl& f) noexcept {
    return !f;
  }
  /*
  这两个函数用于检查 any_invocable_impl 对象是否不等于 nullptr。它们的工作原理是：
    将 any_invocable_impl 对象隐式转换为布尔值。
    如果转换后的布尔值为 true，则对象不被视为 nullptr。
  */
  friend bool operator!=(const any_invocable_impl& f, std::nullptr_t) noexcept {
    return static_cast<bool>(f);
  }
  friend bool operator!=(std::nullptr_t, const any_invocable_impl& f) noexcept {
    return static_cast<bool>(f);
  }

  friend void swap(any_invocable_impl& lhs, any_invocable_impl& rhs) noexcept {
    lhs.swap(rhs);
  }

 private:
  storage storage_;
  handle_func handle_ = nullptr;
  call_func call_;
};

template <class T>
using remove_cvref_t = std::remove_cv_t<std::remove_reference_t<T>>;
/*
std::remove_reference：
  std::remove_reference 是一个类型特征，用于去除类型的引用部分。
  例如，std::remove_reference_t<int&> 的结果是 int。
  std::remove_reference_t<int&&> 的结果也是 int。
std::remove_cv：
  std::remove_cv 是一个类型特征，用于去除类型的 cv 修饰符（const 和 volatile）。
  例如，std::remove_cv_t<const int> 的结果是 int。
  std::remove_cv_t<volatile int> 的结果也是 int。
  std::remove_cv_t<const volatile int> 的结果同样是 int。
组合使用：
  remove_cvref_t 结合了 std::remove_reference 和 std::remove_cv，首先去除引用，然后去除 cv 修饰符。
  例如，remove_cvref_t<const int&> 的结果是 int。
  remove_cvref_t<volatile int&&> 的结果也是 int。

remove_cvref_t 是一个非常有用的类型别名，它结合了 std::remove_reference 和 std::remove_cv，
用于去除类型中的所有顶层 cv 修饰符和引用。这在模板编程中特别有用，特别是在处理通用引用时，可以确保类型的一致性和简洁性。
*/

template <class AI, class F, bool noex, class R, class FCall, class... ArgTypes>
using can_convert = std::conjunction<
    std::negation<std::is_same<remove_cvref_t<F>, AI>>,
    std::negation<any_detail::is_in_place_type<remove_cvref_t<F>>>,
    std::is_invocable_r<R, FCall, ArgTypes...>,
    std::bool_constant<(!noex ||
                        std::is_nothrow_invocable_r_v<R, FCall, ArgTypes...>)>,
    std::is_constructible<std::decay_t<F>, F>>;

}  // namespace any_detail
/*
std::conjunction：
  std::conjunction 是一个模板元编程工具，用于检查一组布尔类型的类型特征是否都为 true。
  如果所有条件都为 true，则 std::conjunction 的结果为 true，否则为 false。
条件列表：
  std::negation<std::is_same<remove_cvref_t<F>, AI>>：
    检查 F 去除 cv 修饰符和引用后是否与 AI 类型相同。
    使用 std::negation 取反，确保 F 不是 AI 类型。
  std::negation<any_detail::is_in_place_type<remove_cvref_t<F>>>：
    检查 F 去除 cv 修饰符和引用后是否是一个“就地类型”（in-place type）。
    使用 std::negation 取反，确保 F 不是“就地类型”。
  std::is_invocable_r<R, FCall, ArgTypes...>：
    检查 FCall 是否可以接受 ArgTypes... 并返回类型 R。
  std::bool_constant<(!noex || std::is_nothrow_invocable_r_v<R, FCall, ArgTypes...>)>：
    检查 FCall 是否可以无异常地调用，或者 noex 为 false。
    使用 std::bool_constant 包装布尔表达式的结果。
  std::is_constructible<std::decay_t<F>, F>：
    检查 F 是否可以构造为 std::decay_t<F> 类型。
    std::decay_t 去除 F 的引用和 cv 修饰符，并将其转换为普通类型。
*/

template <class Signature>
class any_invocable;

/*
定义了一个通用的 any_invocable 类模板，该模板支持多种函数签名和调用约定。通过宏 __OFATS_ANY_INVOCABLE，你可以生成不同组合的 any_invocable 类模板实例

宏定义：
  __OFATS_ANY_INVOCABLE 是一个宏，用于生成 any_invocable 类模板的不同实例。
  参数 cv 控制是否添加 const 修饰符。
  参数 ref 控制是否添加引用修饰符（& 或 &&）。
  参数 noex 控制是否添加 noexcept 修饰符。
  参数 inv_quals 控制调用操作符的调用约定。

类模板：
  any_invocable 继承自 any_detail::any_invocable_impl，并使用 base_type 别名简化基类的引用。
  提供了多种构造函数和赋值操作符，确保 any_invocable 可以接受各种类型的函数对象。

构造函数：
  通用构造函数：
    template <class F, class = std::enable_if_t<any_detail::can_convert<...>::value>> any_invocable(F&& f)
    使用 std::decay_t 和 std::forward 确保 F 可以正确地传递给基类的 create 方法。
  就地构造函数：
    template <class T, class... Args, class VT = std::decay_t<T>, class = std::enable_if_t<...>> explicit any_invocable(std::in_place_type_t<T>, Args&&... args)
    使用 std::in_place_type_t 和 std::forward 就地构造函数对象。
  初始化列表构造函数：
    template <class T, class U, class... Args, class VT = std::decay_t<T>, class = std::enable_if_t<...>> explicit any_invocable(std::in_place_type_t<T>, std::initializer_list<U> il, Args&&... args)
    支持使用初始化列表构造函数对象。

赋值操作符：
  通用赋值操作符：
    template <class F, class FDec = std::decay_t<F>> std::enable_if_t<!std::is_same_v<FDec, any_invocable> && std::is_move_constructible_v<FDec>, any_invocable&> operator=(F&& f)
    使用临时对象和 swap 方法实现赋值操作。
  引用包装赋值操作符：
    template <class F> any_invocable& operator=(std::reference_wrapper<F> f)
    支持使用 std::reference_wrapper 进行赋值。

调用操作符：
  R operator()(ArgTypes... args) cv ref noexcept(noex)
  调用基类的 call 方法，传递参数并返回结果。
*/
#define __OFATS_ANY_INVOCABLE(cv, ref, noex, inv_quals)                        \
  template <class R, class... ArgTypes>                                        \
  class any_invocable<R(ArgTypes...) cv ref noexcept(noex)>                    \
      : public any_detail::any_invocable_impl<R, noex, ArgTypes...> {          \
    using base_type = any_detail::any_invocable_impl<R, noex, ArgTypes...>;    \
                                                                               \
   public:                                                                     \
    using base_type::base_type;                                                \
                                                                               \
    template <                                                                 \
        class F,                                                               \
        class = std::enable_if_t<any_detail::can_convert<                      \
            any_invocable, F, noex, R, F inv_quals, ArgTypes...>::value>>      \
    any_invocable(F&& f) {                                                     \
      base_type::template create<std::decay_t<F>>(std::forward<F>(f));         \
    }                                                                          \
                                                                               \
    template <class T, class... Args, class VT = std::decay_t<T>,              \
              class = std::enable_if_t<                                        \
                  std::is_move_constructible_v<VT> &&                          \
                  std::is_constructible_v<VT, Args...> &&                      \
                  std::is_invocable_r_v<R, VT inv_quals, ArgTypes...> &&       \
                  (!noex || std::is_nothrow_invocable_r_v<R, VT inv_quals,     \
                                                          ArgTypes...>)>>      \
    explicit any_invocable(std::in_place_type_t<T>, Args&&... args) {          \
      base_type::template create<VT>(std::forward<Args>(args)...);             \
    }                                                                          \
                                                                               \
    template <                                                                 \
        class T, class U, class... Args, class VT = std::decay_t<T>,           \
        class = std::enable_if_t<                                              \
            std::is_move_constructible_v<VT> &&                                \
            std::is_constructible_v<VT, std::initializer_list<U>&, Args...> && \
            std::is_invocable_r_v<R, VT inv_quals, ArgTypes...> &&             \
            (!noex ||                                                          \
             std::is_nothrow_invocable_r_v<R, VT inv_quals, ArgTypes...>)>>    \
    explicit any_invocable(std::in_place_type_t<T>,                            \
                           std::initializer_list<U> il, Args&&... args) {      \
      base_type::template create<VT>(il, std::forward<Args>(args)...);         \
    }                                                                          \
                                                                               \
    template <class F, class FDec = std::decay_t<F>>                           \
    std::enable_if_t<!std::is_same_v<FDec, any_invocable> &&                   \
                         std::is_move_constructible_v<FDec>,                   \
                     any_invocable&>                                           \
    operator=(F&& f) {                                                         \
      any_invocable{std::forward<F>(f)}.swap(*this);                           \
      return *this;                                                            \
    }                                                                          \
    template <class F>                                                         \
    any_invocable& operator=(std::reference_wrapper<F> f) {                    \
      any_invocable{f}.swap(*this);                                            \
      return *this;                                                            \
    }                                                                          \
                                                                               \
    R operator()(ArgTypes... args) cv ref noexcept(noex) {                     \
      return base_type::call(std::forward<ArgTypes>(args)...);                 \
    }                                                                          \
  };

// cv -> {`empty`, const}
// ref -> {`empty`, &, &&}
// noex -> {true, false}
// inv_quals -> (is_empty(ref) ? & : ref)
__OFATS_ANY_INVOCABLE(, , false, &)               // 000
__OFATS_ANY_INVOCABLE(, , true, &)                // 001
__OFATS_ANY_INVOCABLE(, &, false, &)              // 010
__OFATS_ANY_INVOCABLE(, &, true, &)               // 011
__OFATS_ANY_INVOCABLE(, &&, false, &&)            // 020
__OFATS_ANY_INVOCABLE(, &&, true, &&)             // 021
__OFATS_ANY_INVOCABLE(const, , false, const&)     // 100
__OFATS_ANY_INVOCABLE(const, , true, const&)      // 101
__OFATS_ANY_INVOCABLE(const, &, false, const&)    // 110
__OFATS_ANY_INVOCABLE(const, &, true, const&)     // 111
__OFATS_ANY_INVOCABLE(const, &&, false, const&&)  // 120
__OFATS_ANY_INVOCABLE(const, &&, true, const&&)   // 121

#undef __OFATS_ANY_INVOCABLE

}  // namespace ofats

/* We, uWebSockets define our own type */
namespace uWS {
  template <class T>
  using MoveOnlyFunction = ofats::any_invocable<T>;
}

#endif  // _ANY_INVOKABLE_H_
