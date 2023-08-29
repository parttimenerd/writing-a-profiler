Modification
============
Requires https://github.com/parttimenerd/jdk/tree/asgst_iterator (younger than 3. August might not work).
Actually checks the correctness of the ASGST stack walking at safepoints.

Sample usage:

```
java -agentpath:libSmallProfiler.so=interval=0.001s -cp samples math.MathParser
```

Writing a Profiler from Scratch
===============================

This repository belongs to my article series with the same name. 
It is a step-by-step guide to writing a profiler from scratch.

This repository currently looks a bit barren, it contains just the example Java code and
the introductory agent code. More will come as the article series progresses.

How to run this all?
--------------------

```sh
# compile the Java sample code and the native agent
./build.sh

# run them together
java -agentpath:libSmallProfiler.so=interval=0.001s -cp samples BasicSample

# or for the native sample
java -agentpath:libSmallProfiler.so -cp samples -Djava.library.path=`pwd`/samples/nat nat.Native
```

Don't set the interval too low, or you'll crash your JVM.

License
-------
GPLv2 (like the OpenJDK)