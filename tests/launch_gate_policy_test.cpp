#include "nxsync/launch_gate_policy.hpp"

#include <cassert>
#include <string>

int main() {
    using nxsync::LaunchReleaseDisposition;

    assert(nxsync::launchReleaseDisposition(false, 0)
        == LaunchReleaseDisposition::WaitingForResult);
    assert(nxsync::launchReleaseDisposition(false, 42)
        == LaunchReleaseDisposition::WaitingForResult);
    assert(nxsync::launchReleaseDisposition(true, 42)
        == LaunchReleaseDisposition::WaitingForWorkerExit);
    assert(nxsync::launchReleaseDisposition(true, 0)
        == LaunchReleaseDisposition::Ready);
    assert(nxsync::restoreAllowsLaunch(true, true, false));
    assert(nxsync::restoreAllowsLaunch(false, false, false));
    assert(nxsync::restoreAllowsLaunch(false, true, true));
    assert(!nxsync::restoreAllowsLaunch(false, true, false));
    const std::string revision(64, 'a'), payload(64, 'b'), other(64, 'c');
    assert(nxsync::restoreMatchesSelectedRevision(revision, payload, revision, payload));
    // A valid archive from the same title may still be the wrong selected save.
    assert(!nxsync::restoreMatchesSelectedRevision(revision, payload, other, payload));
    assert(!nxsync::restoreMatchesSelectedRevision(revision, payload, revision, other));
    assert(!nxsync::restoreMatchesSelectedRevision("", payload, revision, payload));
    assert(!nxsync::restoreMatchesSelectedRevision(revision, "", revision, payload));
    return 0;
}
