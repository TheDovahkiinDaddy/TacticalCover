// Engine-free tests for the tall pose's end rule (T5.2, REFERENCE 22.17).

#include "Core/TallVerb.h"

#include <cstdio>

namespace
{
	int g_failures = 0;

#define CHECK(cond)                                                     \
	do {                                                                \
		if (!(cond)) {                                                  \
			std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			++g_failures;                                               \
		}                                                               \
	} while (false)

	using namespace Core;

	void TestRule()
	{
		TallEnds e;
		// nothing open: the verb does nothing, either side
		CHECK(DecideTallVerb(e, false, 0.95f) == TallVerb::None);
		CHECK(DecideTallVerb(e, true, 0.95f) == TallVerb::None);
		// the right end open and near: here when turned right, turn first when turned left
		e.rightKind = EdgeKind::Open;
		e.right = 0.73f;
		CHECK(DecideTallVerb(e, false, 0.95f) == TallVerb::Here);
		CHECK(DecideTallVerb(e, true, 0.95f) == TallVerb::Turn);
		// open but out of reach
		e.right = 1.2f;
		CHECK(DecideTallVerb(e, false, 0.95f) == TallVerb::None);
		CHECK(DecideTallVerb(e, true, 0.95f) == TallVerb::None);
		e.right = 0.95f;  // exactly at the range counts
		CHECK(DecideTallVerb(e, false, 0.95f) == TallVerb::Here);
		// a blocked end is no end
		e.rightKind = EdgeKind::Blocked;
		e.right = 0.2f;
		CHECK(DecideTallVerb(e, false, 0.95f) == TallVerb::None);
		// both open and near: the actor's own side wins
		e.rightKind = EdgeKind::Open;
		e.right = 0.5f;
		e.leftKind = EdgeKind::Open;
		e.left = 0.4f;
		CHECK(DecideTallVerb(e, false, 0.95f) == TallVerb::Here);
		CHECK(DecideTallVerb(e, true, 0.95f) == TallVerb::Here);
		// the own side far, the other near: turn
		e.left = 3.0f;
		CHECK(DecideTallVerb(e, true, 0.95f) == TallVerb::Turn);
		CHECK(DecideTallVerb(e, false, 0.95f) == TallVerb::Here);
	}

	void TestAutoTurn()
	{
		TallEnds e;
		e.rightKind = EdgeKind::Open;
		e.right = 0.8f;
		e.leftKind = EdgeKind::Blocked;
		e.left = 1.4f;
		// only the other end is open and in range: turn to it
		CHECK(DecideAutoTurn(e, true, 0.95f, 0.25f));
		CHECK(!DecideAutoTurn(e, false, 0.95f, 0.25f));
		// the other end out of range: nothing
		e.right = 1.3f;
		CHECK(!DecideAutoTurn(e, true, 0.95f, 0.25f));
		// both open and in range: the nearer by more than the margin wins, a draw stays
		e.rightKind = EdgeKind::Open;
		e.leftKind = EdgeKind::Open;
		e.right = 0.4f;
		e.left = 0.8f;
		CHECK(DecideAutoTurn(e, true, 0.95f, 0.25f));    // on the left side, the right end is 0.4 nearer
		CHECK(!DecideAutoTurn(e, false, 0.95f, 0.25f));  // on the right side already
		e.left = 0.55f;  // 0.15 nearer only: no turn back and forth
		CHECK(!DecideAutoTurn(e, true, 0.95f, 0.25f));
		CHECK(!DecideAutoTurn(e, false, 0.95f, 0.25f));
		// a blocked end is no end
		e.leftKind = EdgeKind::Blocked;
		e.right = 0.9f;
		CHECK(DecideAutoTurn(e, true, 0.95f, 0.25f));
		e.rightKind = EdgeKind::Blocked;
		CHECK(!DecideAutoTurn(e, true, 0.95f, 0.25f));
	}
}

int RunTallVerbTests()
{
	g_failures = 0;
	TestRule();
	TestAutoTurn();
	std::printf("tall verb tests: %s\n", g_failures == 0 ? "ok" : "FAILED");
	return g_failures;
}
