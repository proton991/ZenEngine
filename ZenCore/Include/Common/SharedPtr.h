#pragma once
#include <atomic>

namespace zen {
class RefCounter {
public:
  RefCounter() { m_count.store(1, std::memory_order_relaxed); }
  inline void AddRef() { ++m_count; }
  inline void DecRef() { --m_count; }
  inline auto GetValue() const { return m_count.load(); }

private:
  std::atomic_uint m_count;
};

template <class T>
class SharedPtr {
public:
  SharedPtr() {}
  //Constructor
  SharedPtr(T* object) : m_ptr{object}, m_refCount{new RefCounter()} { m_refCount->AddRef(); }
  //Destructor
  virtual ~SharedPtr() { Release(); }

  // Move Constructor
  SharedPtr(SharedPtr&& other) noexcept
      : m_ptr(std::move(other.m_ptr)), m_refCount(std::move(other.m_refCount)) {
    other.m_ptr      = nullptr;
    other.m_refCount = nullptr;
  }

  SharedPtr& operator=(SharedPtr&& other) noexcept {
    Release();
    m_ptr      = std::move(other.m_ptr);
    m_refCount = std::move(other.m_refCount);

    other.m_ptr      = nullptr;
    other.m_refCount = nullptr;
    return *this;
  }

  // Copy Constructor
  SharedPtr(const SharedPtr& other) noexcept : m_ptr{other.m_ptr}, m_refCount{other.m_refCount} {
    if (m_ptr != nullptr) {
      m_refCount->AddRef();
    }
  }

  // Overloaded Assignment Operator
  SharedPtr& operator=(const SharedPtr& other) noexcept {
    Release();
    m_ptr      = other.m_ptr;
    m_refCount = other.m_refCount;
    if (m_ptr != nullptr) {
      m_refCount->AddRef();
    }

    return *this;
  }

  // Dereference operator
  inline T& operator*() { return *m_ptr; }
  //Const Member Access operator
  inline const T* operator->() const { return m_ptr; }

  // Member Access operator
  inline T* operator->() { return m_ptr; }
  // Const Dereference operator
  inline const T& operator*() const { return *m_ptr; }

  inline operator bool() const noexcept { return m_ptr != nullptr; }

  inline T* Get() noexcept { return m_ptr; }

  inline const T* Get() const noexcept { return m_ptr; }

  void swap(SharedPtr& rhs) noexcept { std::swap(m_ptr, rhs.ptr); }

private:
  inline void Release() noexcept {
    if (m_refCount) {
      m_refCount->DecRef();
      if (m_refCount->GetValue() <= 0) {
        delete m_refCount;
        delete m_ptr;
        m_refCount = nullptr;
        m_ptr      = nullptr;
      }
    }
  }
  T* m_ptr{nullptr};
  RefCounter* m_refCount{nullptr};
};

template <class T, class... Args>
SharedPtr<T> MakeShared(Args&&... args_) {
  return SharedPtr<T>(new T(std::forward<Args>(args_)...));
}

// comparison operators
template <class T, class U>
inline bool operator==(const SharedPtr<T>& l, const SharedPtr<U>& r) noexcept  // never throws
{
  return (l.Get() == r.Get());
}
template <class T, class U>
inline bool operator!=(const SharedPtr<T>& l, const SharedPtr<U>& r) noexcept  // never throws
{
  return (l.Get() != r.Get());
}
template <class T, class U>
inline bool operator<=(const SharedPtr<T>& l, const SharedPtr<U>& r) noexcept  // never throws
{
  return (l.Get() <= r.Get());
}
template <class T, class U>
inline bool operator<(const SharedPtr<T>& l, const SharedPtr<U>& r) noexcept  // never throws
{
  return (l.Get() < r.Get());
}
template <class T, class U>
inline bool operator>=(const SharedPtr<T>& l, const SharedPtr<U>& r) noexcept  // never throws
{
  return (l.Get() >= r.Get());
}
template <class T, class U>
inline bool operator>(const SharedPtr<T>& l, const SharedPtr<U>& r) noexcept  // never throws
{
  return (l.Get() > r.Get());
}
}  // namespace zen