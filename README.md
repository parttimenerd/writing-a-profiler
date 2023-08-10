Writing a Profiler from Scratch
===============================

This repository belongs to my blog post [Taming the Bias: Unbiased Safepoint-Based Stack Walking](https://mostlynerdless.de/blog/2023/08/05/taming-the-bias-unbiased-safepoint-based-stack-walking). The profiler requires [a modified OpenJDK](https://github.com/parttimenerd/jdk/tree/asgst_iterator).

How to run this all?
--------------------

```sh
# compile all
./build.sh

# run them together
java -agentpath:libSmallProfiler.so=interval=0.001s -cp samples BasicSample
```

```
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