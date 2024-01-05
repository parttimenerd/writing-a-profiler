#include <fstream>
#include <stddef.h>

const int MAX_THREADS_PER_ITERATION = 8;
size_t interval_ns = 1000000;  // 1ms

#include "other.hpp"
#include "flamegraph.hpp"
#include <profile.h>
#include <iomanip>
// our stuff




std::atomic<size_t> failedTraces = 0;
std::atomic<size_t> totalTraces = 0;
std::atomic<size_t> get_top_frame_in_signal_handler_failed = 0;

std::atomic<size_t> stored_in_queue;

std::atomic<size_t> tried_at_safepoint = 0;
std::atomic<size_t> compute_top_frame_failed = 0;
std::atomic<size_t> walk_stack_failed = 0;

const int MAX_DEPTH = 512; // max number of frames to capture

const int QUEUE_SIZE = 1024 * 10; // max size of safepoint queues


// a lockless bounded queue with a fixed size, elements are of type ASGST_Frame
// size is set in the constructor and cannot be changed
class FrameQueue {

  std::vector<ASGST_Frame> buffer;
  std::atomic<size_t> head;
  std::atomic<size_t> tail;
  std::atomic<size_t> failedTraces = 0;

public:
  FrameQueue(size_t size) : head(0), tail(0) {
    buffer.resize(size);
  }

  size_t size() const {
    return head.load(std::memory_order_acquire) - tail.load(std::memory_order_relaxed);
  }

  bool push(ASGST_Frame frame) {
    size_t h = head.load(std::memory_order_relaxed);
    size_t t = tail.load(std::memory_order_acquire);
    if (h - t == buffer.size()) {
      return false;
    }
    buffer[h % buffer.size()] = frame;
    head.store(h + 1, std::memory_order_release);
    return true;
  }

  bool pop(ASGST_Frame *frame) {
    size_t h = head.load(std::memory_order_acquire);
    size_t t = tail.load(std::memory_order_relaxed);
    if (h == t) {
      return false;
    }
    *frame = buffer[t % buffer.size()];
    tail.store(t + 1, std::memory_order_release);
    return true;
  }

  size_t failed() {
    return failedTraces.load();
  }

  void incFailed() {
    failedTraces++;
  }

  void clear() {
    head.store(0, std::memory_order_release);
    tail.store(0, std::memory_order_release);
    failedTraces.store(0, std::memory_order_release);
  }
};


static int storeFrame(ASGST_FrameInfo *frame, std::vector<jmethodID> *frames) {
  if (frames->size() < MAX_DEPTH) {
    frames->push_back(frame->method);
    return 1;
  }
  return 0;
}

Node node{"main"};



SynchronizedQueue<std::vector<jmethodID>> tracesToAddQueue;

void walkStackAtSafepoint(ASGST_Frame frame) {
  tried_at_safepoint++;
  std::vector<jmethodID> frames;
  auto top_frame = ASGST_ComputeTopFrameAtSafepoint(frame);
  if (top_frame.pc == nullptr) {
    failedTraces++;
    compute_top_frame_failed++;
    return;
  }
  int ret = ASGST_WalkStackFromFrame(top_frame, (ASGST_WalkStackCallback)storeFrame, nullptr, &frames, ASGST_RECONSTITUTE);
  if (ret <= 0) {
    walk_stack_failed++;
    failedTraces++;
    return;
  }
  tracesToAddQueue.push(frames);
}


void handleSafepoint(FrameQueue *queue) {
  failedTraces += queue->failed();
  totalTraces += queue->size() + queue->failed();
  while (queue->size() > 0) {
    ASGST_Frame frame;
    if (queue->pop(&frame)) {
      walkStackAtSafepoint(frame);
    } else {
      break;
    }
  }
  queue->clear();
}

thread_local FrameQueue queue{QUEUE_SIZE};

// setup the safepoint callback for the current thread
void setupSafepointCallback() {
  ASGST_SetSafepointCallback((ASGST_SafepointCallback)handleSafepoint, (void*)&queue);
}


