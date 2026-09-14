#include "upload_status.h"

namespace voice_memo_firmware {

UploadStatus classify_upload_status(int http_status) {
    if (http_status == 201) {
        return UploadStatus::Accepted;
    }
    if (http_status == 200) {
        return UploadStatus::AlreadyKnown;
    }
    return UploadStatus::Failed;
}

}  // namespace voice_memo_firmware
