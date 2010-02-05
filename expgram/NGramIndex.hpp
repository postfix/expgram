// -*- mode: c++ -*-

#ifndef __EXPGRAM__NGRAM_INDEX__HPP__
#define __EXPGRAM__NGRAM_INDEX__HPP__ 1

// NGramIndex structure shared by NGram and NGramCounts
// Actually, the difference is the data associated with index:
// NGramCounts has conts and counts-modified
// NGram has logprob, backoff, lobbound


#include <stdint.h>

#include <stdexcept>
#include <vector>

#include <boost/filesystem.hpp>

#include <expgram/Word.hpp>
#include <expgram/Vocab.hpp>
#include <expgram/Stat.hpp>

#include <utils/packed_vector.hpp>
#include <utils/succinct_vector.hpp>
#include <utils/hashmurmur.hpp>
#include <utils/array_power2.hpp>
#include <utils/spinlock.hpp>


namespace expgram
{
  
  class NGramIndex
  {
  public:
    typedef Word                    word_type;
    typedef Vocab                   vocab_type;
    
    typedef size_t                  size_type;
    typedef ptrdiff_t               difference_type;
    typedef word_type::id_type      id_type;
    
    typedef boost::filesystem::path path_type;
    
    typedef uint64_t                           hash_value_type;
    typedef utils::hashmurmur<hash_value_type> hasher_type;

    typedef Stat stat_type;
    
  public:
    struct Shard : public hasher_type
    {
    public:
      typedef utils::packed_vector_mapped<id_type, std::allocator<id_type> >   id_set_type;
      typedef utils::succinct_vector_mapped<std::allocator<int32_t> >          position_set_type;
      typedef std::vector<size_type, std::allocator<size_type> >               off_set_type;
      
    private:
      typedef utils::spinlock spinlock_type;
      typedef spinlock_type::scoped_lock     lock_type;
      typedef spinlock_type::scoped_try_lock trylock_type;
      
      struct cache_pos_type
      {
	size_type pos;
	size_type pos_next;
	id_type id;
        
	cache_pos_type() : pos(size_type(-1)), pos_next(size_type(-1)), id(id_type(-1)) {}
      };
      typedef utils::array_power2<cache_pos_type, 1024 * 64, std::allocator<cache_pos_type> > cache_pos_set_type;
      
    public:
      Shard() {}
      Shard(const path_type& path) { open(path); }

      Shard(const Shard& x) : ids(x.ids), positions(x.positions), offsets(x.offsets) {}
      Shard& operator=(const Shard& x)
      {
	ids = x.ids;
	positions = x.positions;
	offsets = x.offsets;
	caches.clear();
	return *this;
      }

      
    public:
      void close() { clear(); }
      void clear()
      {
	ids.clear();
	positions.clear();
	offsets.clear();
	caches.clear();
      };
      
      void open(const path_type& path);
      void write(const path_type& file) const;
      void write(const path_type& file, const int write_order) const;
      
    public:
      id_type operator[](size_type pos) const { return index(pos); }
      id_type index(size_type pos) const { return (pos < offsets[1] ? id_type(pos) : ids[pos - offsets[1]]); }
      size_type position_size() const { return offsets[offsets.size() - 2]; }
      size_type size() const { return offsets.back(); }
      bool empty() const { return offsets.empty(); }
      path_type path() const { return ids.path().parent_path(); }

      stat_type stat_index() const
      {
	return stat_type(ids.size_bytes(), ids.size_compressed(), ids.size_cache());
      }
      
      stat_type stat_pointer() const
      {
	return stat_type(position_size() * sizeof(uint32_t), positions.size_compressed(), positions.size_cache());
      }
      
