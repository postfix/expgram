// -*- mode: c++ -*-
//
//  Copyright(C) 2010-2011 Taro Watanabe <taro.watanabe@nict.go.jp>
//

#ifndef __UTILS__BASE64__HPP__
#define __UTILS__BASE64__HPP__ 1

#include <string>
#include <algorithm>
#include <iterator>

#include <utils/bithack.hpp>

#include <boost/archive/iterators/binary_from_base64.hpp>
#include <boost/archive/iterators/base64_from_binary.hpp>
#include <boost/archive/iterators/transform_width.hpp>

namespace utils
{
  template <typename Tp, typename Iterator>
  inline
  Iterator encode_base64(const Tp& x, Iterator iter)
  {
    static const char* enc64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    
    const unsigned char* buf = reinterpret_cast<const unsigned char*>(&x);
    const size_t size = sizeof(Tp);
    
    size_t curr = 0;
    while (curr < size) {
      const size_t len = utils::bithack::min(size - curr, size_t(3));
      
      *iter = enc64[buf[curr + 0] >> 2]; ++ iter;
      
      switch (len) {
      case 3:
	*iter = enc64[((buf[curr + 0] & 0x03) << 4) | ((buf[curr + 1] & 0xf0) >> 4)]; ++ iter;
	*iter = enc64[((buf[curr + 1] & 0x0f) << 2) | ((buf[curr + 2] & 0xc0) >> 6)]; ++ iter;
	*iter = enc64[buf[curr + 2] & 0x3f];                                          ++ iter;
	break;
      case 2:
	*iter = enc64[((buf[curr + 0] & 0x03) << 4) | ((buf[curr + 1] & 0xf0) >> 4)]; ++ iter;
	*iter = enc64[(buf[curr + 1] & 0x0f) << 2];                                   ++ iter;
	break;
      case 1:
	*iter = enc64[(buf[curr + 0] & 0x03) << 4]; ++ iter;
	break;
      }
      
      curr += len;
    }

    return iter;
    
#if 0
    using namespace boost::archive::iterators;
    
    typedef base64_from_binary<transform_width<const char*, 6, 8> > encoder_type;
    
    std::copy(encoder_type((const char*) &x), encoder_type(((const char*) &x) + sizeof(Tp)), iter);
    return iter;
#endif
  }

  template <typename Tp>
  inline
  std::string encode_base64(const Tp& x)
  {
    std::string encoded;
    encode_base64(x, std::back_inserter(encoded));
    return encoded;
  }


  template <typename Tp>
  inline
  Tp decode_base64(const std::string& x)
  {
    using namespace boost::archive::iterators;
  
    typedef transform_width<binary_from_base64<std::string::const_iterator>, 8, 6> decoder_type;
  
    Tp value;
  
    char* iter = (char*) &value;
    char* iter_end = iter + sizeof(Tp);

    decoder_type decoder(x.begin());
    for (/**/; iter != iter_end; ++ iter, ++ decoder)
      *iter = *decoder;
  
    return value;
  }
};

#endif
