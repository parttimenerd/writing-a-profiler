This is a reproducer for a bug regarding GetStackTrace:

```sh

test -e renaissance.jar || wget https://github.com/renaissance-benchmarks/renaissance/releases/download/v0.14.2/renaissance-gpl-0.14.2.jar -O renaissance.jar

# on linux
g++ cpp/libSmallProfiler.cpp -I$JAVA_HOME/include/linux -I$JAVA_HOME/include -o libSmallProfiler.so -std=c++17 -shared -fPIC
java -agentpath:./libSmallProfiler.so=interval=0.0001s -jar renaissance.jar all
```

The code essentially calls GetStackTrace on every jthread at every interval:

```
static void sampleThread(jthread thread) {
  jvmtiFrameInfo gstFrames[MAX_DEPTH];
  jint gstCount = 0;
  jvmti->GetStackTrace(thread, 0, MAX_DEPTH, gstFrames, &gstCount);
}
```

Running the agent on an x86_64 Linux with a `52d30087734ad95` JDK build) results in a segfault at:

```
V  [libjvm.so+0xb2fb09]  Klass::is_subclass_of(Klass const*) const+0x9  (klass.hpp:212)
V  [libjvm.so+0xae1188]  JvmtiEnv::GetStackTrace(_jobject*, int, int, jvmtiFrameInfo*, int*)+0xa8  (jvmtiEnv.cpp:1718)
V  [libjvm.so+0xa95493]  jvmti_GetStackTrace+0x113  (jvmtiEnter.cpp:1221)
C  [libSmallProfiler.so+0x1486a]  _jvmtiEnv::GetStackTrace(_jobject*, int, int, jvmtiFrameInfo*, int*)+0x52
C  [libSmallProfiler.so+0x137ce]  sampleThread(_jobject*)+0x78
C  [libSmallProfiler.so+0x1388b]  sampleThreads()+0x7b
C  [libSmallProfiler.so+0x13950]  sampleLoop()+0x5c
C)
```

License
-------
GPLv2 (like the OpenJDK)
