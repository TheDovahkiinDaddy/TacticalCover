// Test runner for the engine-free code in src/Core. Add a Run...Tests() per test file.

int RunInputStateTests();
int RunVerdictTests();
int RunGeometryTests();
int RunSensorTests();
int RunSenseTriggerTests();
int RunCoverTests();
int RunStateMachineTests();
int RunMovementTests();
int RunAutoCoverTests();
int RunTallVerbTests();
int RunReloadLearnTests();
int RunCameraSlotTests();
int RunEntryWaitTests();
int RunSwapChordTests();
int RunReleaseConfigTests();

int main()
{
	int failed = 0;
	failed += RunInputStateTests();
	failed += RunVerdictTests();
	failed += RunGeometryTests();
	failed += RunSensorTests();
	failed += RunSenseTriggerTests();
	failed += RunCoverTests();
	failed += RunStateMachineTests();
	failed += RunMovementTests();
	failed += RunAutoCoverTests();
	failed += RunTallVerbTests();
	failed += RunReloadLearnTests();
	failed += RunCameraSlotTests();
	failed += RunEntryWaitTests();
	failed += RunSwapChordTests();
	failed += RunReleaseConfigTests();
	return failed == 0 ? 0 : 1;
}
