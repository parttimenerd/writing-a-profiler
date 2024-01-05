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
#include <condition_variable>
#include <queue>

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

static void GetJMethodIDs(jclass klass) {
  jint method_count = 0;
  JvmtiDeallocator<jmethodID> methods;
  jvmtiError err = jvmti->GetClassMethods(klass, &method_count, methods.addr());

  // If ever the GetClassMethods fails, just ignore it, it was worth a try.
  if (err != JVMTI_ERROR_NONE && err != JVMTI_ERROR_CLASS_NOT_PREPARED) {
    fprintf(stderr, "GetJMethodIDs: Error in GetClassMethods: %d\n", err);
  }
}

// AsyncGetStackTrace needs class loading events to be turned on!
static void JNICALL OnClassLoad(jvmtiEnv *jvmti, JNIEnv *jni_env,
                                jthread thread, jclass klass) {
}

static void JNICALL OnClassPrepare(jvmtiEnv *jvmti, JNIEnv *jni_env,
                                   jthread thread, jclass klass) {
  // We need to do this to "prime the pump" and get jmethodIDs primed.
  GetJMethodIDs(klass);
}

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

std::vector<std::string> traceToStrings(jmethodID* trace, int num_frames) {
  std::vector<std::string> ret;
  for (int i = 0; i < num_frames; i++) {
    jmethodID method = trace[i];
    JvmtiDeallocator<char> name;
    JvmtiDeallocator<char> signature;
    if (jvmti->GetMethodName(method, name.addr(), signature.addr(), nullptr) != JVMTI_ERROR_NONE) {
      continue;
    }
    jclass klass;
    JvmtiDeallocator<char> className;
    jvmti->GetMethodDeclaringClass(method, &klass);
    jvmti->GetClassSignature(klass, className.addr(), nullptr);
    ret.push_back(std::string(className.get()) + std::string(name.get()) + std::string(signature.get()));
  }
  return ret;
}

template <typename T>
class SynchronizedQueue {
private:
  std::queue<T> queue;
  std::mutex mutex;
  std::condition_variable cv;

public:
  void push(const T& item) {
    std::lock_guard<std::mutex> lock(mutex);
    queue.push(item);
    cv.notify_one();
  }

  bool pop(T* item) {
    std::lock_guard<std::mutex> lock(mutex);
    if (queue.empty()) {
      return false;
    }
    *item = queue.front();
    queue.pop();
    return true;
  }

  bool empty() {
    std::lock_guard<std::mutex> lock(mutex);
    return queue.empty();
  }
};