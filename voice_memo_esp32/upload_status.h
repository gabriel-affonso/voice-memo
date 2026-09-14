#pragma once

namespace voice_memo_firmware {

enum class UploadStatus {
    Accepted,
    AlreadyKnown,
    Failed,
};

UploadStatus classify_upload_status(int http_status);

}  // namespace voice_memo_firmware
