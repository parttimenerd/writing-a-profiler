/*
 * Based on the linAsyncGetCallTraceTest.cpp and libAsyncGetStackTraceSampler.cpp from the OpenJDK project.
 */

#include "jni.h"
#include "profile2.h"
#include <cstddef>
#include <fstream>
#include <stddef.h>

const int MAX_THREADS_PER_ITERATION = 8;
size_t interval_ns = 1000000;  // 1ms

#include "other.hpp"
#include "flamegraph.hpp"

// our stuff

thread_local ASGST_Queue *queue;
thread_local JNIEnv* local_env = nullptr;

static void GetJMethodIDs(jclass klass) {
  jint method_count = 0;
  JvmtiDeallocator<jmethodID> methods;
  jvmtiError err = jvmti->GetClassMethods(klass, &method_count, methods.addr());

  // If ever the GetClassMethods fails, just ignore it, it was worth a try.
  if (err != JVMTI_ERROR_NONE && err != JVMTI_ERROR_CLASS_NOT_PREPARED) {
    fprintf(stderr, "GetJMethodIDs: Error in GetClassMethods: %d\n", err);
  }
}

static void JNICALL OnClassPrepare(jvmtiEnv *jvmti, JNIEnv *jni_env,
                                   jthread thread, jclass klass) {
  // We need to do this to "prime the pump" and get jmethodIDs primed.
  GetJMethodIDs(klass);
}

std::atomic<size_t> failedTraces = 0;
std::atomic<size_t> totalTraces = 0;
std::atomic<size_t> successfullyHandlesTraces = 0;
std::atomic<size_t> successfullyHandlesWithCompressedTraces = 0;
std::atomic<size_t> queueFullCount = 0;
std::atomic<size_t> compressed = 0;
std::atomic<size_t> asgctSuccess = 0;


const int MAX_DEPTH = 1024; // max number of frames to capture

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
  size_t traceEncounters = (size_t)arg;
  std::vector<std::string> trace;
  ASGST_Frame frame;
  for (int count = 0; ASGST_NextFrame(iterator, &frame) > 0 && count < MAX_DEPTH; count++) {
    trace.push_back(methodToString(frame));
  }
  /*ASGST_RewindIterator(iterator);
  for (int count = 0; ASGST_NextFrame(iterator, &frame) > 0 && count < 1; count++) {
    printf("  %s %d\n", methodToString(frame).c_str(), frame.bci);
  }*/
  node.addTrace(trace, traceEncounters);
  successfullyHandlesTraces++;
  successfullyHandlesWithCompressedTraces += traceEncounters;
}

void printLastFrames(ASGST_Iterator* iterator, void* arg) {
  ASGST_Frame frame;
  for (int count = 0; ASGST_NextFrame(iterator, &frame) > 0 && count < 2; count++) {
    printf("%s%s %d\n", (char*)arg, methodToString(frame).c_str(), frame.bci);
  }
}

static void signalHandler(int signo, siginfo_t* siginfo, void* ucontext) {
  if (queue) {
    ASGST_QueueSizeInfo size = ASGST_GetQueueSizeInfo(queue);
    totalTraces++;

    void* pc;
    int ret = ASGST_GetEnqueuablePC(ucontext, &pc);
    ASGCT_CallFrame frames[10];
    ASGCT_CallTrace trace;
    trace.frames = frames;
    trace.env_id = local_env;
    asgct(&trace, 10, ucontext);
    if (trace.num_frames > 0) {
      asgctSuccess++;
    }
   /* if (trace.num_frames > 0 && ret != 1) {
      printf("asgst failed where asgct didn't: %s (%d)\n", errorCodeToString(ret), ret);
          int ret = ASGST_GetEnqueuablePC(ucontext, &pc);

          asgct(&trace, 10, ucontext);

    } else if (trace.num_frames <= 0 && ret == 1) {
      printf("asgst failed where asgct did\n");
    }*/
    if (ret != 1) {
      int ret = ASGST_GetEnqueuablePC(ucontext, &pc);
      failedTraces++;
      return;
    }
    // we do some nifty compression
    // we encode the number of hits in the argument
    // the -1st element is the last enqueued element
    // this compression works well for Java methods that call long running C++ methods
    ASGST_QueueElement* last = ASGST_GetQueueElement(queue, -1);
    if (last != nullptr && last->pc == pc) {
      last->arg = (void*)((size_t)last->arg + 1);
      compressed++;
    } else {
      //ASGST_RunWithIterator(ucontext, 0, &printLastFrames, (void*)"    enqueue ");
      //int r = ASGST_EnqueueElement(queue, {pc, (void*)1});
      int r = ASGST_EnqueueElement(queue,{pc, (void*)1});
      if (r == ASGST_ENQUEUE_FULL_QUEUE) {
        queueFullCount++;
      }
    }
  }
}

