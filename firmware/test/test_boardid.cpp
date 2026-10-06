// test_boardid.cpp — host tests for control/BoardId.h: the ONE rule for "which board is this id".
#include "../control/BoardId.h"
#include <cstdio>
using namespace topo;
static int passed = 0, failed = 0;
static void ok(const char* what, bool c) { if (c) { printf("  ok   %s\n", what); passed++; } else { printf("  FAIL %s\n", what); failed++; } }

int main() {
  printf("\nB1 one canonical spelling\n");
  ok("a bare name stays", bareHost("dustgate-node-1") == "dustgate-node-1");
  ok(".local comes off", bareHost("dustgate-node-1.local") == "dustgate-node-1");
  ok("case is folded", bareHost("DustGate-Node-1.LOCAL") == "dustgate-node-1");
  ok("a trailing dot goes first", bareHost("dustgate-node-1.local.") == "dustgate-node-1");
  ok("null is empty", bareHost(nullptr).empty());
  ok(".local alone is left as it is", bareHost(".local") == ".local");

  printf("\nB2 is this controllerId THIS board?\n");
  ok("absent means this board", isOwnBoard("", "primary"));
  ok("its own id is this board", isOwnBoard("primary", "primary"));
  ok("in any spelling", isOwnBoard("Primary.local", "primary"));
  ok("another board is not", !isOwnBoard("node-1", "primary"));
  ok("a board that does not know its own id claims no NAMED board", !isOwnBoard("node-1", ""));
  ok("...but absent is still this board", isOwnBoard("", ""));

  printf("\nB3 do two ids name the same board?\n");
  ok("one board in two spellings", sameBoard("node-1", "NODE-1.local", "primary"));
  ok("two different boards", !sameBoard("node-1", "node-2", "primary"));
  ok("absent and the board's own id are the same board", sameBoard("", "primary", "primary"));
  ok("absent and a node are not", !sameBoard("", "node-1", "primary"));
  ok("two absents", sameBoard("", "", "primary"));

  printf("\n%d/%d passed%s\n", passed, passed + failed, failed ? " — FAILED" : "");
  return failed ? 1 : 0;
}
