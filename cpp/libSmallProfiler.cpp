/*
 * Based on the linAsyncGetCallTraceTest.cpp and libAsyncGetStackTraceSampler.cpp from the OpenJDK project.
 */

#include "jni.h"
#include "profile2.h"
#include <cstddef>
#include <cstdlib>
#include <fstream>
#include <stddef.h>
#include <array>
#include <atomic>

const int MAX_THREADS_PER_ITERATION = 8;
const int MAX_ASGCT_DEPTH = 8;
size_t interval_ns = 1000000;  // 1ms

#include "other.hpp"

// our stuff

thread_local ASGST_Queue *queue;
thread_local JNIEnv* local_env = nullptr;

struct TopMethodEntry {
  ASGST_Method method;
  int bci;
  ASGST_Frame frame;
  std::array<ASGCT_CallFrame, MAX_ASGCT_DEPTH> asgctFrames;
  int asgctFramesCount = 0;
  std::array<ASGST_Frame, MAX_ASGCT_DEPTH> asgstFrames;
  int asgstFramesCount = 0;

  bool valid() const {
    return method != nullptr && method == frame.method && bci == frame.bci;
  }
};

std::array<TopMethodEntry, 1000000> topMethods;
std::atomic<int> topMethodsNextIndex = 0;

int addTopMethod(ASGST_Method method, int bci, ASGST_Frame frame, ASGCT_CallFrame* asgctFrames, int asgctFramesCount, std::array<ASGST_Frame, MAX_ASGCT_DEPTH> asgstFrames, int asgstFramesCount) {
  size_t index = topMethodsNextIndex++ % topMethods.size();
  topMethods[index].method = method;
  topMethods[index].bci = bci;
  topMethods[index].frame = frame;
  for (int i = 0; i < asgctFramesCount; i++) {
    topMethods[index].asgctFrames[i] = asgctFrames[i];
  }
  topMethods[index].asgctFramesCount = asgctFramesCount;
  for (int i = 0; i < asgstFramesCount; i++) {
    topMethods[index].asgstFrames[i] = asgstFrames[i];
  }
  topMethods[index].asgstFramesCount = asgstFramesCount;
  return index;
}

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

std::atomic<size_t> asgctAndAsgstFailedTraces = 0;
std::atomic<size_t> asgctNotWalkableNotJavaOrUnknownTraces = 0;
std::atomic<size_t> failedTraces = 0;
std::atomic<size_t> totalTraces = 0;
std::atomic<size_t> successfullyHandlesTraces = 0;
std::atomic<size_t> successfullyHandlesWithCompressedTraces = 0;
std::atomic<size_t> queueFullCount = 0;
std::atomic<size_t> compressed = 0;
std::atomic<size_t> asgctSuccess = 0;
std::atomic<size_t> failedToObtainFirstFrame = 0;
std::atomic<size_t> asgstUnsafeState = 0;
std::atomic<size_t> asgctASGSTMethodMismatch = 0;
std::atomic<size_t> asgctASGSTBCIMismatch = 0;
std::atomic<size_t> checkCount = 0;
std::atomic<size_t> checkFailureCount = 0;
std::atomic<size_t> checkBCIFailureCount = 0;
std::atomic<size_t> checkFailedASGCTBCIZero = 0;
std::atomic<size_t> checkBCIDifferenceLargerThenTen = 0;
std::atomic<size_t> checkBCIDifferenceLargerThenTwenty = 0;
std::atomic<size_t> checkBCIFailedAndNotInlinedCompiled = 0;
std::atomic<size_t> checkWithSignalASGSTFailed = 0;
std::atomic<size_t> checkWithSignalASGSTMethodMismatch = 0;
std::atomic<size_t> checkWithSignalASGSTBCIMismatch = 0;

const int MAX_DEPTH = 1024; // max number of frames to capture

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

