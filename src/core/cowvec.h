// A vector whose contents are shared between copies until one of them is changed. Copying one is cheap (a pointer);
// reading never copies; the first change through a copy that shares the contents makes the copy its own.
#pragma once
#include <atomic>
#include <cstddef>
#include <initializer_list>
#include <memory>
#include <vector>

namespace reno {

// How many times a CowVec copied its contents because they were shared (for measuring: ui_scale_test prints it).
struct CowStats {
    static std::atomic<long>& copies() { static std::atomic<long> n{0}; return n; }
};

template <class T>
class CowVec {
public:
    typedef T value_type;
    typedef T* iterator;
    typedef const T* const_iterator;
    typedef std::size_t size_type;

    CowVec() {}
    CowVec(const std::vector<T>& v) { if (!v.empty()) d_ = std::make_shared<std::vector<T>>(v); }
    CowVec(std::vector<T>&& v) { if (!v.empty()) d_ = std::make_shared<std::vector<T>>(std::move(v)); }
    CowVec(std::initializer_list<T> l) { if (l.size()) d_ = std::make_shared<std::vector<T>>(l); }
    CowVec(const_iterator a, const_iterator b) { if (a != b) d_ = std::make_shared<std::vector<T>>(a, b); }
    CowVec& operator=(const std::vector<T>& v) { d_ = v.empty() ? nullptr : std::make_shared<std::vector<T>>(v); return *this; }
    CowVec& operator=(std::vector<T>&& v) { d_ = v.empty() ? nullptr : std::make_shared<std::vector<T>>(std::move(v)); return *this; }
    CowVec& operator=(std::initializer_list<T> l) { d_ = l.size() ? std::make_shared<std::vector<T>>(l) : nullptr; return *this; }

    // ---- reading (never copies)
    size_type size() const { return d_ ? d_->size() : 0; }
    bool empty() const { return !d_ || d_->empty(); }
    const T& operator[](size_type i) const { return (*d_)[i]; }
    const T& at(size_type i) const { return d_->at(i); }
    const T& front() const { return d_->front(); }
    const T& back() const { return d_->back(); }
    const T* begin() const { return d_ ? d_->data() : nullptr; }
    const T* end() const { return d_ ? d_->data() + d_->size() : nullptr; }
    const T* cbegin() const { return begin(); }
    const T* cend() const { return end(); }
    const T* data() const { return begin(); }
    const std::vector<T>& cvec() const { return d_ ? *d_ : none(); }
    operator const std::vector<T>&() const { return cvec(); }
    // Whether the contents are shared with another copy (a change would copy them), and whether two share the same.
    bool shared() const { return d_ && d_.use_count() > 1; }
    bool sameContents(const CowVec& o) const { return d_ == o.d_; }
    // A number that stands for these contents while they are not changed (the address of the vector holding them):
    // equal for copies that share, so caches can tell "the same events" without comparing them.
    const void* identity() const { return d_.get(); }

    // ---- changing (the contents are made this copy's own first)
    T& operator[](size_type i) { return own()[i]; }
    T& front() { return own().front(); }
    T& back() { return own().back(); }
    T* begin() { return d_ ? own().data() : nullptr; }
    T* end() { if (!d_) return nullptr; std::vector<T>& v = own(); return v.data() + v.size(); }
    T* data() { return begin(); }
    std::vector<T>& mut() { return own(); }
    void push_back(const T& x) { own().push_back(x); }
    void push_back(T&& x) { own().push_back(std::move(x)); }
    template <class... A>
    T& emplace_back(A&&... a) { return own().emplace_back(std::forward<A>(a)...); }
    void pop_back() { own().pop_back(); }
    void clear() { d_.reset(); }
    void reserve(size_type n) { own().reserve(n); }
    void resize(size_type n) { own().resize(n); }
    void resize(size_type n, const T& x) { own().resize(n, x); }
    void swap(CowVec& o) { d_.swap(o.d_); }
    T* insert(const T* pos, const T& x)
    {
        const std::ptrdiff_t k = pos - cdata();
        std::vector<T>& v = own();
        return &*v.insert(v.begin() + k, x);
    }
    T* insert(const T* pos, T&& x)
    {
        const std::ptrdiff_t k = pos - cdata();
        std::vector<T>& v = own();
        return &*v.insert(v.begin() + k, std::move(x));
    }
    template <class It>
    T* insert(const T* pos, It a, It b)
    {
        const std::ptrdiff_t k = pos - cdata();
        std::vector<T>& v = own();
        return &*v.insert(v.begin() + k, a, b);
    }
    T* erase(const T* pos)
    {
        const std::ptrdiff_t k = pos - cdata();
        std::vector<T>& v = own();
        return &*v.erase(v.begin() + k);
    }
    T* erase(const T* a, const T* b)
    {
        const std::ptrdiff_t k = a - cdata(), n = b - a;
        std::vector<T>& v = own();
        return &*v.erase(v.begin() + k, v.begin() + k + n);
    }
    template <class It>
    void assign(It a, It b) { d_ = std::make_shared<std::vector<T>>(a, b); }

private:
    // (the start of the contents as the const side sees them: positions given to insert/erase are measured from it;
    // they were got from this vector's begin() - which already made it unshared - so it is the same memory)
    const T* cdata() const { return d_ ? d_->data() : nullptr; }
    static const std::vector<T>& none() { static const std::vector<T> v; return v; }
    std::vector<T>& own()
    {
        if (!d_) d_ = std::make_shared<std::vector<T>>();
        else if (d_.use_count() > 1) {
            CowStats::copies().fetch_add(1, std::memory_order_relaxed);
            d_ = std::make_shared<std::vector<T>>(*d_);
        }
        return *d_;
    }
    std::shared_ptr<std::vector<T>> d_;
};

}  // namespace reno
