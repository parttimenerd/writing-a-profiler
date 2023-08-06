#pragma once

#include <algorithm>
#include <atomic>
#include <assert.h>
#include <chrono>
#include <iostream>
#include <dlfcn.h>
#include <mutex>
#include <optional>
#include <signal.h>
#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <tuple>
#include <thread>
#include <random>
#include <unordered_map>
#include <vector>
#include <sys/types.h>
#include <sys/time.h>

#include <pthread.h>

#if defined(__linux__)
#include <sys/syscall.h>
#include <unistd.h>
#endif

#include "jvmti.h"


jvmtiEnv* jvmti;
JavaVM* jvm;

void ensureSuccess(jvmtiError err, const char *msg) {
  if (err != JVMTI_ERROR_NONE) {
    fprintf(stderr, "Error in %s: %d", msg, err);
    exit(1);
  }
}

pid_t get_thread_id() {
  #if defined(__APPLE__) && defined(__MACH__)
  uint64_t tid;
  pthread_threadid_np(NULL, &tid);
  return (pid_t) tid;
  #else
  return syscall(SYS_gettid);
  #endif
}



static size_t parseTimeToNanos(char *str) {
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

struct Options {
  size_t interval_ns;
  bool wall_clock_mode;
  std::string output_file;
  bool printTraces;

  static void printHelp() {
    fprintf(stderr, "Usage: -agentpath:libSmallProfiler.so=<options>\n");
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  interval=<time>  - sampling interval, default 1ms\n");
    fprintf(stderr, "  cpu              - sample CPU time instead of wall clock time\n");
    fprintf(stderr, "  output=<file>    - output file, default flames.html\n");
    fprintf(stderr, "  printTraces      - print stack traces to stderr\n");
    exit(1);
  }

  /** parse all options, like interval, and store them into global variables */
  static Options parseOptions(char *options) {
    size_t interval_ns = 1000000;
    bool wall_clock_mode = true;
    std::string output_file = "flames.html";
    bool printTraces = false;
    char *token = strtok(options, ",");
    while (token != NULL) {
      if (strncmp(token, "interval=", 9) == 0) {
        interval_ns = parseTimeToNanos(token + 9);
      } else  if (strncmp(token, "cpu", 3) == 0) {
        wall_clock_mode = false;
      } else if (strncmp(token, "output=", 7) == 0) {
        output_file = token + 7;
      } else if (strcmp(token, "printTraces") == 0) {
        printTraces = true;
      } else {
        printHelp();
      }
      token = strtok(NULL, ",");
    }
    return Options{.interval_ns = interval_ns, .wall_clock_mode = wall_clock_mode, 
      .output_file = output_file, .printTraces = printTraces};
  }
};

static bool is_thread_running(jthread thread) {
  jint state;
  auto err = jvmti->GetThreadState(thread, &state);
  return err == 0 && state != JVMTI_THREAD_STATE_RUNNABLE;
}


struct ValidThreadInfo {
    jthread jthreadId;
    pthread_t pthread;
    bool is_running;
    long id;
};

class ThreadMap {
  std::recursive_mutex m;
  std::unordered_map<pid_t, ValidThreadInfo> map;
  std::vector<std::string> names;

  public:

    void add(pid_t pid, std::string name, jthread thread) {
      const std::lock_guard<std::recursive_mutex> lock(m);
      map[pid] = ValidThreadInfo{.jthreadId = thread, .pthread = pthread_self(), .id = (long)names.size()};
      names.emplace_back(name);
    }

    void remove(pid_t pid) {
      const std::lock_guard<std::recursive_mutex> lock(m);
      map.erase(pid);
    }

    std::optional<ValidThreadInfo> get_info(pid_t pid) {
      const std::lock_guard<std::recursive_mutex> lock(m);
      if (map.find(pid) == map.end()) {
        return {};
      }
      return {map.at(pid)};
    }

    long get_id(pid_t pid) {
      return get_info(pid).value().id;
    }

    const std::string& get_name(long id) {
      const std::lock_guard<std::recursive_mutex> lock(m);
      return names.at(id);
    }

    std::vector<pid_t> get_all_threads(bool wall_clock_mode) {
      const std::lock_guard<std::recursive_mutex> lock(m);
      std::vector<pid_t> pids;
      for (const auto &it : map) {
        if (wall_clock_mode || is_thread_running(it.second.jthreadId)) {
          pids.emplace_back(it.first);
        }
      }
      return pids;
    }

    std::vector<pid_t> get_shuffled_threads(int max_threads, bool wall_clock_mode) {
      const std::lock_guard<std::recursive_mutex> lock(m);
      std::vector<pid_t> threads = get_all_threads(wall_clock_mode);
      std::random_device rd;
      std::mt19937 g(rd());
      std::shuffle(threads.begin(), threads.end(), g);
      return std::vector(threads.begin(), threads.begin() + std::min(max_threads, (int)threads.size()));
    }
};

static ThreadMap thread_map;

typedef void (*SigAction)(int, siginfo_t*, void*);
typedef void (*SigHandler)(int);
typedef void (*TimerCallback)(void*);

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