#include <cstdio>
#include <cstdlib>
#include <cassert>
#include <cstring>
#include <time.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/time.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <arm_neon.h>

#include <iostream>
#include <fstream>
#include <string>

#include <map>

#include "helper.hh"
#include "driver.hh"
#include "loadelf.hh"
#include "globals.hh"
#include "helper.hh"

#include "syscall.h"

uint32_t globals::tohost_addr;
uint32_t globals::fromhost_addr;

int globals::sysArgc;
char **globals::sysArgv;
bool globals::silent;
bool globals::log;
std::map<std::string, uint32_t> globals::symtab;

static const uint32_t control = 0xA0050000;
static const uint32_t ram = 0x40e00000;

#define CONTROL_REG 0
#define STATUS_REG 1
#define RAM_REG 2

struct status_ {
  uint32_t state : 3;
  uint32_t hist : 8;
  uint32_t bresp : 2;
  uint32_t zero : 15;
  uint32_t read_resp_error : 1;
  uint32_t write_resp_error : 1;
  uint32_t read_mismatch : 1;
  uint32_t rnext : 1;
};

struct rvstatus_ {
  uint32_t ready : 1;
  uint32_t flush : 1;
  uint32_t break_: 1;
  uint32_t ud : 1;
  uint32_t bad_addr : 1;
  uint32_t monitor : 1;
  uint32_t state : 5;
  uint32_t l1d_flushed : 1;
  uint32_t l1i_flushed : 1;
  uint32_t l2_flushed : 1;
  uint32_t reset_out : 1;
  uint32_t mem_req : 1;
  uint32_t mem_req_opcode : 4;
  uint32_t l1d_state : 4;
  uint32_t mem_rsp : 1;
  uint32_t l1i_state : 3;
  uint32_t l2_state : 4;
};

static_assert(sizeof(rvstatus_) == 4, "rvstatus bad size");

union status {
  uint32_t u;
  status_ s;
  status(uint32_t u) :u(u) {}
};

union rvstatus {
  uint32_t u;
  rvstatus_ s;
  rvstatus(uint32_t u) : u(u) {}
};

inline bool cpu_stopped(const rvstatus &rs) {
  return rs.s.break_ or rs.s.ud or rs.s.bad_addr or rs.s.monitor;
}

std::ostream &operator<<(std::ostream &out, const rvstatus &rs) {
  out << "ready           : " << rs.s.ready << "\n";
  out << "flush           : " << rs.s.flush << "\n";
  out << "break           : " << rs.s.break_ << "\n";
  out << "undef inst      : " << rs.s.ud << "\n";
  out << "bad address     : " << rs.s.bad_addr << "\n";
  out << "monitor         : " << rs.s.monitor << "\n";
  out << "state           : " << rs.s.state << "\n";
  out << "l1d_flushed     : " << rs.s.l1d_flushed << "\n";
  out << "l1i_flushed     : " << rs.s.l1i_flushed << "\n";
  out << "l2_flushed      : " << rs.s.l2_flushed << "\n";
  out << "reset           : " << rs.s.reset_out << "\n";
  out << "mem_req         : " << rs.s.mem_req << "\n";
  out << "mem_req_opcode  : " << rs.s.mem_req_opcode << "\n";
  out << "l1d state       : " << rs.s.l1d_state << "\n";
  out << "l1i state       : " << rs.s.l1i_state << "\n";
  out << "l2  state       : " << rs.s.l2_state << "\n";  
  out << "mem_rsp         : " << rs.s.mem_rsp << "\n";
  
  return out;
}


static_assert(sizeof(status) == 4, "wrong size for status");

