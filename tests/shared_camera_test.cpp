// Host-side tests for players::SharedCamera, run without the game.
//
// The two PlayerController fields are plain pointers here, and the "controllers" are distinct
// objects of a test-only UObject. The cases are the lifecycle events the loan must survive:
// player 1's controller changing, the engine emptying player 2's field, the engine giving player 2
// a controller of its own, and removal while the loan is a tick behind.

#include "players/shared_camera.h"

#include <cstdio>

namespace RC::Unreal {
class UObject  // the engine's type is only forward-declared by the mod; the tests need objects
{
};
}  // namespace RC::Unreal

namespace {

using lss::players::SharedCamera;
using RC::Unreal::UObject;

int g_failures = 0;

#define EXPECT(condition)                                                                \
    do                                                                                   \
    {                                                                                    \
        if (!(condition))                                                                \
        {                                                                                \
            std::printf("  FAILED line %d: %s\n", __LINE__, #condition);                 \
            ++g_failures;                                                                \
        }                                                                                \
    } while (false)

struct Players
{
    UObject  controllerA, controllerB, playerTwosOwn;
    UObject* one = &controllerA;  // player 1's PlayerController field
    UObject* two = nullptr;       // player 2's PlayerController field
};

void TestAttachLendsPlayerOnesController()
{
    std::puts("attach: player 2's empty field gets player 1's controller");
    Players      p;
    SharedCamera camera;
    EXPECT(camera.Attach(&p.one, &p.two));
    EXPECT(p.two == &p.controllerA);
}

void TestAttachRefusesWhatItMustNotTouch()
{
    std::puts("attach: refuses an occupied field, a missing controller, or a second attach");
    {
        Players      p;
        SharedCamera camera;
        p.two = &p.playerTwosOwn;
        EXPECT(!camera.Attach(&p.one, &p.two));
        EXPECT(p.two == &p.playerTwosOwn);
    }
    {
        Players      p;
        SharedCamera camera;
        p.one = nullptr;
        EXPECT(!camera.Attach(&p.one, &p.two));
        EXPECT(p.two == nullptr);
    }
    {
        Players      p;
        SharedCamera camera;
        EXPECT(camera.Attach(&p.one, &p.two));
        EXPECT(!camera.Attach(&p.one, &p.two));
    }
}

void TestRefreshFollowsPlayerOnesNewController()
{
    std::puts("refresh: follows player 1 to a new controller");
    Players      p;
    SharedCamera camera;
    camera.Attach(&p.one, &p.two);
    EXPECT(!camera.Refresh());  // nothing changed
    p.one = &p.controllerB;
    EXPECT(camera.Refresh());
    EXPECT(p.two == &p.controllerB);
}

void TestRefreshRestoresAnEmptiedField()
{
    std::puts("refresh: restores the loan if the engine empties player 2's field");
    Players      p;
    SharedCamera camera;
    camera.Attach(&p.one, &p.two);
    p.two = nullptr;
    EXPECT(camera.Refresh());
    EXPECT(p.two == &p.controllerA);
}

void TestRefreshLeavesPlayerTwosOwnController()
{
    std::puts("refresh: never replaces a controller the engine gave player 2");
    Players      p;
    SharedCamera camera;
    camera.Attach(&p.one, &p.two);
    p.two = &p.playerTwosOwn;
    EXPECT(!camera.Refresh());
    EXPECT(p.two == &p.playerTwosOwn);
}

void TestDetachNeverLeavesPlayerOnesController()
{
    std::puts("detach: player 2's field never keeps player 1's controller");
    {
        Players      p;  // the ordinary case
        SharedCamera camera;
        camera.Attach(&p.one, &p.two);
        camera.Detach();
        EXPECT(p.two == nullptr);
        EXPECT(!camera.IsAttached());
    }
    {
        Players      p;  // player 1's controller changed and the loan is a tick behind
        SharedCamera camera;
        camera.Attach(&p.one, &p.two);
        p.one = &p.controllerB;
        camera.Detach();
        EXPECT(p.two == nullptr);
    }
    {
        Players      p;  // something wrote player 1's new controller into player 2's field
        SharedCamera camera;
        camera.Attach(&p.one, &p.two);
        p.one = &p.controllerB;
        p.two = &p.controllerB;
        camera.Detach();
        EXPECT(p.two == nullptr);
    }
    {
        Players      p;  // player 2 has a controller of its own: removal may destroy it
        SharedCamera camera;
        camera.Attach(&p.one, &p.two);
        p.two = &p.playerTwosOwn;
        camera.Detach();
        EXPECT(p.two == &p.playerTwosOwn);
    }
}

}  // namespace

int main()
{
    TestAttachLendsPlayerOnesController();
    TestAttachRefusesWhatItMustNotTouch();
    TestRefreshFollowsPlayerOnesNewController();
    TestRefreshRestoresAnEmptiedField();
    TestRefreshLeavesPlayerTwosOwnController();
    TestDetachNeverLeavesPlayerOnesController();

    std::printf(g_failures == 0 ? "all tests passed\n" : "%d check(s) failed\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