      size_type parent(size_type pos) const
      {
	return (pos < offsets[1] ? size_type(-1) : positions.select(pos + 1 - offsets[1], true) + (offsets[1] + 1) - pos - 1);
      }
      size_type children_first(size_type pos) const
      {
	if (pos == size_type(-1) || pos == 0)
	  return (~pos) & offsets[1];
	else
	  return children_last(pos - 1);
      }
      size_type children_last(size_type pos) const
      {
	if (pos == size_type(-1) || pos >= position_size()) {
	  const size_type is_root_mask = size_type(pos == size_type(-1)) - 1;
	  return ((~is_root_mask) & offsets[1]) | (is_root_mask & size());
	}
	
	position_set_type::size_type last = positions.select(pos + 2 - 1, false);

	const size_type last_mask = size_type(last == position_set_type::size_type(-1)) - 1;
	
	return ((~last_mask) & size()) | (last_mask & ((last + 1 + offsets[1] + 1) - (pos + 2)));
	
	//return (last == position_set_type::size_type(-1) ? size() : (last + 1 + offsets[1] + 1) - (pos + 2));
      }
      
      template <typename Iterator>
      std::pair<Iterator, size_type> traverse(Iterator first, Iterator last, const vocab_type& vocab) const
      {
	typedef typename std::iterator_traits<Iterator>::value_type value_type;
	return __traverse_dispatch(first, last, vocab, value_type());
      }
      
      size_type __find(size_type pos, const id_type& id) const
      {
	// we do caching, here...?
	const size_type pos_first = children_first(pos);
	const size_type pos_last  = children_last(pos);
	
	const size_type child = lower_bound(pos_first, pos_last, id);
	const size_type found_child_mask = size_type(child != pos_last && ! (id < operator[](child))) - 1;
	
	return ((~found_child_mask) & child) | found_child_mask;
      }

      size_type find(size_type pos, const id_type& id) const
      {
	if (pos == size_type(-1)) {
          const size_type child_mask = size_type(id < offsets[1]) - 1;
          return (~child_mask & size_type(id)) | child_mask;
        } else {
	  // lock here!
	  trylock_type lock(const_cast<spinlock_type&>(spinlock));
	  
	  if (lock) {
	    const size_type cache_pos = hasher_type::operator()(id, pos) & (caches.size() - 1);
	    cache_pos_type& cache = const_cast<cache_pos_type&>(caches[cache_pos]);
	    if (cache.pos != pos || cache.id != id) {
	      cache.pos      = pos;
	      cache.id       = id;
	      cache.pos_next = __find(pos, id);
	    }
	    return cache.pos_next;
	  } else
	    return __find(pos, id);
        }
      }
    
      size_type lower_bound(size_type first, size_type last, const id_type& id) const
      {
	if (last <= offsets[1])
	  return std::min(size_type(id), last); // unigram!
	else {
	  // otherwise...
	  size_type length = last - first;
	  const size_type offset = offsets[1];
	  
	  if (length <= 128) {
	    for (/**/; first != last && ids[first - offset] < id; ++ first);
	    return first;
	  } else {
	    while (length > 0) {
	      const size_t half  = length >> 1;
	      const size_t middle = first + half;
	      
	      if (ids[middle - offset] < id) {
		first = middle + 1;
		length = length - half - 1;
	      } else
		length = half;
	    }
	    return first;
	  }
	}
      }
      
      template <typename Iterator, typename _Word>
      std::pair<Iterator, size_type> __traverse_dispatch(Iterator first, Iterator last, const vocab_type& vocab, _Word) const
      {
	size_type pos = size_type(-1);
	for (/**/; first != last; ++ first) {
	  const size_type node = find(pos, vocab[word_type(*first)]);
	  
	  if (node == size_type(-1))
	    return std::make_pair(first, pos);
	  pos = node;
	}
	return std::make_pair(first, pos);
      }
      
      template <typename Iterator>
      std::pair<Iterator, size_type> __traverse_dispatch(Iterator first, Iterator last, const vocab_type& vocab, id_type) const
      {
	size_type pos = size_type(-1);
	for (/**/; first != last; ++ first) {
	  const size_type node = find(pos, *first);
	  
	  if (node == size_type(-1))
	    return std::make_pair(first, pos);
	  pos = node;
	}
	return std::make_pair(first, pos);
      }

    public:
      id_set_type        ids;
      position_set_type  positions;
      off_set_type       offsets;