static inline uint8_t *mmap4G() {
  void* mempt = mmap(nullptr, 1UL<<32, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  assert(mempt != reinterpret_cast<void*>(-1));
  assert(madvise(mempt, 1UL<<32, MADV_DONTNEED)==0);

  return reinterpret_cast<uint8_t*>(mempt);
}


int main(int argc, char *argv[]) {
  
  Driver d(control);
  d.write32(CONTROL_REG, 0);
  d.write32(4, 1);
  d.write32(4,0);  
  status s(0);

  d.write32(6,ram);

  Driver dd(ram);

  uint8_t *mem_ = mmap4G();
  char *bin_name = "dhrystone.rv32";
  if(argc == 2) {
    bin_name = argv[1];
  }
  uint32_t pc = load_elf(bin_name, mem_);
  //make addresses 0 based
  d.write32(6,0);
  uint32_t *mem_w32 = reinterpret_cast<uint32_t*>(mem_);
  
  rvstatus rs(d.read32(0xa));
  std::cout << rs;

  //resume pc
  d.write32(5, pc);
  
  d.write32(4,2);
  d.write32(4,0);
  
  std::cout << "fired go!\n";
  
  // globals::tohost_addr;
  //globals::fromhost_addr;

  bool run = true;
  uint64_t mem_ops = 0;
  while(run) {
    rs.u = d.read32(0xa);
    //std::cout << rs << "\n";
    
    if(rs.s.mem_req) {
      mem_ops++;
      if((mem_ops & ((1UL<<26)-1)) == 0 ) {
	std::cout << "mem_ops = " << mem_ops << "\n";
      }
      uint32_t addr = d.read32(15);
      //if(addr > (1<<24)) {
      //std::cout << std::hex << "request for addr : "
      //<< addr << std::dec << "\n";
      //exit(-1);
      //}

      assert(rs.s.mem_req_opcode == 4 ||
	     rs.s.mem_req_opcode == 7);

      if(rs.s.mem_req_opcode == 4) {
	for(int i = 0; i < 4; i++) {
	  d.write32(36+i, mem_w32[(addr/4) + i]);
	}
      }
      else {
	for(int i = 0; i < 4; i++) {
	  mem_w32[(addr/4) + i] = d.read32(32+i);
	}
      }
      d.write32(CONTROL_REG, 1U<<31);
      d.write32(CONTROL_REG, 0);
      
    }

    if(rs.s.monitor) {
      //std::cout << "tohost_addr = " << std::hex << globals::tohost_addr << std::dec << "\n";

      //std::cout << "monitor request\n";
      uint32_t to_host = *reinterpret_cast<uint32_t*>(mem_ + globals::tohost_addr);
      //std::cout << "to_host reason = " << std::hex << to_host << std::dec << "\n";
      
      if(to_host) {
	if(to_host & 1) {
	  run = false;
	  break;
	}
	else {
	  uint64_t *buf = reinterpret_cast<uint64_t*>(mem_ + to_host);
	  switch(buf[0])
	    {
	    case SYS_write: /* int write(int file, char *ptr, int len) */
	      buf[0] = write(buf[1], (void*)(mem_ + buf[2]), buf[3]);
	      if(buf[1]==1)
		fflush(stdout);
	      else if(buf[1]==2)
		fflush(stderr);
	      break;
	    case SYS_open: {
	      const char *path = reinterpret_cast<const char*>(mem_ + buf[1]);
	      buf[0] = open(path, remapIOFlags(buf[2]), S_IRUSR|S_IWUSR);
	      break;
	    }
	    case SYS_close: {
	      if(buf[1] > 2) {
		buf[0] = close(buf[1]);
	      }
	      else {
		buf[0] = 0;
	      }
	      break;
	    }
	    case SYS_read: {
	      buf[0] = read(buf[1], reinterpret_cast<char*>(mem_ + buf[2]), buf[3]); 
	      break;
	    }
	    case SYS_lseek: {
	      buf[0] = lseek(buf[1], buf[2], buf[3]);
	      break;
	    }
	    case SYS_fstat : {
	      struct stat native_stat;
	      stat32_t *host_stat = reinterpret_cast<stat32_t*>(mem_ + buf[2]);
	      int rc = fstat(buf[1], &native_stat);
	      host_stat->st_dev = native_stat.st_dev;
	      host_stat->st_ino = native_stat.st_ino;
	      host_stat->st_mode = native_stat.st_mode;
	      host_stat->st_nlink = native_stat.st_nlink;
	      host_stat->st_uid = native_stat.st_uid;
	      host_stat->st_gid = native_stat.st_gid;
	      host_stat->st_size = native_stat.st_size;
	      host_stat->_st_atime = native_stat.st_atime;
	      host_stat->_st_mtime = 0;
	      host_stat->_st_ctime = 0;
	      host_stat->st_blksize = native_stat.st_blksize;
	      host_stat->st_blocks = native_stat.st_blocks;
	      buf[0] = rc;
	      break;
	    }
	    case SYS_stat : {
	      buf[0] = 0;
	      break;
	    }
	    case SYS_gettimeofday: {
	      static_assert(sizeof(struct timeval)==16, "timeval has a weird size");
	      struct timeval *tp = reinterpret_cast<struct timeval*>(mem_ + buf[1]);
	      struct timezone *tzp = reinterpret_cast<struct timezone*>(mem_ + buf[2]);
	      buf[0] = gettimeofday(tp, tzp);
	      break;
	    }
	    default:
	      std::cout << "syscall " << buf[0] << " unsupported\n";
	      exit(-1);
	    }
	  //ack
	  *reinterpret_cast<uint64_t*>(mem_ + globals::tohost_addr) = 0;
	  *reinterpret_cast<uint64_t*>(mem_ + globals::fromhost_addr) = 1;
	  
	  d.write32(4,4);
	  d.write32(4,0);
	  
	  //ack monitor
	}
      }
    }
  }
  uint32_t insns = d.read32(0);
  uint32_t cycle = d.read32(12);
  std::cout << "insns = " << insns << "\n";
  std::cout << "cycle = " << cycle << "\n";
  double ipc = static_cast<double>(insns) / cycle;
  std::cout << "ipc   = " << ipc << "\n";
  munmap(mem_, 1UL<<32);
  return 0;
  
  while(true) {
    sleep(1);
    s.u = d.read32(STATUS_REG);
    std::cout << "state : " << s.s.state << "\n";
    std::cout << "hist : " << std::hex << s.s.hist << std::dec << "\n";
    std::cout << "bresp : " << s.s.bresp << "\n";
    std::cout << "read_resp_error : " << s.s.read_resp_error << "\n";
    std::cout << "write_resp_error : " << s.s.write_resp_error << "\n";
    std::cout << "read_mismatch : " << s.s.read_mismatch << "\n";
    std::cout << "rnext         : " << s.s.rnext << "\n";
    std::cout << "zero        : " << s.s.zero << "\n";
    
    rs.u = d.read32(0xa);
    std::cout << rs << "\n";
    
    std::cout << std::hex << "memory txns : " << d.read32(14) << std::dec << "\n";
    std::cout << std::hex << "last addr : " << d.read32(15) << std::dec << "\n";

    if(rs.s.mem_req) {
      std::cout << "see mem req, force rsp_valid\n";
      d.write32(CONTROL_REG, 1U<<31);
      d.write32(CONTROL_REG, 0);
    }
    
    for(int i = 0; i < 4; i++) {
      std::cout << std::hex << d.read32(16+i) << std::dec << "\n";
    }
    
    if(cpu_stopped(rs)) {
      std::cout << "EPC : " << std::hex << d.read32(7) << std::dec << "\n";
      std::cout << "insn retired " << d.read32(0) << "\n";
      std::cout << std::hex << "memory txns : " << d.read32(14) << std::dec << "\n";
      std::cout << std::hex << "last addr : " << d.read32(15) << std::dec << "\n";
      break;
    }
    sleep(1);
  }
  
  for(int i = 0; i < 4; i++) {
    std::cout << std::hex << d.read32(16+i) << std::dec << "\n";
  }
  


  
#if 0
  Driver dd(ram);
  std::cout << "dram values interface:\n";  
  for(int i = 0; i < 1024; i++) {
    std::cout << dd.read32(i) << "\n";
  }
#endif

  //std::cout << "last read interface:\n";
  //d.write32(CONTROL_REG, 1);
  //d.write32(CONTROL_REG, 0);
  
  //for(int i = 0;i < 4; i++) {
  //std::cout << std::hex << d.read32(4+i) << std::dec << "\n";
  //}
  
  //for(int i = 0; i < 4; i++) {
  //std::cout << std::hex << dd.read32(i) << std::dec << "\n";
  //}


  
  return 0;
}
