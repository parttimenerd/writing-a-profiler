/*
 * Based on the linAsyncGetCallTraceTest.cpp and libAsyncGetStackTraceSampler.cpp from the OpenJDK project.
 */

#include <fstream>
#include <stddef.h>

const int MAX_THREADS_PER_ITERATION = 8;
size_t interval_ns = 1000000;  // 1ms

#include "other.hpp"
#include "flamegraph.hpp"

// our stuff




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

Node node{"main"};

static void processTraces(size_t num_threads) {
  for (int i = 0; i < num_threads; i++) {
    auto& trace = global_traces[i];
    if (trace.num_frames <= 0) {
      failedTraces++;
    } else {
      std::cout << "Trace:\n";
      for (auto s : traceToStrings(trace)) {
        std::cout << " " << s << std::endl;
      }
      node.addTrace(traceToStrings(trace));
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
  std::ofstream flames("flames.html");
  node.writeAsHTML(flames, 100);
}

std::atomic<bool> shouldStop = false;
std::thread samplerThread;

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
  initASGCT();
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
