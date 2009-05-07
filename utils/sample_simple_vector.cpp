
#include <iostream>

#include <vector>
#include <string>

#include "utils/simple_vector.hpp"
#include "utils/allocinfo_allocator.hpp"

int main(int argc, char** argv)
{
  srandom(time(0) * getpid());
  
  std::cout << "allocated: " << utils::allocinfo().allocated() << std::endl;

  utils::simple_vector<std::string, utils::allocinfo_allocator<std::string, std::allocator<std::string> > > str_vec(12);
  utils::simple_vector<std::string, utils::allocinfo_allocator<std::string, std::allocator<std::string> > > str_vec2;

  utils::simple_vector<int> int_vec;
  
  std::cout << "allocated: " << utils::allocinfo().allocated() << std::endl;
  std::cout << "sizeof simple_vector<int>: " << sizeof(utils::simple_vector<int>) << std::endl;
  std::cout << "sizeof simple_vector<string>: " << sizeof(utils::simple_vector<std::string>) << std::endl;
  
  str_vec[0] = "January";
  str_vec[1] = "February";
  str_vec[2] = "March";
  str_vec[3] = "April";
  str_vec[4] = "May";
  str_vec[5] = "June";
  str_vec[6] = "July";
  str_vec[7] = "August";
  str_vec[8] = "September";
  str_vec[9] = "October";
  str_vec[10] = "November";
  str_vec[11] = "December";
  
  for (int i = 0; i < str_vec.size(); ++ i)
    std::cout << "i = " << i << " " << str_vec[i] << std::endl;
  for (utils::simple_vector<std::string, utils::allocinfo_allocator<std::string, std::allocator<std::string> > >::const_iterator
	 iter = str_vec.begin(); iter != str_vec.end(); ++ iter)
    std::cout << "i = " << (iter - str_vec.begin()) << " " << *iter << std::endl;
  
  std::cout << "back: " << str_vec.back() << std::endl;

  str_vec2 = str_vec;
  for (int i = 0; i < str_vec2.size(); ++ i) {
    std::cout << "i = " << i << " " << str_vec2[i] << std::endl;
    str_vec2[i] = "";
  }
  str_vec2 = str_vec;
  for (int i = 0; i < str_vec2.size(); ++ i)
    std::cout << "i = " << i << " " << str_vec2[i] << std::endl;

  std::vector<std::string> vec;
  vec.push_back("1");
  vec.push_back("2");
  vec.push_back("3");
  
  str_vec.assign(vec.begin(), vec.end());
  
  for (int i = 0; i < str_vec.size(); ++ i)
    std::cout << "i = " << i << " " << str_vec[i] << std::endl;
  std::cout << "allocated: " << utils::allocinfo().allocated() << std::endl;
  
  str_vec.resize(500);
  std::cout << "resize: " << str_vec.size() << std::endl;
  std::cout << "allocated: " << utils::allocinfo().allocated() << std::endl;

  str_vec.clear();
  str_vec2.clear();
  std::cout << "allocated (clear): " << utils::allocinfo().allocated() << std::endl;

  std::vector<int> intvec;
  for (int i = 0; i < 1024 * 16; ++ i)
    intvec.push_back(i);

  int_vec.assign(intvec.begin(), intvec.end());
  
  std::vector<int>::const_iterator iter = intvec.begin();
  utils::simple_vector<int>::const_iterator siter = int_vec.begin();
  for (int i = 0; i < 1024 * 16; ++ i, ++ iter, ++ siter)
    if (*iter != *siter)
      std::cout << "differ: " << i << " " << *iter << " " << *siter << std::endl;
  
  std::cout << "size: " << int_vec.size() << std::endl;
  
  
  utils::simple_vector<int> intvec2(12, 666);
  utils::simple_vector<int> intvec3(12);

  intvec3.clear();
  intvec2.swap(intvec3);
  
  std::cerr << "size: " << intvec2.size() << " " << intvec3.size() << std::endl;
}
