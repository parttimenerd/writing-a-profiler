/*
 * Based on the linAsyncGetCallTraceTest.cpp and libAsyncGetStackTraceSampler.cpp from the OpenJDK project.
 */

#include "profile2.h"
#include <fstream>
#include <stddef.h>

const int MAX_THREADS_PER_ITERATION = 8;
size_t interval_ns = 1000000;  // 1ms

#include "profile2.h"
#include <vector>
#include <algorithm>
#include "other.hpp"
#include "flamegraph.hpp"

// our stuff

thread_local ASGST_Queue* queue;
thread_local ASGST_FrameMark* frameMark;

std::atomic<size_t> failedTraces = 0;
std::atomic<size_t> totalTraces = 0;

int available_trace;
int stored_traces;

const int MAX_DEPTH = 512; // max number of frames to capture

const size_t DIFFERENCE = 5;

struct StoredTraceEntry {
  std::string name;
  void* sp;
};

class StoredTrace {
  std::vector<StoredTraceEntry> _trace;
  size_t _firstValidIdx = 0;

  void setWatermark() {
    void* sp = empty() ? nullptr : first().sp;
    ASGST_MoveFrameMark(frameMark, sp);
  }
public:
  size_t size() const { return _trace.size() - _firstValidIdx; }
  bool empty() const { return size() == 0; }

  StoredTraceEntry operator[](size_t idx) { return _trace[idx + _firstValidIdx]; }
  StoredTraceEntry first() const { return _trace[_firstValidIdx]; }
  void moveValidIdxBack(size_t inc = DIFFERENCE, void* foundSP = nullptr) { 
    _firstValidIdx = std::min(_firstValidIdx + inc, _trace.size());
    for (size_t i = _firstValidIdx + inc; i < _trace.size(); i++) {
      if (_trace.at(i).sp && (foundSP == nullptr || foundSP < _trace.at(i).sp)) { // ignore frames without stack pointers
        _firstValidIdx = i;
        break;
      }
    }
    setWatermark();
  }
  void reset(std::vector<StoredTraceEntry> &trace) {
    std::vector<StoredTraceEntry> newTrace = trace;
    for (size_t i = _firstValidIdx; i < _trace.size(); i++) {
      newTrace.push_back(_trace[i]);
    }
    _trace = newTrace;
    _firstValidIdx = 0;
    printf("reset size %lu traceSize %lu\n", size(), trace.size());
    moveValidIdxBack();
  }

  std::vector<std::string> names() const {
    std::vector<std::string> names;
    for (size_t i = _firstValidIdx; i < _trace.size(); i++) {
      names.push_back(_trace[i].name);
    }
    return names;
  }

  auto begin() const {
    return _trace.begin() + _firstValidIdx;
  }
  auto end() const {
    return _trace.end();
  }
};

thread_local StoredTrace storedTrace;

void frameMarkHandler(ASGST_FrameMark* frameMark, ASGST_Iterator* iter, void* arg) {
  storedTrace.moveValidIdxBack();
}

Node node{"main"};

static std::string methodToString(ASGST_Frame frame) {
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
  return std::string(class_info.class_name) + "." + std::string(info.method_name) + std::string(info.signature);
}

static void asgstHandler(ASGST_Iterator* iterator, void* queueArg, void* arg) {
  std::vector<std::string> trace;
  std::vector<StoredTraceEntry> sTrace;
  ASGST_Frame frame;
  printf("asgstHandler\n");
  /*for (int count = 0; ASGST_NextFrame(iterator, &frame) > 0 && count < MAX_DEPTH; count++) {
    if (frame.sp && (!storedTrace.empty() && storedTrace.first().sp == frame.sp)) {
      std::vector<std::string> names = storedTrace.names();
      printf("obtained from store %s...\n", methodToString(frame).c_str());
      trace.insert(trace.end(), names.begin(), names.end());
      if (count > DIFFERENCE) {
        sTrace.insert(sTrace.end(), storedTrace.begin(), storedTrace.end());
        storedTrace.reset(sTrace);
      }
      break;
    }
    std::string name = methodToString(frame);
    trace.push_back(name);
    sTrace.push_back(StoredTraceEntry{name, frame.sp});
  }
  if (storedTrace.empty()) {
    storedTrace.reset(sTrace);
  }
  node.addTrace(trace);*/
}

