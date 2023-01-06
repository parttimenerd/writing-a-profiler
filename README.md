Writing a Profiler from Scratch
===============================

This repository belongs to my article series with the same name. 
It is a step-by-step guide to writing a profiler from scratch:

- [Introduction](https://mostlynerdless.de/blog/2022/12/20/writing-a-profiler-from-scratch-introduction/)

This repository currently looks a bit barren, it contains just the example Java code and
the introductory agent code. More will come as the article series progresses.

How to run this all?
--------------------

```sh
# compile the Java sample code
javac samples/BasicSample.java

# compile the small profiler agent
# on mac
g++ libSmallProfiler.cpp -I$JAVA_HOME/include/darwin -I$JAVA_HOME/include -o libSmallProfiler.dylib -std=c++17 -shared -pthread
# on linux
g++ libSmallProfiler.cpp -I$JAVA_HOME/include/linux -I$JAVA_HOME/include -o libSmallProfiler.so -std=c++17 -shared -pthread

# run them together on mac
java -agentpath:cpp/libSmallProfiler.dylib=interval=0.001s -cp samples BasicSample
# on linux
java -agentpath:cpp/libSmallProfiler.so=interval=0.001s -cp samples BasicSample
```

Don't set the interval too low, or you'll crash your JVM.

License
-------
GPLv2 (like the OpenJDK)
