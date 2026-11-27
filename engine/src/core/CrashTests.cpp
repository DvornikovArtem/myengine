// CrashTests.cpp
// The crashes of the CRASH TEST buttons of the main menu of Rocket Arena (LR5, docs/crash/tester-guide.md). They are on purpose: the tester presses a
// button, the game crashes, the crash handler (CrashHandler.cpp) writes the report, and the developer finds the line below in the stack of the dump.
// Each crash is a function of its own that is never inlined (the stack names it); volatile keeps the optimizer from removing or folding the crash.

#include <myengine/core/CrashHandler.h>

namespace myengine::core::crash::test
{
    __declspec(noinline) void DivideByZero()
    {
        volatile int divisor = 0;
        volatile int dividend = 42;
        volatile int quotient = dividend / divisor;   // <- EXCEPTION_INT_DIVIDE_BY_ZERO 0xC0000094 (the idiv instruction of the CPU)
        (void)quotient;
    }

    __declspec(noinline) void AccessViolation()
    {
        volatile int* volatile nothing = nullptr;
#pragma warning(suppress : 6011)   // on purpose: the write through the null pointer
        *nothing = 42;   // <- EXCEPTION_ACCESS_VIOLATION 0xC0000005: writing the address 0
    }

    __declspec(noinline) void AssertionFailure()
    {
        volatile int lives = 0;
        MYENGINE_CHECK(lives > 0);   // <- assertion failure 0xE0000003 (MYENGINE_CHECK stays in a release build)
    }
}