static void beforeQueueProcessingHandler(ASGST_Queue* queue, ASGST_Iterator* iter, void* arg) {
  ASGST_QueueSizeInfo info = ASGST_GetQueueSizeInfo(queue);
  if (info.attempts > info.capacity / 2 || info.attempts <= info.capacity / 4) {
    int new_size = std::max(info.attempts * 2, 500);
    if (new_size != info.capacity) {
      ASGST_ResizeQueue(queue, new_size);
    }
  }
}

static void sampleThreads() {
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
  printf("Stored traces: %10zu\n", successfullyHandlesTraces.load());
  printf("Stored ratio:  %10.2f%%\n", (double)successfullyHandlesTraces.load() / totalTraces.load() * 100);
  printf("Stored + Compressed/Submitted: %10.2f%%\n", (double)successfullyHandlesWithCompressedTraces.load()  / (totalTraces - failedTraces) * 100);
  printf("Queue full:    %10zu\n", queueFullCount.load());
  printf("Compressed:    %10zu\n", compressed.load());
  printf("ASGCT success: %10zu\n", asgctSuccess.load());
  printf("ASGCT failure ratio:   %10.2f%%\n", (double)(totalTraces - asgctSuccess) / totalTraces * 100);
  std::ofstream flames("flames.html");
  node.writeAsHTML(flames, 100 /* browsers hate large flamegraphs */);
}

std::atomic<bool> shouldStop = false;
std::thread samplerThread;

static void sampleLoop() {

  JavaVMAttachArgs attachArgs = {
    JNI_VERSION_20, 
    (char*)"Profiling Thread", 
    nullptr
  };
  jvm->AttachCurrentThreadAsDaemon((void**)&env, &attachArgs);

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
  queue = ASGST_RegisterQueue(env, 10000, 0, &asgstHandler, nullptr);
  ASGST_SetOnQueueProcessingStart(queue, 0, false, beforeQueueProcessingHandler, nullptr);
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
  local_env = jni_env;
  jvmtiThreadInfo info;           
  ensureSuccess(jvmti->GetThreadInfo(thread, &info), "GetThreadInfo");
  // check that thread is not notification or profiling thread
  if (info.is_daemon) {
    return;
  }

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
  if (env != nullptr) {
    return;
  }

jint class_count = 0;
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

  env = jni_env;
  sigemptyset(&prof_signal_mask);
  sigaddset(&prof_signal_mask, SIGPROF);
  //OnThreadStart(jvmti, jni_env, thread);
  startSamplerThread();
}

extern "C" {

// AsyncGetStackTrace needs class loading events to be turned on!
static void JNICALL OnClassLoad(jvmtiEnv *jvmti, JNIEnv *jni_env,
                                jthread thread, jclass klass) {
}


static
jint Agent_Initialize(JavaVM *_jvm, char *options, void *reserved) {
  parseOptions(options);
  initASGCT();
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
  callbacks.ClassPrepare = &OnClassPrepare;
  callbacks.ClassLoad = &OnClassLoad;

  ensureSuccess(jvmti->SetEventCallbacks(&callbacks, sizeof(jvmtiEventCallbacks)), "EventCallbacks");
  ensureSuccess(jvmti->SetEventNotificationMode(JVMTI_ENABLE, JVMTI_EVENT_VM_INIT, NULL), "VMInit");
  ensureSuccess(jvmti->SetEventNotificationMode(JVMTI_ENABLE, JVMTI_EVENT_THREAD_START, NULL),
      "AgentInitialize: Error in SetEventNotificationMode for THREAD_START");
  ensureSuccess(jvmti->SetEventNotificationMode(JVMTI_ENABLE, JVMTI_EVENT_THREAD_END, NULL),
      "AgentInitialize: Error in SetEventNotificationMode for THREAD_END");
  ensureSuccess(jvmti->SetEventNotificationMode(JVMTI_ENABLE, JVMTI_EVENT_CLASS_PREPARE, NULL), "ClassPrepare");
   err = jvmti->SetEventNotificationMode(JVMTI_ENABLE, JVMTI_EVENT_CLASS_LOAD, NULL);
  if (err != JVMTI_ERROR_NONE) {
    fprintf(stderr, "AgentInitialize: Error in SetEventNotificationMode for CLASS_LOAD: %d\n", err);
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
  return JNI_VERSION_20;
}

JNIEXPORT
void JNICALL Agent_OnUnload(JavaVM *jvm) {
  shouldStop = true;
  samplerThread.join();
}

}
