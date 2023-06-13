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
#include "helper.hpp"

static size_t interval_ns = 1000000;  // 1ms

static void signalHandler(int signo, siginfo_t* siginfo, void* ucontext) {
  
}



void JNICALL Agent_OnUnload(JavaVM *jvm) {
 
}



// boring stuff

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

static void JNICALL OnVMInit(jvmtiEnv *jvmti, JNIEnv *jni_env, jthread thread) {
  jint class_count = 0;
  // Get any previously loaded classes that won't have gone through the
  // OnClassPrepare callback to prime the jmethods for AsyncGetStackTrace.
  JvmtiDeallocator<jclass*> classes;
  ensureSuccess(jvmti->GetLoadedClasses(&class_count, classes.get_addr()), "GetLoadedClasses");

  // Prime any class already loaded and try to get the jmethodIDs set up.
  jclass *classList = classes.get();
  for (int i = 0; i < class_count; ++i) {
    GetJMethodIDs(classList[i]);
  }
  
  startITimerSampler();
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

extern "C" {

static
jint Agent_Initialize(JavaVM *jvm, char *options, void *reserved) {
  initASGCT();
  parseOptions(options);
  ensureSuccess(jvm->GetEnv((void **) &jvmti, JVMTI_VERSION), "GetEnv");

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

  ensureSuccess(jvmti->SetEventCallbacks(&callbacks, sizeof(jvmtiEventCallbacks)), "Callbacks");
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

}
