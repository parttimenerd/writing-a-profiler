/*
 * Based on the linAsyncGetCallTraceTest.cpp and libAsyncGetStackTraceSampler.cpp from the OpenJDK project.
 */

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

#include <pthread.h>

#if defined(__linux__)
#include <sys/syscall.h>
#include <unistd.h>
#endif

#include "jvmti.h"

static jvmtiEnv* jvmti;
static JNIEnv* env;

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

static size_t interval_ns = 1000000;  // 1ms
static bool wall_clock_mode = true;

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

const int MAX_THREADS_PER_ITERATION = 8;

struct ValidThreadInfo {
    jthread thread;
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
      map[pid] = ValidThreadInfo{.thread = thread, .pthread = pthread_self(), .id = (long)names.size()};
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
        if (wall_clock_mode || is_thread_running(it.second.thread)) {
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

std::atomic<size_t> failedTraces = 0;
std::atomic<size_t> totalTraces = 0;

int available_trace;
int stored_traces;

const int MAX_DEPTH = 512; // max number of frames to capture

static ASGCT_CallFrame global_frames[MAX_DEPTH * MAX_THREADS_PER_ITERATION];
static ASGCT_CallTrace global_traces[MAX_THREADS_PER_ITERATION];

static void signalHandler(int signo, siginfo_t* siginfo, void* ucontext) {
  asgct(&global_traces[available_trace++], MAX_DEPTH, ucontext);
  stored_traces++;
}

static void initSampler() {
  for (int i = 0; i < MAX_THREADS_PER_ITERATION; i++) {
    global_traces[i].frames = global_frames + i * MAX_DEPTH;
    global_traces[i].num_frames = 0;
    global_traces[i].env_id = env;
  }
  installSignalHandler(SIGPROF, signalHandler);
}

static void processTraces(size_t num_threads) {
  for (int i = 0; i < num_threads; i++) {
    auto& trace = global_traces[i];
    if (trace.num_frames <= 0) {
      failedTraces++;
    }
    totalTraces++;
  }
}

static void sampleThreads() {
  available_trace = 0;
  stored_traces = 0;

  auto threads = thread_map.get_shuffled_threads();
  for (pid_t thread : threads) {
    auto info = thread_map.get_info(thread);
    if (info) {
      pthread_kill(info->pthread, SIGPROF);
    }
  }
  while (stored_traces < threads.size());
  processTraces(threads.size());
}

static void endSampler() {
  printf("Failed traces: %10zu\n", failedTraces.load());
  printf("Total traces:  %10zu\n", totalTraces.load());
  printf("Failed ratio:  %10.2f%%\n", (double)failedTraces.load() / totalTraces.load() * 100);
}

std::atomic<bool> shouldStop = false;
std::thread samplerThread;

static void sampleLoop() {
  initSampler();
  std::chrono::nanoseconds interval{interval_ns};
  while (!shouldStop) {
    auto start = std::chrono::system_clock::now();
    sampleThreads();
    auto duration = std::chrono::system_clock::now() - start;
   auto sleep = interval - duration;
    if (std::chrono::seconds::zero() < sleep) {
      std::this_thread::sleep_for(sleep);
    }
  }
  endSampler();
}

static void startSamplerThread() {
  samplerThread = std::thread(sampleLoop);
}

sigset_t prof_signal_mask;

void JNICALL
OnThreadStart(jvmtiEnv *jvmti_env,
            JNIEnv* jni_env,
            jthread thread) {
  jvmtiThreadInfo info;           
  ensureSuccess(jvmti->GetThreadInfo(thread, &info), "GetThreadInfo");
  thread_map.add(get_thread_id(), info.name, thread);
  pthread_sigmask(SIG_UNBLOCK, &prof_signal_mask, NULL);
}

void JNICALL
OnThreadEnd(jvmtiEnv *jvmti_env,
            JNIEnv* jni_env,
            jthread thread) {
  pthread_sigmask(SIG_BLOCK, &prof_signal_mask, NULL);
  thread_map.remove(get_thread_id());
}

static void JNICALL OnVMInit(jvmtiEnv *jvmti, JNIEnv *jni_env, jthread thread) {
  jint class_count = 0;
  env = jni_env;
  sigemptyset(&prof_signal_mask);
  sigaddset(&prof_signal_mask, SIGPROF);
  OnThreadStart(jvmti, jni_env, thread);
  // Get any previously loaded classes that won't have gone through the
  // OnClassPrepare callback to prime the jmethods for AsyncGetCallTrace.
  JvmtiDeallocator<jclass> classes;
  jvmtiError err = jvmti->GetLoadedClasses(&class_count, classes.addr());
  if (err != JVMTI_ERROR_NONE) {
    fprintf(stderr, "OnVMInit: Error in GetLoadedClasses: %d\n", err);
    return;
  }

  // Prime any class already loaded and try to get the jmethodIDs set up.
  jclass *classList = classes.get();
  for (int i = 0; i < class_count; ++i) {
    GetJMethodIDs(classList[i]);
  }

  startSamplerThread();
}

extern "C" {

static
jint Agent_Initialize(JavaVM *jvm, char *options, void *reserved) {
  initASGCT();
  parseOptions(options);
  jint res = jvm->GetEnv((void **) &jvmti, JVMTI_VERSION);
  if (res != JNI_OK || jvmti == NULL) {
    fprintf(stderr, "Error: wrong result of a valid call to GetEnv!\n");
    return JNI_ERR;
  }

  jvmtiError err;
  jvmtiCapabilities caps;
  memset(&caps, 0, sizeof(caps));
  caps.can_get_line_numbers = 1;
  caps.can_get_source_file_name = 1;

  err = jvmti->AddCapabilities(&caps);
  if (err != JVMTI_ERROR_NONE) {
    fprintf(stderr, "AgentInitialize: Error in AddCapabilities: %d\n", err);
    return JNI_ERR;
  }

  jvmtiEventCallbacks callbacks;
  memset(&callbacks, 0, sizeof(callbacks));
  callbacks.ClassLoad = &OnClassLoad;
  callbacks.VMInit = &OnVMInit;
  callbacks.ClassPrepare = &OnClassPrepare;
  callbacks.ThreadStart = &OnThreadStart;
  callbacks.ThreadEnd = &OnThreadEnd;

  err = jvmti->SetEventCallbacks(&callbacks, sizeof(jvmtiEventCallbacks));
  if (err != JVMTI_ERROR_NONE) {
    fprintf(stderr, "AgentInitialize: Error in SetEventCallbacks: %d\n", err);
    return JNI_ERR;
  }

  err = jvmti->SetEventNotificationMode(JVMTI_ENABLE, JVMTI_EVENT_CLASS_LOAD, NULL);
  if (err != JVMTI_ERROR_NONE) {
    fprintf(stderr, "AgentInitialize: Error in SetEventNotificationMode for CLASS_LOAD: %d\n", err);
    return JNI_ERR;
  }

  err = jvmti->SetEventNotificationMode(JVMTI_ENABLE, JVMTI_EVENT_CLASS_PREPARE, NULL);
  if (err != JVMTI_ERROR_NONE) {
    fprintf(stderr,
            "AgentInitialize: Error in SetEventNotificationMode for CLASS_PREPARE: %d\n",
            err);
    return JNI_ERR;
  }

  err = jvmti->SetEventNotificationMode(JVMTI_ENABLE, JVMTI_EVENT_VM_INIT, NULL);
  if (err != JVMTI_ERROR_NONE) {
    fprintf(
        stderr, "AgentInitialize: Error in SetEventNotificationMode for VM_INIT: %d\n",
        err);
    return JNI_ERR;
  }

  return JNI_OK;
}

JNIEXPORT
jint JNICALL Agent_OnLoad(JavaVM *jvm, char *options, void *reserved) {
  return Agent_Initialize(jvm, options, reserved);
}

JNIEXPORT
jint JNICALL Agent_OnAttach(JavaVM *jvm, char *options, void *reserved) {
  return Agent_Initialize(jvm, options, reserved);
}

JNIEXPORT
jint JNICALL JNI_OnLoad(JavaVM *jvm, void *reserved) {
  return JNI_VERSION_1_8;
}

JNIEXPORT
void JNICALL Agent_OnUnload(JavaVM *jvm) {
  shouldStop = true;
  samplerThread.join();
}

}
