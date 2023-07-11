Modification
============
Requires https://github.com/parttimenerd/jdk/tree/asgst_iterator (younger than 11 July might not work).

Writing a Profiler from Scratch
===============================

This repository belongs to my article series with the same name. 
It is a step-by-step guide to writing a profiler from scratch.

This repository currently looks a bit barren, it contains just the example Java code and
the introductory agent code. More will come as the article series progresses.

How to run this all?
--------------------

```sh
# compile the Java sample code
javac samples/BasicSample.java

# compile the small profiler agent
# on mac
g++ cpp/libSmallProfiler.cpp -I$JAVA_HOME/include/darwin -I$JAVA_HOME/include -o libSmallProfiler.so -std=c++17 -shared 
# on linux
g++ cpp/libSmallProfiler.cpp -I$JAVA_HOME/include/linux -I$JAVA_HOME/include -o libSmallProfiler.so -std=c++17 -shared
# or just
./build.sh

# run them together
java -agentpath:libSmallProfiler.so=interval=0.001s -cp samples BasicSample
```

Don't set the interval too low, or you'll crash your JVM.

License
-------
GPLv2 (like the OpenJDK)