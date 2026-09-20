#include "nxsync/cloud_worker_status.hpp"

#include <cassert>
#include <string>

int main() {
    nxsync::CloudWorkerStatus status;
    status.buildVersion = "0.1.0-dev";
    status.state = "upload-working";
    status.revisionId = std::string(64, 'a');
    status.message = "Upload = 1\nverifica";
    status.completed = 2;
    status.failed = 1;
    status.bytesTransferred = 1024;
    status.totalBytes = 4096;

    const std::string text = nxsync::serializeCloudWorkerStatus(status);
    nxsync::CloudWorkerStatus parsed;
    std::string error;
    assert(nxsync::parseCloudWorkerStatus(text, parsed, error));
    assert(parsed.buildVersion == status.buildVersion);
    assert(parsed.state == status.state);
    assert(parsed.revisionId == status.revisionId);
    assert(parsed.message == status.message);
    assert(parsed.completed == status.completed);
    assert(parsed.failed == status.failed);
    assert(parsed.bytesTransferred == status.bytesTransferred);
    assert(parsed.totalBytes == status.totalBytes);

    std::string invalid = text;
    invalid.replace(invalid.find("version=1"), 9, "version=2");
    assert(!nxsync::parseCloudWorkerStatus(invalid, parsed, error));
    return 0;
}