static void signalHandler(int signo, siginfo_t* siginfo, void* ucontext) {
  if (queue) {
    int r = ASGST_Enqueue(queue, ucontext, nullptr);
    totalTraces++;
    if (r < 0) {
      failedTraces++;
      printf("Failed %d queue size %d\n", r, ASGST_QueueSize(queue));
      ASGST_RunWithIterator(ucontext, 0, printFirstFrame, nullptr);
    }
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
}

static void endSampler() {
  printf("Failed traces: %10zu\n", failedTraces.load());
  printf("Total traces:  %10zu\n", totalTraces.load());
  printf("Failed ratio:  %10.2f%%\n", (double)failedTraces.load() / totalTraces.load() * 100);
  std::ofstream flames("flames.html");
  node.writeAsHTML(flames, 100);
}

std::atomic<bool> shouldStop = false;
std::thread samplerThread;

static void sampleLoop() {
  jvm->AttachCurrentThreadAsDaemon((void**)&env, nullptr);
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

static void initQueue(JNIEnv* env) {
  queue = ASGST_RegisterQueue(env, 100, 0, &asgstHandler, nullptr);
  frameMark = ASGST_RegisterFrameMark(env, &frameMarkHandler, 0, (void*)nullptr);
}



// complicated stuff

static void startSamplerThread() {
  installSignalHandler(SIGPROF, signalHandler);
  samplerThread = std::thread(sampleLoop);
}

sigset_t prof_signal_mask;

void JNICALL
OnThreadStart(jvmtiEnv *jvmti_env,
            JNIEnv* jni_env,
            jthread thread) {
  jvmtiThreadInfo info;           
  ensureSuccess(jvmti->GetThreadInfo(thread, &info), "GetThreadInfo");
  printf("thread %s\n", info.name);
  thread_map.add(get_thread_id(), info.name, thread);
  pthread_sigmask(SIG_UNBLOCK, &prof_signal_mask, NULL);
  initQueue(jni_env);
}

void JNICALL
OnThreadEnd(jvmtiEnv *jvmti_env,
            JNIEnv* jni_env,
            jthread thread) {
  pthread_sigmask(SIG_BLOCK, &prof_signal_mask, NULL);
  thread_map.remove(get_thread_id());
}

static void JNICALL OnVMInit(jvmtiEnv *jvmti, JNIEnv *jni_env, jthread thread) {
  if (env == nullptr) {
    env = jni_env;
    sigemptyset(&prof_signal_mask);
    sigaddset(&prof_signal_mask, SIGPROF);
    startSamplerThread();
  }
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
  callbacks.VMInit = &OnVMInit;
  callbacks.ThreadStart = &OnThreadStart;
  callbacks.ThreadEnd = &OnThreadEnd;

  ensureSuccess(jvmti->SetEventCallbacks(&callbacks, sizeof(jvmtiEventCallbacks)), "EventCallbacks");
  ensureSuccess(jvmti->SetEventNotificationMode(JVMTI_ENABLE, JVMTI_EVENT_VM_INIT, NULL), "VMInit");
  ensureSuccess(jvmti->SetEventNotificationMode(JVMTI_ENABLE, JVMTI_EVENT_THREAD_START, NULL),
      "AgentInitialize: Error in SetEventNotificationMode for THREAD_START");
  ensureSuccess(jvmti->SetEventNotificationMode(JVMTI_ENABLE, JVMTI_EVENT_THREAD_END, NULL),
      "AgentInitialize: Error in SetEventNotificationMode for THREAD_END");
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
