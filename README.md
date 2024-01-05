Writing a Profiler from Scratch
===============================

This profiler implementation requires a [modified JDK](https://github.com/parttimenerd/jdk/tree/minimal_asgst).

How to run this all?
--------------------

```sh
# compile the small profiler agent
# on mac
g++ cpp/libSmallProfiler.cpp -I$JAVA_HOME/include/darwin -I$JAVA_HOME/include -o libSmallProfiler.so -std=c++17 -shared 
# on linux
g++ cpp/libSmallProfiler.cpp -I$JAVA_HOME/include/linux -I$JAVA_HOME/include -o libSmallProfiler.so -std=c++17 -shared
# or just
./build.sh

# run them together
java -agentpath:./libSmallProfiler.so=interval=0.001s samples/BasicSample.java
```

Usage:
------
```sh
Usage: -agentpath:libSmallProfiler.so=<options>
Options:
  interval=<time>  - sampling interval, default 1ms
  cpu              - sample CPU time instead of wall clock time
  output=<file>    - output file, default flames.html
  printTraces      - print stack traces to stderr
```

Don't set the interval too low, or you'll crash your JVM.

License
-------
GPLv2 (like the OpenJDK)