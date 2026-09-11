#pragma once

// Shared by the test translation units; test_main.cpp owns the counters and main().
namespace testing {

void check(bool ok, const char* what);
int failures();
int checks();

}  // namespace testing

// One entry point per file, called in order from main().
void runGameTests();
void runHttpTests();
void runFlowTests();