static void asgstHandler(ASGST_Iterator* iterator, void* queueArg, void* arg) {
  if ((int)(long)arg < 0) {
    return;
  }
  auto m = topMethods.at((int)(long)arg);
  std::vector<std::string> trace;
  ASGST_Frame frame;
  successfullyHandlesTraces++;
  int ret = ASGST_NextFrame(iterator, &frame);
  checkCount++;
  if (ret <= 0) {
        checkFailureCount++;
        failedToObtainFirstFrame++;
    printf("Failed to get first frame %d\n", ret);
    return;
  }
  if (frame.method != m.method || std::max(-1, frame.bci) != std::max(-1, m.bci)) {
    checkFailureCount++;
    if (frame.bci != m.bci && frame.method == m.method) {
      checkBCIFailureCount++;
      if (m.bci == 0) {
        checkFailedASGCTBCIZero++;
      }
      if (std::abs(frame.bci - m.bci) > 10) {
        checkBCIDifferenceLargerThenTen++;
      }
      if (std::abs(frame.bci - m.bci) > 20) {
        checkBCIDifferenceLargerThenTwenty++;
      }
      if (frame.type != ASGST_FRAME_JAVA_INLINED && frame.comp_level > 0) {
        checkBCIFailedAndNotInlinedCompiled++;
      }
      printf("First frame bci is not as expected\n");
    } else {
      printf("First frame is not the method we expected:\n");
    }
    printf("  asgct %s:%d\n", methodToString(m.method).c_str(), m.bci);
    printf("  own   %s:%d inlined %d compiled %d\n", methodToString(frame.method).c_str(), frame.bci, frame.type == ASGST_FRAME_JAVA_INLINED, frame.comp_level > 0);
    int count = 0;
    ASGST_Frame frame;
    while (ASGST_NextFrame(iterator, &frame) > 0 && count < 2) {
      printf("       %s inlined=%d compiled=%d pc=%p\n", methodToString(frame.method).c_str(), frame.type == ASGST_FRAME_JAVA_INLINED, frame.comp_level > 0, frame.pc);
      count++;
    }
    for (int i = 0; i < m.asgctFramesCount; i++) {
      auto f = m.asgctFrames[i];
      if (f.method_id != 0) {
        ASGST_Method m = ASGST_JMethodIDToMethod(f.method_id);
        printf("  -- asgct     %s\n", methodToString(m).c_str());
      }
    }
    printf(" ... \n");
    for (int i = 0; i < m.asgstFramesCount; i++) {
      auto f = m.asgstFrames[i];
      if (f.method != nullptr) {
        printf("  -- asgst     %s:%d inlined=%d compiled=%d pc=%p\n", methodToString(f.method).c_str(), f.bci, f.type == ASGST_FRAME_JAVA_INLINED, f.comp_level > 0, f.pc);
      }
    }
    printf("end\n");
  }
  if (frame.bci != m.frame.bci || frame.method != m.frame.method) {
    checkWithSignalASGSTFailed++;
    if (frame.bci != m.frame.bci && frame.method == m.frame.method) {
      checkWithSignalASGSTBCIMismatch++;
      printf("First frame bci is not as expected (asgst)\n");
      printf("frame info: pc %p sp %p fp %p compiled %d inlined %d\n", frame.pc, frame.sp, frame.fp, frame.comp_level > 0, frame.type == ASGST_FRAME_JAVA_INLINED);
      printf("m.fra info: pc %p sp %p fp %p compiled %d inlined %d\n", m.frame.pc, m.frame.sp, m.frame.fp, m.frame.comp_level > 0, m.frame.type == ASGST_FRAME_JAVA_INLINED);
    } else {
      checkWithSignalASGSTMethodMismatch++;
      printf("First frame is not the method we expected (asgst):\n");
    }
    printf("  asgst %s:%d\n", methodToString(m.method).c_str(), m.frame.bci);
    printf("  own   %s:%d inlined %d compiled %d\n", methodToString(frame.method).c_str(), frame.bci, frame.type == ASGST_FRAME_JAVA_INLINED, frame.comp_level > 0);
  }
}

void printFirstFrames(ASGST_Iterator* iterator, void* arg) {
  ASGST_Frame frame;
  int count;
  for (count = 0; ASGST_NextFrame(iterator, &frame) > 0 && count < 2; count++) {
    printf("%s%s %d inlined=%d compiled=%d\n", (char*)arg, methodToString(frame.method).c_str(), frame.bci, frame.type == ASGST_FRAME_JAVA_INLINED, frame.comp_level > 0);
  }
  if (count == 0) {
    printf("%s<empty>\n", (char*)arg);
  }
}

void obtainFirstFrame(ASGST_Iterator* iterator, ASGST_Frame* frame) {
  int ret;
  while ((ret = ASGST_NextFrame(iterator, frame)) == 1) {
    if (frame->type != ASGST_FRAME_JAVA_NATIVE) {
      return;
    }
  }
  if (ret <= 0) {
    printf("Failed to get first frame %d\n", ret);
    return;
  }
}

int findNonNativeFrameIndex(ASGCT_CallTrace trace) {
  for (int i = 0; i < trace.num_frames; i++) {
    if (trace.frames[i].lineno != -3) {
      return i;
    }
  }
  return -1;
}

struct ColRes {
  std::array<ASGST_Frame, MAX_ASGCT_DEPTH> asgstFrames;
  int asgstFramesCount = 0;
};

void collectFrames(ASGST_Iterator* iterator, ColRes* res) {
  ASGST_Frame frame;
  int count;
  for (count = 0; ASGST_NextFrame(iterator, &frame) > 0 && count < MAX_ASGCT_DEPTH; count++) {
    res->asgstFrames[count] = frame;
  }
  res->asgstFramesCount = count;
}