static void signalHandler(int signo, siginfo_t* siginfo, void* ucontext) {
  ASGST_Frame top_frame = ASGST_GetFrame(ucontext, true);
  if (top_frame.pc == nullptr) {
    queue.incFailed();
    get_top_frame_in_signal_handler_failed++;
    stored_in_queue++;
    return;
  }
  if (!queue.push(top_frame)) {
    queue.incFailed();
    printf("queue full, size %ld\n", queue.size());
  }
  ASGST_TriggerSafePoint();
  stored_in_queue++;
}

static void initSampler() {
  installSignalHandler(SIGPROF, signalHandler);
}

static void sampleThreads() {
  stored_in_queue = 0;

  auto threads = thread_map.get_shuffled_threads();
  int count = 0;
  for (pid_t thread : threads) {
    auto info = thread_map.get_info(thread);
    if (info) {
      pthread_kill(info->pthread, SIGPROF);
      count++;
    }
  }
  while (stored_in_queue < count);
}

static void endSampler() {
  std::cout << std::left << std::setw(30) << "Failed traces:" << std::setw(10) << failedTraces.load() << std::endl;
  std::cout << std::left << std::setw(30) << "Total traces:" << std::setw(10) << totalTraces.load() << std::endl;
  std::cout << std::left << std::setw(30) << "Failed ratio:" << std::setw(10) << std::fixed << std::setprecision(2)
            << (double)failedTraces.load() / totalTraces.load() * 100 << "%" << std::endl;
  std::cout << std::left << std::setw(30) << "Top frame in signal handler failed:" << std::setw(10)
            << get_top_frame_in_signal_handler_failed.load() << std::endl;
  std::cout << std::left << std::setw(30) << "Tried at safepoint:" << std::setw(10) << tried_at_safepoint.load() << std::endl;
  std::cout << std::left << std::setw(30) << "Compute top frame failed:" << std::setw(10) << compute_top_frame_failed.load() << std::endl;
  std::cout << std::left << std::setw(30) << "Walk stack failed:" << std::setw(10) << walk_stack_failed.load() << std::endl;
  std::ofstream flames("flames.html");
  node.writeAsHTML(flames, 100);
}

std::atomic<bool> shouldStop = false;
std::thread samplerThread;

void processTraceQueue() {
  while (!shouldStop && !tracesToAddQueue.empty()) {
    std::vector<jmethodID> trace;
    if (tracesToAddQueue.pop(&trace)) {
      node.addTrace(traceToStrings(trace.data(), trace.size()));
    }
  }
}

static void sampleLoop() {
  jvm->AttachCurrentThreadAsDaemon((void**)&env, nullptr);
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
    processTraceQueue();
  }
  endSampler();
}






// complicated stuff

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
  setupSafepointCallback();
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
jint Agent_Initialize(JavaVM *_jvm, char *options, void *reserved) {
  parseOptions(options);
  jvm = _jvm;
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

  ensureSuccess(jvmti->AddCapabilities(&caps), "AddCapabilities");

  jvmtiEventCallbacks callbacks;
  memset(&callbacks, 0, sizeof(callbacks));
  callbacks.ClassLoad = &OnClassLoad;
  callbacks.VMInit = &OnVMInit;
  callbacks.ClassPrepare = &OnClassPrepare;
  callbacks.ThreadStart = &OnThreadStart;
  callbacks.ThreadEnd = &OnThreadEnd;

  ensureSuccess(jvmti->SetEventCallbacks(&callbacks, sizeof(jvmtiEventCallbacks)), "EventCallbacks");
  ensureSuccess(jvmti->SetEventNotificationMode(JVMTI_ENABLE, JVMTI_EVENT_CLASS_LOAD, NULL), "ClassLoad");
  ensureSuccess(jvmti->SetEventNotificationMode(JVMTI_ENABLE, JVMTI_EVENT_CLASS_PREPARE, NULL), "ClassPrepare");
  ensureSuccess(jvmti->SetEventNotificationMode(JVMTI_ENABLE, JVMTI_EVENT_VM_INIT, NULL), "VMInit");
  ensureSuccess(jvmti->SetEventNotificationMode(JVMTI_ENABLE, JVMTI_EVENT_THREAD_START, NULL), "ThreadStart");
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
