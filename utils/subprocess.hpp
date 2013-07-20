// -*- mode: c++ -*-
//
//  Copyright(C) 2009-2011 Taro Watanabe <taro.watanabe@nict.go.jp>
//

#ifndef __UTILS__SUBPROCESS__HPP__
#define __UTILS__SUBPROCESS__HPP__ 1

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>

#include <cerrno>
#include <cstring>

#include <string>
#include <stdexcept>

#include <boost/filesystem.hpp>

namespace utils
{
  
  class subprocess
  {
    
  public:
    explicit subprocess(const std::string& sh_command,
			const bool open_stdin=true,
			const bool open_stdout=true)
      : __pid(-1), __pread(-1), __pwrite(-1) { open(sh_command, open_stdin, open_stdout); }
    explicit subprocess(const boost::filesystem::path& command,
			const bool open_stdin=true,
			const bool open_stdout=true)
      : __pid(-1), __pread(-1), __pwrite(-1) { open(command, open_stdin, open_stdout); }
    ~subprocess() { close(); }
  private:
    subprocess(const subprocess& x) {}
    subprocess& operator=(const subprocess& x) { return *this; }
    
  public:
    int desc_read() const { return __pread; }
    int desc_write() const { return __pwrite; }
    int& desc_read() { return __pread; }
    int& desc_write() { return __pwrite; }
    

    void terminate()
    {
      if (__pid >= 0)
	::kill(__pid, SIGTERM);
    }

    void open(const std::string& sh_command,
	      const bool open_stdin=true,
	      const bool open_stdout=true)
    {
      int pin[2] = {-1, -1};
      int pout[2] = {-1, -1};
    
      if (open_stdin && ::pipe(pin) < 0)
	throw std::runtime_error(std::string("pipe(): ") + strerror(errno));
      
      if (open_stdout && ::pipe(pout) < 0) {
	if (pin[0] >= 0) {
	  ::close(pin[0]);
	  ::close(pin[1]);
	}
	
	throw std::runtime_error(std::string("pipe(): ") + strerror(errno));
      }
      
      const pid_t pid = ::fork();
      if (pid < 0) {
	if (pin[0] >= 0) {
	  ::close(pin[0]);
	  ::close(pin[1]);
	}
	
	if (pout[0] >= 0) {
	  ::close(pout[0]);
	  ::close(pout[1]);
	}
	
	throw std::runtime_error(std::string("fork(): ") + strerror(errno));
      }
      
      if (pid == 0) {
	// child process...
	// redirect input...
	if (pin[0] >= 0) {
	  ::close(pin[1]);
	  ::dup2(pin[0], STDIN_FILENO);
	  ::close(pin[0]);
	}
	
	// redirect output...
	if (pout[0] >= 0) {
	  ::close(pout[0]);
	  ::dup2(pout[1], STDOUT_FILENO);
	  ::close(pout[1]);
	}
	
	// new group-id
	::setpgid(0, 0);
	
	__pid = ::getpid();
	
	// back to default signal handler
	default_handler(SIGTERM);
	default_handler(SIGINT);
	default_handler(SIGHUP);
	default_handler(SIGPIPE);
	default_handler(SIGCHLD);
	
	// unblock signals
	sigset_t sigs;
	sigprocmask(0, 0, &sigs);
	sigprocmask(SIG_UNBLOCK, &sigs, 0);
	
	::execlp("sh", "sh", "-c", sh_command.c_str(), (char*) 0);
	
	::_exit(errno);  // not exit(errno)!
      } else {
	// parent process...
	if (pin[0] >= 0)
	  ::close(pin[0]);
	if (pout[1] >= 0)
	  ::close(pout[1]);
	
	__pid = pid;
	__pread = pout[0];
	__pwrite = pin[1];
      }
    }

    void open(const boost::filesystem::path& command,
	      const bool open_stdin=true,
	      const bool open_stdout=true)
    {     
      int pin[2] = {-1, -1};
      int pout[2] = {-1, -1};
      
      if (open_stdin && ::pipe(pin) < 0)
	throw std::runtime_error(std::string("pipe(): ") + strerror(errno));
      
      if (open_stdout && ::pipe(pout) < 0) {
	if (pin[0] >= 0) {
	  ::close(pin[0]);
	  ::close(pin[1]);
	}
	
	throw std::runtime_error(std::string("pipe(): ") + strerror(errno));
      }
      
      const pid_t pid = ::fork();
      if (pid < 0) {
	if (pin[0] >= 0) {
	  ::close(pin[0]);	
	  ::close(pin[1]);
	}
	if (pout[0] >= 0) {
	  ::close(pout[0]);
	  ::close(pout[1]);
	}
	
	throw std::runtime_error(std::string("fork(): ") + strerror(errno));
      }
      
      if (pid == 0) {
	// child process...
	// redirect input...
	if (pin[0] >= 0) {
	  ::close(pin[1]);
	  ::dup2(pin[0], STDIN_FILENO);
	  ::close(pin[0]);
	}
	
	// redirect output...
	if (pout[0] >= 0) {
	  ::close(pout[0]);
	  ::dup2(pout[1], STDOUT_FILENO);
	  ::close(pout[1]);
	}
	
	// new group-id
	::setpgid(0, 0);

	__pid = ::getpid();

	// back to default signal handler
	default_handler(SIGTERM);
	default_handler(SIGINT);
	default_handler(SIGHUP);
	default_handler(SIGPIPE);
	default_handler(SIGCHLD);

	// unblock signals
	sigset_t sigs;
	sigprocmask(0, 0, &sigs);
	sigprocmask(SIG_UNBLOCK, &sigs, 0);

#if BOOST_FILESYSTEM_VERSION == 2
	::execlp(command.file_string().c_str(), command.file_string().c_str(), (char*) 0);
#else
	::execlp(command.string().c_str(), command.string().c_str(), (char*) 0);
#endif
	
	::_exit(errno);  // not exit(errno)!
      } else {
	// parent process...
	if (pin[0] >= 0)
	  ::close(pin[0]);
	if (pout[1] >= 0)
	  ::close(pout[1]);
	
	__pid = pid;
	__pread = pout[0];
	__pwrite = pin[1];
      }
    }
    
    void close() {
      if (__pwrite >= 0) {
	::close(__pwrite);
	__pwrite = -1;
      }
      
      if (__pread >= 0) {
	::close(__pread);
	__pread  = -1;
      }
      
      if (__pid >= 0) {
	int status = 0;
	do {
	  const int ret = ::waitpid(__pid, &status, 0);
	  
	  if (ret == __pid)
	    break;
	  else if (ret < 0 && errno == EINTR)
	    continue;
	  else // error...
	    break; 
	} while (true);
	
	__pid = -1;
      }
    }
    
    void __close_on_exec(int fd)
    {
      int flags = ::fcntl(fd, F_GETFD, 0);
      if (flags == -1)
	throw std::runtime_error(std::string("fcntl(): ") + strerror(errno));
      flags |= FD_CLOEXEC;
      if (::fcntl(fd, F_SETFD, flags) == -1)
	throw std::runtime_error(std::string("fcntl(): ") + strerror(errno));
    }
    
    static void default_handler(const int sig)
    {
      struct sigaction act;

      act.sa_handler = SIG_DFL;
      act.sa_flags = 0;
      sigemptyset(&act.sa_mask);
      
      sigaction(sig, &act, (struct sigaction *)0);
    }

  public:
    pid_t __pid;
    int   __pread;
    int   __pwrite;
  };
  
};

#endif
