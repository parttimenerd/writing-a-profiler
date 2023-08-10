#include <fstream>
#include <map>
#include <string>
#include <iostream>
#include <memory>

class Node {
  std::string method;
  std::map<std::string, std::unique_ptr<Node>> children;
  long samples = 0;

public:
  Node(std::string method): method(method) {}

  Node& getChild(std::string method) {
    if (children.find(method) == children.end()) {
      children[method] = std::make_unique<Node>(method);
    }
    return *children[method];
  }
  void addTrace(std::vector<std::string> trace, int end) {
    samples++;
    if (end > 0) {
      getChild(trace.at(end)).addTrace(trace, end - 1);
    }
  }

  void addTrace(std::vector<std::string> trace) {
    addTrace(trace, trace.size() - 1);
  }

  /**
    * Write in d3-flamegraph format
    */
  void writeAsJson(std::ofstream &s, int maxDepth, int minValue = 0) {
      s << "{ \"name\": \"" << method << "\", \"value\": " << samples << ", \"children\": [";
      if (maxDepth > 1) {
          for (auto& [m, child] : children) {
            if (child->samples >= minValue) {
              child->writeAsJson(s, maxDepth - 1);
              s << ",";
            }
          }
      }
      s << "]}";
  }

  void writeAsHTML(std::ofstream &s, int maxDepth, int minValue = 0) {
    s << R"B(
            <head>
              <link rel="stylesheet" type="text/css" href="https://cdn.jsdelivr.net/npm/d3-flame-graph@4.1.3/dist/d3-flamegraph.css">
            </head>
            <body>
              <div id="chart"></div>
              <script type="text/javascript" src="https://d3js.org/d3.v7.js"></script>
              <script type="text/javascript" src="https://cdn.jsdelivr.net/npm/d3-flame-graph@4.1.3/dist/d3-flamegraph.min.js"></script>
              <script type="text/javascript">
              var chart = flamegraph().width(window.innerWidth);
              d3.select("#chart").datum(
              )B";
    writeAsJson(s, maxDepth, minValue);
    s << R"B(
            ).call(chart);
              window.onresize = () => chart.width(window.innerWidth);
              </script>
            </body>
            )B";
  }
};