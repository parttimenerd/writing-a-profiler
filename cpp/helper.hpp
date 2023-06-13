/*
 * Copyright (c) 2023, SAP SE. All rights reserved.
 * Copyright (c) 2022, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 */

#pragma once

#include <sstream>
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 600
#endif

#include <array>
#include <assert.h>
#include <chrono>
#include <signal.h>
#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/ucontext.h>
#include <ucontext.h>
#include <utility>
#include "jni.h"
#include "jvmti.h"
#include "profile.h"
#include <mutex>
#include <dlfcn.h>

#if defined(__APPLE__) && defined(__MACH__)
#include <mach/mach.h>
#include <mach/task.h>
#include <mach/mach_vm.h>
#endif

#ifdef DEBUG
// more space for debug info
const int METHOD_HEADER_SIZE = 0x200;
const int METHOD_PRE_HEADER_SIZE = 0x20;
#else
const int METHOD_HEADER_SIZE = 0x100;
const int METHOD_PRE_HEADER_SIZE = 0x10;
#endif

static jvmtiEnv* jvmti;

typedef void (*SigAction)(int, siginfo_t*, void*);
typedef void (*SigHandler)(int);
typedef void (*TimerCallback)(void*);


template <class T>
class JvmtiDeallocator {
 public:
  JvmtiDeallocator() {
    elem_ = nullptr;
  }

  ~JvmtiDeallocator() {
    jvmti->Deallocate(reinterpret_cast<unsigned char*>(elem_));
  }

  T* get_addr() {
    return &elem_;
  }

  T get() {
    return elem_;
  }

 private:
  T elem_;
};

void ensureSuccess(jvmtiError err, const char *msg) {
  if (err != JVMTI_ERROR_NONE) {
        std::stringstream ss;
    ss << "Error in " << msg << ": " << err;
    throw std::runtime_error(ss.str());
  }
}

void ensureSuccess(jint err, const char *msg) {
  if (err != JNI_OK) {
        std::stringstream ss;
    ss << "Error in " << msg << ": " << err;
    throw std::runtime_error(ss.str());
  }
}

static void GetJMethodIDs(jclass klass) {
  jint method_count = 0;
  JvmtiDeallocator<jmethodID*> methods;
  jvmti->GetClassMethods(klass, &method_count, methods.get_addr());
}

void printMethod(FILE* stream, jmethodID method) {
  JvmtiDeallocator<char*> name;
  JvmtiDeallocator<char*> signature;
  ensureSuccess(jvmti->GetMethodName(method, name.get_addr(), signature.get_addr(), nullptr), "GetMethodName");
  jclass klass;
  JvmtiDeallocator<char*> className;
  jvmti->GetMethodDeclaringClass(method, &klass);
  jvmti->GetClassSignature(klass, className.get_addr(), nullptr);
  fprintf(stream, "%s.%s%s", className.get(), name.get(), signature.get());
}

typedef struct {
    jint lineno;                      // line number in the source file
    jmethodID method_id;              // method executed in this frame
} ASGCT_CallFrame;

typedef struct {
    JNIEnv *env_id;                   // Env where trace was recorded
    jint num_frames;                  // number of frames in this trace
    ASGCT_CallFrame *frames;          // frames
} ASGCT_CallTrace;

typedef void (*ASGCTType)(ASGCT_CallTrace *, jint, void *);

static ASGCTType asgct = nullptr;

bool isASGCTNativeFrame(ASGCT_CallFrame frame) {
  return frame.lineno == -3;
}

void printASGCTFrame(FILE* stream, ASGCT_CallFrame frame) {
  JvmtiDeallocator<char*> name;
  ensureSuccess(jvmti->GetMethodName(frame.method_id, name.get_addr(), nullptr, nullptr), "GetMethodName");
  if (isASGCTNativeFrame(frame)) {
    fprintf(stream, "Native frame ");
    printMethod(stream, frame.method_id);
  } else {
    fprintf(stream, "Java frame   ");
    printMethod(stream, frame.method_id);
    fprintf(stream, ": %d", frame.lineno);
  }
}

void printASGCTFrames(FILE* stream, ASGCT_CallFrame *frames, int length) {
  for (int i = 0; i < length; i++) {
    fprintf(stream, "Frame %d: ", i);
    printASGCTFrame(stream, frames[i]);
    fprintf(stream, "\n");
  }
}

void printASGCTTrace(FILE* stream, ASGCT_CallTrace trace) {
  fprintf(stream, "Trace length: %d\n", trace.num_frames);
  if (trace.num_frames > 0) {
    printASGCTFrames(stream, trace.frames, trace.num_frames);
  }
}

void initASGCT() {
  void *mptr = dlsym(RTLD_DEFAULT, "AsyncGetCallTrace");
  if (mptr == nullptr) {
    fprintf(stderr, "Error: could not find AsyncGetCallTrace!\n");
    exit(0);
  }
  asgct = reinterpret_cast<ASGCTType>(mptr);
}

typedef void (*SigAction)(int, siginfo_t *, void *);
typedef void (*SigHandler)(int);
typedef void (*TimerCallback)(void *);

static SigAction installSignalHandler(int signo, SigAction action,
                                      SigHandler handler = nullptr) {
  struct sigaction sa;
  struct sigaction oldsa;
  sigemptyset(&sa.sa_mask);

  if (handler != nullptr) {
    sa.sa_handler = handler;
    sa.sa_flags = 0;
  } else {
    sa.sa_sigaction = action;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
  }

  sigaction(signo, &sa, &oldsa);
  return oldsa.sa_sigaction;
}

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