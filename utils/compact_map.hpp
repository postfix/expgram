// -*- mode: c++ -*-
//
//  Copyright(C) 2012 Taro Watanabe <taro.watanabe@nict.go.jp>
//

#ifndef __UTILS__COMPACT_MAP__HPP__
#define __UTILS__COMPACT_MAP__HPP__

#include <utils/compact_hashtable.hpp>

#include <boost/functional/hash/hash.hpp>

namespace utils
{
  template <typename Key,
	    typename Data,
	    typename Empty,
	    typename Deleted,
	    typename Hash=boost::hash<Key>,
	    typename Pred=std::equal_to<Key>,
	    typename Alloc=std::allocator<std::pair<const Key, Data> > >
  class compact_map 
  {
  public:
    typedef Key                                    key_type;
    typedef Data                                   mapped_type;
    typedef std::pair<const key_type, mapped_type> value_type;
    
  private:
    template <typename __Key>
    struct value_static : public __Key
    {
      const value_type& operator()() const
      {
	static value_type __value(__Key::operator()(), mapped_type());
	return __value;
      }
    };
    
    struct extract_key
    {
      const Key& operator()(const value_type& x) const { return x.first; }
      const Key& operator()(value_type& x) const { return x.first; }
    };
    
    typedef compact_hashtable<key_type, value_type,
			      value_static<Empty>, value_static<Deleted>,
			      extract_key, Hash, Pred, Alloc> impl_type;

  public:
    typedef typename impl_type::size_type       size_type;
    typedef typename impl_type::difference_type difference_type;
    
    typedef typename impl_type::iterator        iterator;
    typedef typename impl_type::const_iterator  const_iterator;
    typedef typename impl_type::pointer         pointer;
    typedef typename impl_type::reference       reference;
    typedef typename impl_type::const_reference const_reference;

  public:
    compact_map(const size_type __size=8,
		const Hash& __hash=Hash(),
		const Pred& __pred=Pred())
      : impl(__size, __hash, __pred) {}

  public:
    void assign(const compact_map& x) { impl.assign(x.impl); }
    void swap(compact_map& x) { impl.swap(x.impl); }

  private:
    struct default_value_type
    {
      value_type operator()(const Key& key) {
	return std::make_pair(key, Data());
      }
    };
    
  public:
    inline mapped_type& operator[](const key_type& x)
    {
      return impl.template insert_default<default_value_type>(x).second;
    }
    
    const_iterator begin() const { return impl.begin(); }
    iterator begin() { return impl.begin(); }
    const_iterator end() const { return impl.end(); }
    iterator end() { return impl.end(); }
    
    bool empty() const { return impl.empty(); }
    size_type size() const { return impl.size(); }
    size_type bucket_count() const { return impl.bucket_count(); }
    size_type occupied_count() const { return impl.occupied_count(); }
    void clear() { impl.clear(); }

    void rehash(size_type hint) { impl.rehash(hint); }
    
    const_iterator find(const key_type& x) const { return impl.find(x); }
    iterator find(const key_type& x) { return impl.find(x); }

    std::pair<iterator, bool> insert(const value_type& x) { return impl.insert(x); }

    iterator insert(iterator, const value_type& x) { return insert(x).first; }
    
    template <typename Iterator>
    void insert(Iterator first, Iterator last) { impl.insert(first, last); }
    
    size_type erase(const key_type& key) { return impl.erase(key); }
    void erase(iterator iter) { impl.erase(iter); }
    void erase(const_iterator iter) { impl.erase(iter); }

    void erase(iterator first, iterator last) { impl.erase(first, last); }
    void erase(const_iterator first, const_iterator last) { impl.erase(first, last); }
  private:
    impl_type impl;
  };
  
};

namespace std
{
  template <typename Key, typename Data, typename Empty, typename Deleted, typename Hash, typename Pred, typename Alloc>
  inline
  void swap(utils::compact_map<Key,Data,Empty,Deleted,Hash,Pred,Alloc>& x,
	    utils::compact_map<Key,Data,Empty,Deleted,Hash,Pred,Alloc>& y)
  {
    x.swap(y);
  }
  
};


#endif
