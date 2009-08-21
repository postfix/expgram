// -*- encoding: utf-8 -*-

#include <cstdio>

#include <iostream>
#include <string>
#include <vector>
#include <stdexcept>
#include <iterator>
#include <algorithm>

#include <boost/filesystem.hpp>
#include <boost/program_options.hpp>
#include <boost/thread.hpp>
#include <boost/shared_ptr.hpp>
#include <boost/algorithm/string.hpp>

#include <utils/mpi.hpp>
#include <utils/mpi_stream.hpp>
#include <utils/compress_stream.hpp>
#include <utils/lockfree_list_queue.hpp>

typedef boost::filesystem::path path_type;
typedef std::vector<path_type> path_set_type;

path_set_type input_files;
int debug = 0;

int getoptions(int argc, char** argv);

void run_command(const std::string& command)
{
  static const size_t buffer_size = 1024;
  
  char buffer[buffer_size];
  
  FILE* fp = ::popen(command.c_str(), "r");
  
  for (;;) {
    const size_t size = ::fread(buffer, 1, buffer_size, fp);
    if (size == 0) {
      if (::feof(fp))
	break;
      else if (::ferror(fp))
	throw std::runtime_error("error?");
    } else
      std::cout.write(buffer, size);
  }
  ::pclose(fp);
}

struct Task
{
  typedef boost::thread thread_type;
  typedef utils::lockfree_list_queue<std::string, std::allocator<std::string> > queue_type;
  
  queue_type& queue;
  
  Task(queue_type& __queue)
    : queue(__queue) {}

  void operator()()
  {
    std::string command;
    
    while (1) {
      queue.pop_swap(command);
      if (command.empty()) break;
      
      run_command(command);
    }
  }
};

enum {
  command_tag = 1000,
};

