#!/bin/sh

cd $(dirname "$0")
if [ -d "$JAVA_HOME/include/linux" ]; then
  g++ cpp/libSmallProfiler.cpp -I$JAVA_HOME/include/linux -I$JAVA_HOME/include -o libSmallProfiler.so -std=c++17 -shared
else
  g++ cpp/libSmallProfiler.cpp -I$JAVA_HOME/include/darwin -I$JAVA_HOME/include -o libSmallProfiler.so -std=c++17 -shared
fi
