/*
 * Based on the linAsyncGetCallTraceTest.cpp and libAsyncGetStackTraceSampler.cpp from the OpenJDK project.
 */

#include "jni.h"
#include <fstream>
#include <stddef.h>
#include <profile2.h>


const int MAX_THREADS_PER_ITERATION = 8;

#include "other.hpp"
#include "flamegraph.hpp"

Options options;

std::atomic<size_t> failedTraces = 0;
std::atomic<size_t> totalTraces = 0;

int available_trace;
int stored_traces;

const int MAX_DEPTH = 128; // max number of frames to capture

static std::string methodToString(ASGST_Method method) {
  char method_name[100];
  char signature[100];
  char class_name[100];
  ASGST_MethodInfo info;
  info.method_name = (char*)method_name;
  info.method_name_length = 100;
  info.signature = (char*)signature;
  info.signature_length = 100;
  info.generic_signature = nullptr;
  ASGST_GetMethodInfo(method, &info);
  ASGST_ClassInfo class_info;
  class_info.class_name = (char*)class_name;
  class_info.class_name_length = 100;
  class_info.generic_class_name = nullptr;
  ASGST_GetClassInfo(info.klass, &class_info);
  return std::string(class_info.class_name) + "." + std::string(info.method_name) + std::string(info.signature);
}

struct CallTrace {
  std::array<ASGST_Frame, MAX_DEPTH> frames;
  int num_frames;

  std::vector<std::string> to_strings() const {
    std::vector<std::string> strings;
    for (int i = 0; i < num_frames; i++) {
      strings.push_back(methodToString(frames[i].method));
    }
    return strings;
  }
};

static CallTrace global_traces[MAX_THREADS_PER_ITERATION];

void storeTrace(ASGST_Iterator* iterator, void* arg) {
  CallTrace *trace = (CallTrace*)arg;
  ASGST_Frame frame;
  int count;
  for (count = 0; ASGST_NextFrame(iterator, &frame) == 1 && count < MAX_DEPTH; count++) {
    trace->frames[count] = frame;  
  }
  trace->num_frames = count;
}

static void signalHandler(int signo, siginfo_t* siginfo, void* ucontext) {
  CallTrace &trace = global_traces[available_trace++];
  int ret = ASGST_RunWithIterator(ucontext, 0, &storeTrace, &trace);
  if (ret >= 2) {
    ret = 0;
  }
  if (ret <= 0) {
    trace.num_frames = ret;
  }
  stored_traces++;
}

static void initSampler() {
  installSignalHandler(SIGPROF, signalHandler);
}

Node node{"main"};

static void processTraces(size_t num_threads) {
  for (int i = 0; i < num_threads; i++) {
    auto& trace = global_traces[i];
    if (trace.num_frames <= 0) {
      failedTraces++;
    } else {
      if (options.printTraces) {
        std::cerr << "Trace:\n";
        for (auto s : trace.to_strings()) {
          std::cerr << " " << s << std::endl;
        }
      }
      node.addTrace(trace.to_strings());
    }
    totalTraces++;
  }
}

static void sampleThreads() {
  available_trace = 0;
  stored_traces = 0;

  auto threads = thread_map.get_shuffled_threads(MAX_THREADS_PER_ITERATION, options.wall_clock_mode);
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
  std::ofstream flames(options.output_file);
  node.writeAsHTML(flames, 100);
}

std::atomic<bool> shouldStop = false;
std::thread samplerThread;

static void sampleLoop() {
  JNIEnv *env;
  jvm->AttachCurrentThreadAsDaemon((void**)&env, nullptr);
  initSampler();
  std::chrono::nanoseconds interval{options.interval_ns};
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
  sigemptyset(&prof_signal_mask);
  sigaddset(&prof_signal_mask, SIGPROF);
  OnThreadStart(jvmti, jni_env, thread);
  startSamplerThread();
}

extern "C" {

static
jint Agent_Initialize(JavaVM *_jvm, char *opts, void *reserved) {
  options = Options::parseOptions(opts);
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
