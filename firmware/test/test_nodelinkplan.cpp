// test_nodelinkplan.cpp — host tests for control/NodeLinkPlan.h.
#include "../control/NodeLinkPlan.h"
#include <cstdio>
using namespace nodelinks;
using V = std::vector<std::string>;
static int passed = 0, failed = 0;
static void ok(const char* what, bool c) { if (c) { printf("  ok   %s\n", what); passed++; } else { printf("  FAIL %s\n", what); failed++; } }

int main() {
  printf("\nN1 adding a node leaves the others alone\n");
  {
    Plan p = plan({"a", "b"}, {"a", "b", "c"}, "");
    ok("nothing stopped", p.stop.empty());
    ok("only the new one dials", p.start == V{"c"});
    ok("the existing links are kept", p.keep == V({"a", "b"}));
  }
  printf("\nN2 removing a node stops only that one\n");
  {
    Plan p = plan({"a", "b", "c"}, {"a", "c"}, "");
    ok("b stops", p.stop == V{"b"});
    ok("nothing new dials", p.start.empty());
    ok("a and c are kept", p.keep == V({"a", "c"}));
  }
  printf("\nN3 a takeover restarts that link and no other\n");
  {
    Plan p = plan({"a", "b"}, {"a", "b"}, "b");
    ok("b stops", p.stop == V{"b"});
    ok("and dials again", p.start == V{"b"});
    ok("a is untouched", p.keep == V{"a"});
  }
  printf("\nN4 nothing changed, nothing happens\n");
  {
    Plan p = plan({"a", "b"}, {"a", "b"}, "");
    ok("no stops, no starts", p.stop.empty() && p.start.empty() && p.keep.size() == 2);
  }
  printf("\nN5 boot: nothing live, everything dials\n");
  {
    Plan p = plan({}, {"a", "b"}, "");
    ok("both dial", p.start == V({"a", "b"}) && p.stop.empty() && p.keep.empty());
  }
  printf("\nN6 odd input\n");
  {
    Plan p = plan({"a"}, {"a", "", "b", "b"}, "");
    ok("an empty host never dials, a duplicate dials once", p.start == V{"b"});
    Plan q = plan({"a"}, {}, "");
    ok("an emptied registry stops everything", q.stop == V{"a"} && q.start.empty());
  }
  printf("\n%d passed, %d failed\n", passed, failed);
  return failed ? 1 : 0;
}
