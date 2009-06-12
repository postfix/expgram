// -*- mode: c++ -*-
#ifndef __UTILS__SIMPLE_VECTOR__HPP__
#define __UTILS__SIMPLE_VECTOR__HPP__ 1

#include <memory>
#include <iterator>
#include <vector>
#include <stdexcept>

#include <boost/type_traits.hpp>

#include <utils/memory.hpp>

namespace utils {
  
  
  template <typename _Alloc >
  struct __simple_vector_alloc_impl
  {
    typedef size_t      size_type;
    typedef ptrdiff_t   difference_type;
    typedef char*       pointer;
    
    typedef typename _Alloc::template rebind<char >::other allocator_type;
  };
  
  template <typename Tp, typename _Alloc>
  struct __simple_vector_base : public __simple_vector_alloc_impl<_Alloc>::allocator_type
  {
    typedef __simple_vector_alloc_impl<_Alloc> impl_type;
    
    typedef       Tp  value_type;
    typedef       Tp* iterator;
    typedef const Tp* const_iterator;
    typedef       Tp& reference;
    typedef const Tp& const_reference;
    
    typedef typename impl_type::size_type       size_type;
    typedef typename impl_type::difference_type difference_type;
    typedef typename impl_type::pointer         pointer;
    typedef typename impl_type::allocator_type  allocator_type;
    
    __simple_vector_base() : __base(0) {}
    __simple_vector_base(size_type __n) : __base(0) { initialize_vector(__n); }
    ~__simple_vector_base()
      {
	if (__base) {
	  const size_type __size = *(reinterpret_cast<size_type*>(__base));
	  allocator().deallocate(__base, __size * sizeof(value_type) + sizeof(size_type));
	}
      }
    
    void swap(__simple_vector_base& x)
    {
      std::swap(__base, x.__base);
    }

    inline const_iterator begin() const { return reinterpret_cast<const_iterator>(__base + sizeof(size_type)); }
    inline       iterator begin()       { return reinterpret_cast<iterator>(__base + sizeof(size_type)); }
    inline const_iterator end() const { return begin() + size(); }
    inline       iterator end()       { return begin() + size(); }
    
    bool empty() const { return size() == 0; }
    size_type size() const { return *(reinterpret_cast<size_type*>(__base)); }
    
    void initialize_vector(size_type __n)
    {
      __base = allocator().allocate(__n * sizeof(value_type) + sizeof(size_type));
      *(reinterpret_cast<size_type*>(__base)) = __n;
    }
    
    const allocator_type& allocator() const { return static_cast<allocator_type&>(*this); }
    allocator_type& allocator() { return static_cast<allocator_type&>(*this); }
    
  private:
    pointer __base;
  };
  
  template <typename Tp, typename Alloc=std::allocator<Tp> >
  class simple_vector
  {
  private:
    typedef __simple_vector_base<Tp, Alloc> base_type;

  public:
    typedef Tp                                  value_type;
    typedef typename base_type::size_type       size_type;
    typedef typename base_type::difference_type difference_type;
    
    typedef typename base_type::reference       reference;
    typedef typename base_type::const_reference const_reference;

    typedef typename base_type::iterator       iterator;
    typedef typename base_type::const_iterator const_iterator;
    
    typedef typename std::reverse_iterator<iterator>       reverse_iterator;
    typedef typename std::reverse_iterator<const_iterator> const_reverse_iterator;
    
  public:
    simple_vector() : __base(0) {}
    simple_vector(size_type __n) : __base(__n) { std::uninitialized_fill(__base.begin(), __base.end(), value_type()); }
    simple_vector(const size_type __n, const Tp& __value) : __base(__n) { std::uninitialized_fill(__base.begin(), __base.end(), __value); }
    
    template <typename Iterator>
    simple_vector(Iterator first, Iterator last) : __base() {
      typedef typename boost::is_integral<Iterator>::type __integral;
      __initialize_dispatch(first, last, __integral());
    }
    simple_vector(const simple_vector& x) : __base(x.size()) { std::uninitialized_copy(x.begin(), x.end(), __base.begin()); }
    
    ~simple_vector() { utils::destroy_range(__base.begin(), __base.end()); }
    
    simple_vector& operator=(const simple_vector& x)
    {
      __range_assign(x.begin(), x.end());
      return *this;
    }
    
  public:
    void assign(size_type __n, const Tp& __value) { __fill_assign(__n, __value); }
    void assign(const simple_vector& x) { __range_assign(x.begin(), x.end()); }
    template <typename Iterator>
    void assign(Iterator first, Iterator last)
    { 
      typedef typename boost::is_integral<Iterator>::type __integral;
      __assign_dispatch(first, last, __integral());
    }
    
  public:
    bool empty() const { return __base.empty(); }
    size_type size() const { return __base.size(); }
    
    reference       operator[](size_type __n)       { return *(begin() + __n); }
    const_reference operator[](size_type __n) const { return *(begin() + __n); }
    
    reference front() { return *(begin()); }
    const_reference front() const { return *(begin()); }
    reference back() { return *(end() - 1); }
    const_reference back() const { return *(end() - 1); }
    
    const_iterator begin() const { return __base.begin(); }
    iterator begin() { return __base.begin(); }
    
    const_iterator end() const { return __base.end(); }
    iterator end() { return __base.end(); }
    
    reverse_iterator rbegin() { return reverse_iterator(end()); }
    const_reverse_iterator rbegin() const { return const_reverse_iterator(end()); }
    reverse_iterator rend() { return reverse_iterator(begin()); }
    const_reverse_iterator rend() const { return const_reverse_iterator(begin()); }
    
