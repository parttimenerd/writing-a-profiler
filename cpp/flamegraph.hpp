#include <fstream>
#include <map>
#include <string>
#include <iostream>
#include <memory>
#include <vector>
#include <mutex>

class Node {
  std::string method;
  std::map<std::string, std::unique_ptr<Node>> children;
  long samples = 0;
  std::mutex m;

  Node& getChild(std::string method) {
    if (children.find(method) == children.end()) {
      children[method] = std::make_unique<Node>(method);
    }
    return *children[method];
  }
  void addTrace(std::vector<std::string> trace, int end, int encounters) {
    samples = samples + encounters;
    if (end > 0) {
      getChild(trace.at(end)).addTrace(trace, end - 1, encounters);
    }
  }

public:

  Node(std::string method): method(method) {
  }

  void addTrace(std::vector<std::string> trace, size_t encounters = 1) {
    std::lock_guard<std::mutex> lock(m);
    addTrace(trace, trace.size() - 1, encounters);
  }

  /**
    * Write in d3-flamegraph format
    */
  void writeAsJson(std::ofstream &s, int maxDepth) {
      s << "{ \"name\": \"" << method << "\", \"value\": " << samples << ", \"children\": [";
      if (maxDepth > 1) {
          // sort children by samples
          std::vector<std::pair<long, Node*>> sortedChildren;
          for (auto& [m, child] : children) {
              sortedChildren.push_back({child->samples, child.get()});
          }
          std::sort(sortedChildren.begin(), sortedChildren.end(), [](auto& a, auto& b) {
              return a.first > b.first;
          });
          for (auto& [c, child] : sortedChildren) {
              child->writeAsJson(s, maxDepth - 1);
              s << ",";
          }
      }
      s << "]}";
  }

  void writeAsHTML(std::ofstream &s, int maxDepth) {
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
    writeAsJson(s, maxDepth);
    s << R"B(
            ).call(chart);
              window.onresize = () => chart.width(window.innerWidth);
              </script>
            </body>
            )B";
  }
};