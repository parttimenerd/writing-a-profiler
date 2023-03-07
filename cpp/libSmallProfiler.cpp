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
#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <tuple>
#include <thread>
#include <random>
#include <unordered_set>
#include <vector>
#include <sys/types.h>
#include <sys/time.h>

#include "jvmti.h"

static jvmtiEnv* jvmti;
static JNIEnv* env;
static JavaVM* jvm;

void ensureSuccess(jvmtiError err, const char *msg) {
  if (err != JVMTI_ERROR_NONE) {
    fprintf(stderr, "Error in %s: %d", msg, err);
    exit(1);
  }
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

class ThreadSet {
  std::recursive_mutex m;
  std::unordered_set<jthread> set;

  public:

    void add(jthread thread) {
      const std::lock_guard<std::recursive_mutex> lock(m);
      set.insert(thread);
    }

    void remove(jthread thread) {
      const std::lock_guard<std::recursive_mutex> lock(m);
      set.erase(thread);
    }

    std::vector<jthread> get_all_threads() {
      const std::lock_guard<std::recursive_mutex> lock(m);
      std::vector<jthread> threads;
      for (auto it : set) {
        if (wall_clock_mode || is_thread_running(it)) {
          threads.emplace_back(it);
        }
      }
      return threads;
    }

    bool contains(jthread thread) {
      const std::lock_guard<std::recursive_mutex> lock(m);
      return set.find(thread) != set.end();
    }
};

static ThreadSet thread_set;

const int MAX_DEPTH = 1024; // max number of frames to capture

std::atomic<bool> shouldStop = false;
std::thread samplerThread;

static void sampleThread(jthread thread) {
  jvmtiFrameInfo gstFrames[MAX_DEPTH];
  jint gstCount = 0;
  jvmti->GetStackTrace(thread, 0, MAX_DEPTH, gstFrames, &gstCount);
}

static void sampleThreads() {
  auto threads = thread_set.get_all_threads();
  for (jthread thread : threads) {
    sampleThread(thread);
  }
}

static void sampleLoop() {
  JNIEnv* newEnv;
  jvm->AttachCurrentThreadAsDaemon((void **) &newEnv, NULL);
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
}

static void startSamplerThread() {
  samplerThread = std::thread(sampleLoop);
}

void JNICALL
OnThreadStart(jvmtiEnv *jvmti_env,
            JNIEnv* jni_env,
            jthread thread) {
  jvmtiThreadInfo info;           
  ensureSuccess(jvmti->GetThreadInfo(thread, &info), "GetThreadInfo");
  thread_set.add(thread);
}

void JNICALL
OnThreadEnd(jvmtiEnv *jvmti_env,
            JNIEnv* jni_env,
            jthread thread) {
  thread_set.remove(thread);
}

static void JNICALL OnVMInit(jvmtiEnv *jvmti, JNIEnv *jni_env, jthread thread) {
  jint class_count = 0;
  env = jni_env;
  OnThreadStart(jvmti, jni_env, thread);
  startSamplerThread();
}

extern "C" {

static
jint Agent_Initialize(JavaVM *_jvm, char *options, void *reserved) {
  jvm = _jvm;
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

  ensureSuccess(jvmti->AddCapabilities(&caps), "AddCapabilities");

  jvmtiEventCallbacks callbacks;
  memset(&callbacks, 0, sizeof(callbacks));
  callbacks.VMInit = &OnVMInit;
  callbacks.ThreadStart = &OnThreadStart;
  callbacks.ThreadEnd = &OnThreadEnd;

  ensureSuccess(jvmti->SetEventCallbacks(&callbacks, sizeof(jvmtiEventCallbacks)), "SetEventCallbacks");
  
  ensureSuccess(jvmti->SetEventNotificationMode(JVMTI_ENABLE, JVMTI_EVENT_THREAD_END, NULL), "SetEventNotificationMode for THREAD_END");
  ensureSuccess(jvmti->SetEventNotificationMode(JVMTI_ENABLE, JVMTI_EVENT_VM_INIT, NULL), "SetEventNotificationMode for VM_INIT");
  ensureSuccess(jvmti->SetEventNotificationMode(JVMTI_ENABLE, JVMTI_EVENT_THREAD_START, NULL), "SetEventNotificationMode for THREAD_START");

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
  fprintf(stderr, "Agent_OnUnload\n");
  shouldStop = true;
  samplerThread.join();
}

}
