#pragma once
#include <cassert>
template <class T>
class UniquePtr {
public:
  UniquePtr() noexcept : m_ptr(nullptr){};
  explicit UniquePtr(T* ptr) noexcept : m_ptr(ptr) {}
  // disable copy
  UniquePtr(const UniquePtr&)            = delete;
  UniquePtr& operator=(const UniquePtr&) = delete;
  ~UniquePtr() noexcept { Destroy(); }

  UniquePtr(UniquePtr&& other) noexcept : m_ptr(other.Release()) {}

  UniquePtr& operator=(UniquePtr&& other) noexcept {
    Reset(other.Release());
    return *this;
  }

  inline void Reset(T* ptr) noexcept {
    assert((nullptr == ptr) || (ptr != m_ptr));
    if (m_ptr != ptr) {
      if (m_ptr) {
        Destroy();
      }
      m_ptr = ptr;
    }
    Destroy();
  }

  T* Release() noexcept {
    T* orignial = m_ptr;
    m_ptr       = nullptr;
    return orignial;
  }

  void swap(UniquePtr& rhs) noexcept { std::swap(m_ptr, rhs.ptr); }

  inline T* operator->() noexcept {
    assert(m_ptr != nullptr);
    return m_ptr;
  }

  inline const T* operator->() const noexcept {
    assert(m_ptr != nullptr);
    return m_ptr;
  }

  inline T& operator*() noexcept {
    assert(m_ptr != nullptr);
    return *m_ptr;
  }

  inline const T& operator*() const noexcept {
    assert(m_ptr != nullptr);
    return *m_ptr;
  }

  inline operator bool() const noexcept { return m_ptr != nullptr; }

  inline T* Get() noexcept { return m_ptr; }

  inline const T* Get() const noexcept { return m_ptr; }

private:
  inline void Destroy() noexcept {
    delete m_ptr;
    m_ptr = nullptr;
  }
  T* m_ptr;
};

template <class T>
inline void swap(const UniquePtr<T>& lhs, const UniquePtr<T>& rhs) noexcept {
  lhs.swap(rhs);
}

template <class T, class... Args>
UniquePtr<T> MakeUnique(Args&&... args_) {
  return UniquePtr<T>(new T(std::forward<Args>(args_)...));
}

// comparison operators
template <class T, class U>
inline bool operator==(const UniquePtr<T>& l, const UniquePtr<U>& r) noexcept  // never throws
{
  return (l.Get() == r.Get());
}
template <class T, class U>
inline bool operator!=(const UniquePtr<T>& l, const UniquePtr<U>& r) noexcept  // never throws
{
  return (l.Get() != r.Get());
}
template <class T, class U>
inline bool operator<=(const UniquePtr<T>& l, const UniquePtr<U>& r) noexcept  // never throws
{
  return (l.Get() <= r.Get());
}
template <class T, class U>
inline bool operator<(const UniquePtr<T>& l, const UniquePtr<U>& r) noexcept  // never throws
{
  return (l.Get() < r.Get());
}
template <class T, class U>
inline bool operator>=(const UniquePtr<T>& l, const UniquePtr<U>& r) noexcept  // never throws
{
  return (l.Get() >= r.Get());
}
template <class T, class U>
inline bool operator>(const UniquePtr<T>& l, const UniquePtr<U>& r) noexcept  // never throws
{
  return (l.Get() > r.Get());
}