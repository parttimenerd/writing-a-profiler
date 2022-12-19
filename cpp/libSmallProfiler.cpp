/*
 * Based on the linAsyncGetCallTraceTest.cpp and libAsyncGetStackTraceSampler.cpp from the OpenJDK project.
 */

#include <assert.h>
#include <dlfcn.h>
#include <signal.h>
#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <atomic>
#include <sys/types.h>
#include <sys/time.h>
#include "jvmti.h"
#include "profile.h"

static jvmtiEnv* jvmti;
static JNIEnv* env;

static size_t interval_ns = 1000000;  // 1ms

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
    token = strtok(NULL, ",");
  }
}

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

static void signalHandler(int signo, siginfo_t* siginfo, void* ucontext) {
  const int MAX_DEPTH = 512; // max number of frames to capture
  static ASGCT_CallFrame frames[MAX_DEPTH];
  ASGCT_CallTrace trace;
  trace.frames = frames;
  trace.num_frames = 0;
  trace.env_id = env;
  asgct(&trace, MAX_DEPTH, ucontext);
  totalTraces++;
  if (trace.num_frames < 0) {
    failedTraces++;
  }
}

static bool startITimerSampler() {
  time_t sec = interval_ns / 1000000000;
  suseconds_t usec = (interval_ns % 1000000000) / 1000;
  struct itimerval tv = {{sec, usec}, {sec, usec}};

  installSignalHandler(SIGPROF, signalHandler);

  if (setitimer(ITIMER_PROF, &tv, NULL) != 0) {
    return false;
  }
  return true;
}

static void JNICALL OnVMInit(jvmtiEnv *jvmti, JNIEnv *jni_env, jthread thread) {
  jint class_count = 0;
  env = jni_env;
  // Get any previously loaded classes that won't have gone through the
  // OnClassPrepare callback to prime the jmethods for AsyncGetStackTrace.
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
  
  startITimerSampler();
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
  printf("Failed traces: %10zu\n", failedTraces.load());
  printf("Total traces:  %10zu\n", totalTraces.load());
  printf("Failed ratio:  %10.2f%%\n", (double)failedTraces.load() / totalTraces.load() * 100);
}

}
