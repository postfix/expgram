// -*- mode: c++ -*-
//
//  Copyright(C) 2009-2013 Taro Watanabe <taro.watanabe@nict.go.jp>
//

#ifndef __EXPGRAM__NGRAM_STATE__HPP__
#define __EXPGRAM__NGRAM_STATE__HPP__ 1

//
// ngram state manager
//

#include <algorithm>

#include <expgram/Word.hpp>

namespace expgram
{
  struct NGramState
  {
    typedef size_t             size_type;
    typedef ptrdiff_t          difference_type;
    
    typedef Word               word_type;
    typedef float              logprob_type;
    
    NGramState(const size_type order) : order_(order) {}
    
    size_type order_;
    
    size_type buffer_size() const
    {
      return sizeof(size_type) + (sizeof(word_type::id_type) + sizeof(logprob_type)) * (order_ - 1);
    }
    
    size_type& length(void* buffer) const
    {
      return *reinterpret_cast<size_type*>(buffer);
    }
    
    const size_type& length(const void* buffer) const
    {
      return *reinterpret_cast<const size_type*>(buffer);
    }
    
    word_type::id_type* context(void* buffer) const
    {
      return reinterpret_cast<word_type::id_type*>((char*) buffer + sizeof(size_type));
    }
    
    const word_type::id_type* context(const void* buffer) const
    {
      return reinterpret_cast<const word_type::id_type*>((const char*) buffer + sizeof(size_type));
    }
    
    logprob_type* backoff(void* buffer) const
    {
      return reinterpret_cast<logprob_type*>((char*) buffer + sizeof(size_type) + sizeof(word_type::id_type) * (order_ - 1));
    }
    
    const logprob_type* backoff(const void* buffer) const
    {
      return reinterpret_cast<const logprob_type*>((const char*) buffer + sizeof(size_type) + sizeof(word_type::id_type) * (order_ - 1));
    }
    
    void fill(void* buffer) const
    {
      const size_type len = length(buffer);
      
      std::fill(reinterpret_cast<char*>(context(buffer) + len), reinterpret_cast<char*>(context(buffer) + order_ - 1), 0);
      std::fill(reinterpret_cast<char*>(backoff(buffer) + len), reinterpret_cast<char*>(backoff(buffer) + order_ - 1), 0);
    }

    void copy(const void* buffer, void* copied)
    {
      std::copy((char*) buffer, ((char*) buffer) + buffer_size(), (char*) copied);
    }
    
  };
};

#endif