static void signalHandler(int signo, siginfo_t* siginfo, void* ucontext) {
  if (queue) {
    ASGST_QueueSizeInfo size = ASGST_GetQueueSizeInfo(queue);
    totalTraces++;

    ASGST_QueueElement elem;
    int ret = ASGST_GetEnqueuableElement(ucontext, &elem);
    //ASGST_RunWithIterator(ucontext, 0, &printLastFrames, (void*)"  ct ");
    ASGCT_CallFrame frames[MAX_ASGCT_DEPTH];
    ASGCT_CallTrace trace;
    trace.frames = frames;
    trace.env_id = local_env;
    asgct(&trace, MAX_ASGCT_DEPTH, ucontext);
    int mIndex = -1;
    /*printf("------\n");
    for (int i = 0; i < trace.num_frames && i < 4; i++) {
      ASGST_Method m = ASGST_JMethodIDToMethod(trace.frames[i].method_id);
      if (m != nullptr) {
        printf("asgct     %s\n", methodToString(m).c_str());
      }
    }
    printf("------\n");*/
    if (trace.num_frames > 0) {
      int nonNativeIndex = findNonNativeFrameIndex(trace);
      if (nonNativeIndex == -1) {
        asgctAndAsgstFailedTraces++;
        return;
      }
      auto topASGCTFrame = trace.frames[nonNativeIndex];
      auto mId = ASGST_JMethodIDToMethod(topASGCTFrame.method_id);

      ASGST_Frame frame;
      int ret = ASGST_RunWithIterator(ucontext, 0, (void (*)(ASGST_Iterator *, void *))&obtainFirstFrame, &frame);
      if (ret == ASGST_UNSAFE_STATE) {
        asgstUnsafeState++;
      } else if (frame.method != mId) {
        asgctASGSTMethodMismatch++;
        printf("asgct and asgst method mismatch in signal handler: %s != %s\n", methodToString(mId).c_str(), methodToString(frame.method).c_str());
      } else if (std::max(-1, frame.bci) != std::max(-1, topASGCTFrame.lineno)) {
        asgctASGSTBCIMismatch++;
        printf("asgct and asgst bci mismatch in signal handler: %d != %d\n", frame.bci, topASGCTFrame.lineno);
      }
      ColRes asgstFrames;
      int asgstFramesCount = 0;
      ASGST_RunWithIterator(ucontext, 0, (void (*)(ASGST_Iterator *, void *))&collectFrames, &asgstFrames);
      mIndex = addTopMethod(mId, topASGCTFrame.lineno, frame, trace.frames, trace.num_frames, asgstFrames.asgstFrames, asgstFrames.asgstFramesCount);
      asgctSuccess++;
    }
    if (ret != 1) {
      asgctAndAsgstFailedTraces++;
      if (trace.num_frames > 0) {
        int ret = ASGST_GetEnqueuableElement(ucontext, &elem);
        failedTraces++;
        printf("  asgst error %d\n", ret);
      } else if (trace.num_frames == -4 || trace.num_frames == -5) {
        asgctNotWalkableNotJavaOrUnknownTraces++;
      }
      return;
    }
          int r = ASGST_Enqueue(queue, ucontext, (void*)(size_t)mIndex);
          auto enqElem = ASGST_GetQueueElement(queue, -1);
          printf("  enqueued pc %p fp %p sp %p\n", enqElem->pc, enqElem->fp, enqElem->sp);
      if (r == ASGST_ENQUEUE_FULL_QUEUE) {
        queueFullCount++;
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
  size_t total_traces = totalTraces.load();
  size_t asgct_and_asgst_failed_traces = asgctAndAsgstFailedTraces.load();
  size_t asgct_not_walkable_not_java_or_unknown_traces = asgctNotWalkableNotJavaOrUnknownTraces.load();
  size_t failed_traces = failedTraces.load();
  size_t stored_traces = successfullyHandlesTraces.load();
  size_t queue_full_count = queueFullCount.load();
  size_t compressed_count = compressed.load();
  size_t asgct_success_count = asgctSuccess.load();
  size_t check_count = checkCount.load();
  size_t check_failure_count = checkFailureCount.load();
  size_t check_bci_failure_count = checkBCIFailureCount.load();
  size_t check_failed_asgct_bci_zero_count = checkFailedASGCTBCIZero.load();
  size_t failed_to_obtain_first_frame_count = failedToObtainFirstFrame.load();
  size_t check_bci_difference_larger_then_ten = checkBCIDifferenceLargerThenTen.load();
  size_t check_bci_difference_larger_then_twenty = checkBCIDifferenceLargerThenTwenty.load();
  size_t check_bci_failed_and_not_inlined_compiled = checkBCIFailedAndNotInlinedCompiled.load();
  size_t asgst_unsafe_state = asgstUnsafeState.load();
  size_t asgct_asgst_method_mismatch = asgctASGSTMethodMismatch.load();
  size_t asgct_asgst_bci_mismatch = asgctASGSTBCIMismatch.load();
  size_t check_with_signal_asgst_failed = checkWithSignalASGSTFailed.load();
  size_t check_with_signal_asgst_method_mismatch = checkWithSignalASGSTMethodMismatch.load();
  size_t check_with_signal_asgst_bci_mismatch = checkWithSignalASGSTBCIMismatch.load();

  printf("--------------------------------------------------------------\n");
  printf("| %-45s | %10zu |\n", "Total traces", total_traces);
  printf("| %-45s | %10zu | %2.2f%% |\n", "ASGCT and ASGST failed traces", asgct_and_asgst_failed_traces, (double)asgct_and_asgst_failed_traces / total_traces * 100);
  printf("| %-45s | %10zu | %2.2f%% |\n", "ASGCT not walkable / unknown Java", asgct_not_walkable_not_java_or_unknown_traces, (double)asgct_not_walkable_not_java_or_unknown_traces / total_traces * 100);
  printf("| %-45s | %10zu | %2.2f%% |\n", "ASGST unsafe state", asgst_unsafe_state, (double)asgst_unsafe_state / total_traces * 100);
  printf("| %-45s | %10zu | %2.2f%% |\n", "ASGCT/ASGST method mismatch", asgct_asgst_method_mismatch, (double)asgct_asgst_method_mismatch / total_traces * 100);
  printf("| %-45s | %10zu | %2.2f%% |\n", "ASGCT/ASGST bci mismatch", asgct_asgst_bci_mismatch, (double)asgct_asgst_bci_mismatch / total_traces * 100);
  printf("| %-45s | %10zu | %2.2f%% |\n", "Failed traces", failed_traces, (double)failed_traces / total_traces * 100);
  printf("| %-45s | %10zu | %2.2f%% |\n", "Stored traces", stored_traces, (double)stored_traces / total_traces * 100);
  printf("| %-45s | %10zu |\n", "Queue full count", queue_full_count);
  printf("| %-45s | %10zu |\n", "Compressed count", compressed_count);
  printf("| %-45s | %10zu | %2.2f%% |\n", "ASGCT success count", asgct_success_count, (double)asgct_success_count / total_traces * 100);
  printf("| %-45s | %10zu |\n", "ASGCT check count", check_count);
  printf("| %-45s | %10zu | %2.2f%% |\n", "ASGCT check failure count", check_failure_count, (double)check_failure_count / check_count * 100);
  printf("| %-45s | %10zu | %2.2f%% |\n", "ASGCT check BCI failure count", check_bci_failure_count, (double)check_bci_failure_count / check_count * 100);
  printf("| %-45s | %10zu | %2.2f%% |\n", "ASGCT check BCI failed + ASGCT BCI zero count", check_failed_asgct_bci_zero_count, (double)check_failed_asgct_bci_zero_count / check_count * 100);
  printf("| %-45s | %10zu | %2.2f%% |\n", "ASGCT check BCI difference larger then 10", check_bci_difference_larger_then_ten, (double)check_bci_difference_larger_then_ten / check_count * 100);
  printf("| %-45s | %10zu | %2.2f%% |\n", "ASGCT check BCI difference larger then 20", check_bci_difference_larger_then_twenty, (double)check_bci_difference_larger_then_twenty / check_count * 100);
  printf("| %-45s | %10zu | %2.2f%% |\n", "ASGCT check BCI failed -inlined compiled", check_bci_failed_and_not_inlined_compiled, (double)check_bci_failed_and_not_inlined_compiled / check_count * 100);
  printf("| %-45s | %10zu | %2.2f%% |\n", "Failed to obtain first frame count", failed_to_obtain_first_frame_count, (double)failed_to_obtain_first_frame_count / check_count * 100);
  printf("| %-45s | %10zu | %2.2f%% |\n", "ASGST check with signal failed", check_with_signal_asgst_failed, (double)check_with_signal_asgst_failed / check_count * 100);
  printf("| %-45s | %10zu | %2.2f%% |\n", "ASGST check with signal method mismatch", check_with_signal_asgst_method_mismatch, (double)check_with_signal_asgst_method_mismatch / check_count * 100);
  printf("| %-45s | %10zu | %2.2f%% |\n", "ASGST check with signal bci mismatch", check_with_signal_asgst_bci_mismatch, (double)check_with_signal_asgst_bci_mismatch / check_count * 100);
  printf("--------------------------------------------------------------\n");
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
  queue = ASGST_RegisterQueue(env, 100000, 0, &asgstHandler, nullptr);
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