      spinlock_type      spinlock;
      cache_pos_set_type caches;
    };

    
    typedef Shard shard_type;
    typedef std::vector<shard_type, std::allocator<shard_type> > shard_set_type;
    
    typedef shard_set_type::const_iterator  const_iterator;
    typedef shard_set_type::iterator              iterator;
    
    typedef shard_set_type::const_reference const_reference;
    typedef shard_set_type::reference             reference;
    
  public:
    NGramIndex() {}
    NGramIndex(const path_type& path) { open(path); }
    
  public:
    
    template <typename Iterator>
    size_type shard_index(Iterator first, Iterator last) const
    {
      if (last == first || last - first == 1) return 0;
      
      typedef typename std::iterator_traits<Iterator>::value_type value_type;
      return __shard_index_dispatch(first, last, value_type());
    }
    
    template <typename Iterator>
    std::pair<Iterator, size_type> traverse(size_type shard, Iterator first, Iterator last) const
    {
      return __shards[shard].traverse(first, last, __vocab);
    }
    
    template <typename Iterator>
    std::pair<Iterator, size_type> traverse(Iterator first, Iterator last) const
    {
      return __shards[shard_index(first, last)].traverse(first, last, __vocab);
    }
    
    inline const_reference operator[](size_type pos) const { return __shards[pos]; }
    inline       reference operator[](size_type pos)       { return __shards[pos]; }
    
    inline const_iterator begin() const { return __shards.begin(); }
    inline       iterator begin()       { return __shards.begin(); }
    
    inline const_iterator end() const { return __shards.end(); }
    inline       iterator end()       { return __shards.end(); }

    inline const vocab_type& vocab() const { return __vocab; }
    inline       vocab_type& vocab()       { return __vocab; }
    
    size_type size() const { return __shards.size(); }
    bool empty() const { return __shards.empty(); }
    
    void reserve(size_type n) { __shards.reserve(n); }
    void resize(size_type n) { __shards.resize(n); }
    void clear()
    {
      __shards.clear();
      __vocab.clear();
      __order = 0;
      __path = path_type();
    }
    void close() { clear(); }
    
    void open(const path_type& path);
    void write(const path_type& file) const;

    void open_shard(const path_type& path, int shard);
    void write_prepare(const path_type& path) const;
    void write_shard(const path_type& path, int shard) const;
    
    bool is_open() const { return ! __shards.empty() && ! __path.empty(); }
    path_type path() const { return __path; }
    
    inline const int& order() const { return __order; }
    inline       int& order()       { return __order; }
    
    size_type ngram_size(int order) const
    {
      switch (order) {
      case 0: return 0;
      case 1: return __shards.front().offsets[1];
      default:
	size_type sum = 0;
	for (int shard = 0; shard < __shards.size(); ++ shard)
	  sum += __shards[shard].offsets[order] - __shards[shard].offsets[order - 1];
	return sum;
      }
    }
    
    stat_type stat_index() const
    {
      stat_type stat;
      for (int shard = 0; shard < __shards.size(); ++ shard)
	stat += __shards[shard].stat_index();
      return stat;
    }
    
    stat_type stat_pointer() const
    {
      stat_type stat;
      for (int shard = 0; shard < __shards.size(); ++ shard)
	stat += __shards[shard].stat_pointer();
      return stat;
    }
    
    stat_type stat_vocab() const
    {
      return stat_type(__vocab.size_bytes(), __vocab.size_compressed(), __vocab.size_cache());
    }
  
  private:
    
    template <typename Iterator, typename _Word>
    size_type __shard_index_dispatch(Iterator first, Iterator last, _Word) const
    {
      return __hasher(__vocab[word_type(*first)], __hasher(__vocab[word_type(*(first + 1))], 0)) % __shards.size();
    }
    
    template <typename Iterator>
    size_type __shard_index_dispatch(Iterator first, Iterator last, id_type) const
    {
      return __hasher(*first, __hasher(*(first + 1), 0)) % __shards.size();
    }
    
  private:
    shard_set_type __shards;
    vocab_type     __vocab;
    hasher_type    __hasher;
    
    int            __order;
    path_type      __path;
  };
  
};

#endif
