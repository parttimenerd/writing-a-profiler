#!/bin/sh

cd $(dirname "$0")
if [ -d "$JAVA_HOME/include/linux" ]; then
  g++ cpp/libSmallProfiler.cpp -I$JAVA_HOME/include/linux -I$JAVA_HOME/include -o libSmallProfiler.so -std=c++17 -shared -fPIC -g
else
  g++ -O0 cpp/libSmallProfiler.cpp -I$JAVA_HOME/include/darwin -I$JAVA_HOME/include -o libSmallProfiler.so -std=c++17 -shared -fPIC -g -L$JAVA_HOME/lib/server -ljvm
fi

(
  cd samples
  [ BasicSample.java -nt BasicSample.class ] && javac BasicSample.java
  [ math/MathParser.java -nt math/MathParser.class ] && javac math/MathParser.java
  ([ nat/Native.java -nt nat/Native.class ] || [ nat/Native.cpp -nt native/Native.class ] || [ -f nat/libnative.so ]) && (
    cd nat
    rm -f nat_Native.h
    javac -h . Native.java && (
      if [ -d "$JAVA_HOME/include/linux" ]; then
        g++ Native.cpp -I$JAVA_HOME/include/linux -I$JAVA_HOME/include -o libnative.so -fPIC -shared
      else 
        g++ Native.cpp -I$JAVA_HOME/include/darwin -I$JAVA_HOME/include -o libnative.dylib -fPIC -shared
      fi
    )
  )
)
