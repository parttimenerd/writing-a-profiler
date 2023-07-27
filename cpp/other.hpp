#pragma once

#include <algorithm>
#include <atomic>
#include <assert.h>
#include <chrono>
#include <iostream>
#include <dlfcn.h>
#include <mutex>
#include <optional>
#include <signal.h>
#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <tuple>
#include <thread>
#include <random>
#include <unordered_map>
#include <vector>
#include <sys/types.h>
#include <sys/time.h>
#include <profile2.h>

#include <pthread.h>

#if defined(__linux__)
#include <sys/syscall.h>
#include <unistd.h>
#endif

#include "jvmti.h"

bool wall_clock_mode = true;

jvmtiEnv* jvmti;
JNIEnv* env;
JavaVM* jvm;

void ensureSuccess(jvmtiError err, const char *msg) {
  if (err != JVMTI_ERROR_NONE) {
    fprintf(stderr, "Error in %s: %d", msg, err);
    exit(1);
  }
}

pid_t get_thread_id() {
  #if defined(__APPLE__) && defined(__MACH__)
  uint64_t tid;
  pthread_threadid_np(NULL, &tid);
  return (pid_t) tid;
  #else
  return syscall(SYS_gettid);
  #endif
}



size_t parseTimeToNanos(char *str) {
  char *suffix = str + strlen(str) - 2;
  double num = atof(str);
  if (strcmp(suffix, "ms") == 0) {
    return num * 1000000;
  } else if (strcmp(suffix, "us") == 0) {
    return num * 1000;
  } else if (strcmp(suffix, "ns") == 0) {
    return num;
  } else {
    if (str[strlen(str) - 1] == 's') {
      return num * 1000000000;
    } else {
      fprintf(stderr, "Invalid time suffix: %s", suffix);
      exit(1);
    }
  }
}

/** parse all options, like interval, and store them into global variables */
void parseOptions(char *options) {
  char *token = strtok(options, ",");
  while (token != NULL) {
    if (strncmp(token, "interval=", 9) == 0) {
      interval_ns = parseTimeToNanos(token + 9);
    }
    if (strncmp(token, "cpu", 3) == 0) {
      wall_clock_mode = false;
    }
    token = strtok(NULL, ",");
  }
}

bool is_thread_running(jthread thread) {
  jint state;
  auto err = jvmti->GetThreadState(thread, &state);
  return err == 0 && state != JVMTI_THREAD_STATE_RUNNABLE;
}


struct ValidThreadInfo {
    jthread jthreadId;
    pthread_t pthread;
    bool is_running;
    long id;
};
class ThreadMap {
  std::recursive_mutex m;
  std::unordered_map<pid_t, ValidThreadInfo> map;
  std::vector<std::string> names;

  public:

    void add(pid_t pid, std::string name, jthread thread) {
      const std::lock_guard<std::recursive_mutex> lock(m);
      map[pid] = ValidThreadInfo{.jthreadId = thread, .pthread = pthread_self(), .id = (long)names.size()};
      names.emplace_back(name);
    }

    void remove(pid_t pid) {
      const std::lock_guard<std::recursive_mutex> lock(m);
      map.erase(pid);
    }

    std::optional<ValidThreadInfo> get_info(pid_t pid) {
      const std::lock_guard<std::recursive_mutex> lock(m);
      if (map.find(pid) == map.end()) {
        return {};
      }
      return {map.at(pid)};
    }

    long get_id(pid_t pid) {
      return get_info(pid).value().id;
    }

    const std::string& get_name(long id) {
      const std::lock_guard<std::recursive_mutex> lock(m);
      return names.at(id);
    }

    std::vector<pid_t> get_all_threads() {
      const std::lock_guard<std::recursive_mutex> lock(m);
      std::vector<pid_t> pids;
      for (const auto &it : map) {
        if (wall_clock_mode || is_thread_running(it.second.jthreadId)) {
          pids.emplace_back(it.first);
        }
      }
      return pids;
    }

    std::vector<pid_t> get_shuffled_threads() {
      const std::lock_guard<std::recursive_mutex> lock(m);
      std::vector<pid_t> threads = get_all_threads();
      std::random_device rd;
      std::mt19937 g(rd());
      std::shuffle(threads.begin(), threads.end(), g);
      return std::vector(threads.begin(), threads.begin() + std::min(MAX_THREADS_PER_ITERATION, (int)threads.size()));
    }
};

