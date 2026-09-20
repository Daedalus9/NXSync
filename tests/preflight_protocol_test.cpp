#include "nxsync/preflight_protocol.hpp"

#include <cassert>
#include <cstdio>
#include <string>

int main() {
    nxsync::PreflightRequest request;
    request.sequence = 42;
    request.titleId = "01008F6008C5E000";
    request.profileUid = "00112233445566778899AABBCCDDEEFF";
    std::string error;
    nxsync::PreflightRequest parsedRequest;
    assert(nxsync::parsePreflightRequest(
        nxsync::serializePreflightRequest(request), parsedRequest, error));
    assert(parsedRequest.sequence == request.sequence);
    assert(parsedRequest.titleId == request.titleId);
    assert(parsedRequest.profileUid == request.profileUid);

    nxsync::PreflightStatus status;
    status.workerBuildVersion = "0.3.0-dev";
    status.sequence = request.sequence;
    status.titleId = request.titleId;
    status.profileUid = request.profileUid;
    status.state = "completed";
    status.outcome = "cloud-update-available";
    status.message = "Newer cloud revision = available\\test";
    status.validHeads = 2;
    status.invalidHeads = 1;
    status.selectedDeviceId = "NS-OLED-AABBCC";
    status.selectedProfileName = "PlayerOne=Cloud";
    status.selectedRevisionId = std::string(64, 'a');
    status.selectedArchivePath = "/NXSync/source/save.zip";
    status.selectedGameVersion = "4.0.0";
    status.selectedCreatedUtc = "2026-08-14T01:00:00Z";
    status.selectedPayloadSha256 = std::string(64, 'b');
    status.localRevisionId = std::string(64, 'c');
    status.localPayloadSha256 = std::string(64, 'd');
    status.candidates.push_back(nxsync::PreflightCandidate{
        "NS-OLED-AABBCC",
        "PlayerOne=Cloud",
        std::string(64, 'a'),
        std::string(64, 'b'),
        "/NXSync/source/save.zip",
        "4.0.0",
        "2026-08-14T01:00:00Z"});
    nxsync::PreflightStatus parsedStatus;
    assert(nxsync::parsePreflightStatus(
        nxsync::serializePreflightStatus(status), parsedStatus, error));
    assert(parsedStatus.message == status.message);
    assert(parsedStatus.selectedProfileName == status.selectedProfileName);
    assert(parsedStatus.validHeads == 2);
    assert(parsedStatus.candidates.size() == 1);
    assert(parsedStatus.candidates[0].revisionId == std::string(64, 'a'));
    assert(parsedStatus.candidates[0].payloadSha256 == std::string(64, 'b'));
    assert(parsedStatus.selectedPayloadSha256 == status.selectedPayloadSha256);
    assert(parsedStatus.localRevisionId == status.localRevisionId);

    const std::string path = "preflight-protocol-test.request";
    int systemError = 0;
    assert(nxsync::writePreflightRequestAtomic(path, request, systemError));
    nxsync::PreflightRequest loaded;
    assert(nxsync::loadPreflightRequest(path, loaded, error));
    assert(!nxsync::completePreflightRequest(path, 99, systemError));
    assert(nxsync::completePreflightRequest(path, 42, systemError));
    std::remove(path.c_str());
    std::remove((path + ".new").c_str());
    return 0;
}