int main(int argc, char** argv)
{
  utils::mpi_world mpi_world(argc, argv);
  
  const int mpi_rank = MPI::COMM_WORLD.Get_rank();
  const int mpi_size = MPI::COMM_WORLD.Get_size();
  
  try {
    if (getoptions(argc, argv) != 0) 
      return 1;
    
    if (mpi_rank == 0) {
      typedef utils::mpi_ostream ostream_type;
      typedef boost::shared_ptr<ostream_type> ostream_ptr_type;
      
      typedef Task task_type;
      
      typedef task_type::thread_type thread_type;
      typedef task_type::queue_type  queue_type;
      
      std::vector<ostream_ptr_type> stream(mpi_size);
      for (int rank = 1; rank < mpi_size; ++ rank)
	stream[rank].reset(new ostream_type(rank, command_tag, 4096));

      for (int rank = 1; rank < mpi_size; ++ rank) {
	while (! stream[rank]->test())
	  boost::thread::yield();
      }
      
      queue_type queue(1);
      std::auto_ptr<thread_type> thread(new thread_type(task_type(queue)));

      if (input_files.empty())
	input_files.push_back("-");

      std::string command_root;
      
      for (path_set_type::const_iterator piter = input_files.begin(); piter != input_files.end(); ++ piter) {
	if (debug)
	  std::cerr << "file: " << piter->file_string() << std::endl;
	
	utils::compress_istream is(*piter);
	
	std::string command;

	int non_found_iter = 0;
	
	while (is) {
	  bool found = false;
	  
	  for (int rank = 1; rank < mpi_size && is; ++ rank)
	    if (stream[rank]->test() && std::getline(is, command)) {
	      boost::algorithm::trim(command);
	      
	      if (command.empty()) continue;
	      
	      stream[rank]->write(command);
	      
	      found = true;
	    }
	  
	  if (command_root.empty() && std::getline(is, command)) {
	    boost::algorithm::trim(command);
	    
	    if (! command.empty()) 
	      command_root = command;
	  }

	  if (! command_root.empty() && queue.push(command_root, true)) {
	    if (debug)
	      std::cerr << "rank: " << mpi_rank << " " << command_root << std::endl;

	    command_root.clear();
	    found = true;
	  }
	  
	  if (! found) {
	    boost::thread::yield();
	    ++ non_found_iter;
	  } else
	    non_found_iter = 0;
	  
	  if (non_found_iter >= 50) {
	
	    struct timespec tm;
	    tm.tv_sec = 0;
	    tm.tv_nsec = 2000001;
	    nanosleep(&tm, NULL);
	    
	    non_found_iter = 0;
	  }
	}
      }

      
      int non_found_iter = 0;
      while (! command_root.empty()) {
	bool found = false;

	if (! command_root.empty() && queue.push_swap(command_root, true)) {
	  command_root.clear();
	  found = true;
	}
	
	// perform termination
	for (int rank = 1; rank < mpi_size; ++ rank) 
	  if (stream[rank]) {
	    if (! stream[rank]->terminated())
	      stream[rank]->terminate();
	    else
	      stream[rank].reset();
	    found = true;
	  }
	
	if (! found) {
	  boost::thread::yield();
	  ++ non_found_iter;
	} else
	  non_found_iter = 0;
	
	if (non_found_iter >= 50) {
	  struct timespec tm;
	  tm.tv_sec = 0;
	  tm.tv_nsec = 2000001;
	  nanosleep(&tm, NULL);
	  
	  non_found_iter = 0;
	}
      }
      
      
      bool terminated = false;
      while (1) {
	bool found = false;
	
	if (! terminated && queue.push_swap(command_root, true)) {
	  command_root.clear();
	  terminated = true;
	  found = true;
	}
	
	for (int rank = 1; rank < mpi_size; ++ rank) 
	  if (stream[rank]) {
	    if (! stream[rank]->terminated())
	      stream[rank]->terminate();
	    else
	      stream[rank].reset();
	    found = true;
	  }
	
	if (terminated && std::count(stream.begin(), stream.end(), ostream_ptr_type()) == mpi_size) break;
	
	if (! found) {
	  boost::thread::yield();
	  ++ non_found_iter;
	} else
	  non_found_iter = 0;
	
	if (non_found_iter >= 50) {
	  struct timespec tm;
	  tm.tv_sec = 0;
	  tm.tv_nsec = 2000001;
	  nanosleep(&tm, NULL);
	  
	  non_found_iter = 0;
	}
      }

      thread->join();
      
    } else {
      typedef Task task_type;
      
      typedef task_type::thread_type thread_type;
      typedef task_type::queue_type  queue_type;
      
      queue_type queue(1);
      std::auto_ptr<thread_type> thread(new thread_type(task_type(queue)));
      
      utils::mpi_istream is(0, command_tag, 4096, true);
      std::string command;
      while (is.read(command)) {
	if (debug)
	  std::cerr << "rank: " << mpi_rank << " " << command << std::endl;
	
	queue.push(command);
	is.ready();
      }
      
      queue.push(std::string());

      thread->join();
    }
  }
  catch (const std::exception& err) {
    std::cerr << "error: " << err.what() << std::endl;
    return -1;
  }
  return 0;
}

int getoptions(int argc, char** argv)
{
  const int mpi_rank = MPI::COMM_WORLD.Get_rank();
  const int mpi_size = MPI::COMM_WORLD.Get_size();

  namespace po = boost::program_options;
  
  po::options_description desc("options");
  desc.add_options()
    ("debug", po::value<int>(&debug)->implicit_value(1), "debug level")
    ("help", "help message");

  po::options_description hidden;
  hidden.add_options()
    ("input-file", po::value<path_set_type>(&input_files), "input file");

  po::options_description cmdline_options;
  cmdline_options.add(desc).add(hidden);
  
  po::positional_options_description pos;
  pos.add("input-file", -1); // all the files
  
  po::variables_map vm;
  po::store(po::command_line_parser(argc, argv).options(cmdline_options).positional(pos).run(), vm);
  po::notify(vm);
  
  if (vm.count("help")) {
    
    if (mpi_rank == 0)
      std::cout << argv[0] << " [options] [file(s) listing shell commands]" << '\n' << desc << '\n';
    return 1;
  }
  
  return 0;
}
