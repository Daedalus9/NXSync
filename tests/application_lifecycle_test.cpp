#include "nxsync/application_lifecycle.hpp"

#include <cassert>

int main() {
    nxsync::ApplicationLifecycleState state;
    assert(!nxsync::recordApplicationStart(state, 0, "0100123456789000"));
    assert(nxsync::recordApplicationStart(
        state, 42, "0100123456789000", true));
    assert(state.activeProcessId == 42);
    assert(state.lastEvent == "detected");
    assert(state.eventSequence == 1);
    assert(!nxsync::recordApplicationTermination(state, 99, false));
    assert(nxsync::recordApplicationTermination(state, 42, false));
    assert(state.activeProgramId.empty());
    assert(state.lastProgramId == "0100123456789000");
    assert(state.lastEvent == "exit");
    assert(state.eventSequence == 2);
    assert(nxsync::recordApplicationStart(state, 77, "0100ABCDEF123000"));
    assert(nxsync::recordApplicationTermination(state, 77, true));
    assert(state.lastEvent == "crash");
    assert(state.eventSequence == 4);

    assert(nxsync::recordApplicationStart(state, 88, "0100ABCDEF124000"));
    assert(!nxsync::reconcileMissingApplication(state, 88, false));
    assert(!nxsync::reconcileMissingApplication(state, 0, true));
    assert(nxsync::reconcileMissingApplication(state, 0, false));
    assert(state.lastProgramId == "0100ABCDEF124000");
    assert(state.lastEvent == "exit");
    assert(state.eventSequence == 6);

    assert(nxsync::recordApplicationStart(state, 99, "0100ABCDEF125000"));
    assert(nxsync::reconcileMissingApplication(state, 100, false));
    assert(state.lastProgramId == "0100ABCDEF125000");
    assert(state.lastEvent == "exit");
    assert(state.eventSequence == 8);
    return 0;
}