static ThreadMap thread_map;

typedef void (*SigAction)(int, siginfo_t*, void*);
typedef void (*SigHandler)(int);
typedef void (*TimerCallback)(void*);

template <class T>
class JvmtiDeallocator {
 public:
  JvmtiDeallocator() {
    elem_ = NULL;
  }

  ~JvmtiDeallocator() {
    jvmti->Deallocate(reinterpret_cast<unsigned char*>(elem_));
  }

  T* get() {
    return elem_;
  }

  T** addr() {
    return &elem_;
  }

  T& operator*() {
    return *elem_;
  }

 private:
  T* elem_;
};

static SigAction installSignalHandler(int signo, SigAction action, SigHandler handler = NULL) {
    struct sigaction sa;
    struct sigaction oldsa;
    sigemptyset(&sa.sa_mask);

    if (handler != NULL) {
        sa.sa_handler = handler;
        sa.sa_flags = 0;
    } else {
        sa.sa_sigaction = action;
        sa.sa_flags = SA_SIGINFO | SA_RESTART;
    }

    sigaction(signo, &sa, &oldsa);
    return oldsa.sa_sigaction;
}

void printFrame(ASGST_Frame &frame) {
  char method_name[100];
  char signature[100];
  char class_name[100];
  ASGST_MethodInfo info;
  info.method_name = (char*)method_name;
  info.method_name_length = 100;
  info.signature = (char*)signature;
  info.signature_length = 100;
  info.generic_signature = nullptr;
  ASGST_GetMethodInfo(frame.method, &info);
  ASGST_ClassInfo class_info;
  class_info.class_name = (char*)class_name;
  class_info.class_name_length = 100;
  class_info.generic_class_name = nullptr;
  ASGST_GetClassInfo(info.klass, &class_info);
  printf("  %s.%s\n", class_info.class_name, info.method_name);
}

void printFirstFrame(ASGST_Iterator* iterator, void* arg) {
  ASGST_Frame frame;
  ASGST_NextFrame(iterator, &frame);
  printFrame(frame);
}

const char* addrToNativeMethodName(void* addr) {
  Dl_info dlinfo;

  if (dladdr((void*)addr, &dlinfo) != 0) {
    return dlinfo.dli_sname ? dlinfo.dli_sname : dlinfo.dli_fname;
  } else {
    return "unknown";
  }
}
/* ASGST_NO_THREAD          = -1, // thread is not here
  ASGST_THREAD_EXIT        = -2, // dying thread
  ASGST_UNSAFE_STATE       = -3, // thread is in unsafe state
  ASGST_NO_TOP_JAVA_FRAME  = -4, // no top java frame
  ASGST_ENQUEUE_NO_QUEUE   = -5, // no queue registered
  ASGST_ENQUEUE_FULL_QUEUE = -6, // safepoint queue is full
*/

const char* errorCodeToString(int code) {
  switch (code) {
  case ASGST_NO_THREAD:
    return "thread is not here";
  case ASGST_THREAD_EXIT:
    return "dying thread";
  case ASGST_UNSAFE_STATE:
    return "thread is in unsafe state";
  case ASGST_NO_TOP_JAVA_FRAME:
    return "no top java frame";
  case ASGST_ENQUEUE_NO_QUEUE:
    return "no queue registered";
  case ASGST_ENQUEUE_FULL_QUEUE:
    return "safepoint queue is full";
  case ASGST_ENQUEUE_OTHER_ERROR:
    return "other queue error";
  case ASGST_NO_FRAME:
    return "no frame";
  case 1:
    return "ok";
  default:
    return "unknown error";
  }
}

typedef struct {
  jint lineno;         // BCI in the source file, or < 0 for native methods
  jmethodID method_id; // method executed in this frame
} ASGCT_CallFrame;

typedef struct {
  JNIEnv *env_id;   // Env where trace was recorded
  jint num_frames; // number of frames in this trace, < 0 gives us an error code
  ASGCT_CallFrame *frames; // recorded frames
} ASGCT_CallTrace;

typedef void (*ASGCTType)(ASGCT_CallTrace *, jint, void *);

ASGCTType asgct;

static void initASGCT() {
  asgct = reinterpret_cast<ASGCTType>(dlsym(RTLD_DEFAULT, "AsyncGetCallTrace"));
  if (asgct == NULL) {
    fprintf(stderr, "=== ASGCT not found ===\n");
    exit(1);
  }
}