  public:
    void swap(simple_vector& x) { __base.swap(x.__base); }
    
    void clear() { resize(0); }
    void resize(size_type __n) { resize(__n, value_type()); }
    void resize(size_type __n, const Tp& __value) {
      const size_type __size = size();
      
      if (__n < __size) {
	base_type __base_new(__n);
	std::uninitialized_copy(__base.begin(), __base.begin() + __n, __base_new.begin());
	__base.swap(__base_new);
	utils::destroy_range(__base_new.begin(), __base_new.end());
      } else if (__n > __size) {
	base_type __base_new(__n);
	std::uninitialized_copy(__base.begin(), __base.end(), __base_new.begin());
	std::uninitialized_fill(__base_new.begin() + __size, __base_new.end(), __value);
	__base.swap(__base_new);
	utils::destroy_range(__base_new.begin(), __base_new.end());
      }
    }
    
  private:

    template <typename Integer>
    void __assign_dispatch(Integer n, Integer x, boost::true_type)
    {
      __fill_assign(n, x);
    }
    
    template <typename _InputIterator>
    void __assign_dispatch(_InputIterator first, _InputIterator last, boost::false_type)
    {
      typedef typename std::iterator_traits<_InputIterator>::iterator_category __category;
      __range_assign(first, last, __category());
    }
    
    void __fill_assign(size_type __n, const Tp& __value)
    {
      const size_type __size = size();
      
      if (__n == __size)
	std::fill(begin(), end(), __value);
      else {
	base_type __base_new(__n);
	std::uninitialized_fill(__base_new.begin(), __base_new.end(), __value);
	__base.swap(__base_new);
	utils::destroy_range(__base_new.begin(), __base_new.end());
      }
    }
    
    template <typename ForwardIterator>
    void __range_assign(ForwardIterator first, ForwardIterator last, std::forward_iterator_tag)
    {
      const size_type __n = std::distance(first, last);
      const size_type __size = size();
      
      if (__n == __size)
	std::copy(first, last, __base.begin());
      else {
	base_type __base_new(__n);
	std::uninitialized_copy(first, last, __base_new.begin());
	__base.swap(__base_new);
	utils::destroy_range(__base_new.begin(), __base_new.end());
      }
    }

    template <typename InputIterator>
    void __range_assign(InputIterator first, InputIterator last, std::input_iterator_tag)
    {
      // we will first allocate at temporary buffer...
      std::vector<Tp, Alloc> vec(first, last);
      __range_assign(vec.begin(), vec.end(), std::forward_iterator_tag());
    }
    
    template <typename Iterator>
    void __range_assign(Iterator first, Iterator last)
    {
      typedef typename std::iterator_traits<Iterator>::iterator_category __category;
      __range_assign(first, last, __category());
    }
    
    template <typename _InputIterator>
    void __range_initialize(_InputIterator first, _InputIterator last, std::input_iterator_tag)
    {
      std::vector<Tp, Alloc> vec(first, last);
      __range_initialize(vec.begin(), vec.end(), std::forward_iterator_tag());
    }
    
    template <typename _ForwardIterator>
    void __range_initialize(_ForwardIterator first, _ForwardIterator last, std::forward_iterator_tag)
    {
      const size_type __n = std::distance(first, last);
      __base.initialize_vector(__n);
      std::uninitialized_copy(first, last, __base.begin());
    }

    template <typename Integer>
    void __initialize_dispatch(Integer n, Integer x, boost::true_type)
    {
      __base.initialize_vector(n);
      std::uninitialized_fill(__base.begin(), __base.end(), x);
    }
    
    template <typename _InputIterator>
    void __initialize_dispatch(_InputIterator first, _InputIterator last, boost::false_type)
    {
      typedef typename std::iterator_traits<_InputIterator>::iterator_category __category;
      __range_initialize(first, last, __category());
    }
    
  private:
    base_type __base;
  };
  
  template <typename Tp, typename Alloc>
  inline
  bool operator==(const simple_vector<Tp, Alloc>& x,
		  const simple_vector<Tp, Alloc>& y)
  {
    return x.size() == y.size() && std::equal(x.begin(), x.end(), y.begin());
  }
  template <typename Tp, typename Alloc>
  inline
  bool operator!=(const simple_vector<Tp, Alloc>& x,
		  const simple_vector<Tp, Alloc>& y)
  {
    return !(x == y);
  }
  template <typename Tp, typename Alloc>
  inline
  bool operator<(const simple_vector<Tp, Alloc>& x,
		 const simple_vector<Tp, Alloc>& y)
  {
    return std::lexicographical_compare(x.begin(), x.end(), y.begin(), y.end());
  }
  template <typename Tp, typename Alloc>
  inline
  bool operator>(const simple_vector<Tp, Alloc>& x,
		 const simple_vector<Tp, Alloc>& y)
  {
    return y < x;
  }
  template <typename Tp, typename Alloc>
  inline
  bool operator<=(const simple_vector<Tp, Alloc>& x,
		  const simple_vector<Tp, Alloc>& y)
  {
    return ! (y < x);
  }
  template <typename Tp, typename Alloc>
  inline
  bool operator>=(const simple_vector<Tp, Alloc>& x,
		  const simple_vector<Tp, Alloc>& y)
  {
    return ! (x < y);
  }
};

namespace std {
  template<typename _Tp, typename _Alloc>
  inline void
  swap(utils::simple_vector<_Tp,_Alloc>& x,
       utils::simple_vector<_Tp,_Alloc>& y)
  {
    x.swap(y);
  }
};

#endif
