#pragma once

// The persistent layout of the microSD card, in one place.
//
//   <VM_SD_ROOT>/                 default "/voice_memo"
//     pending/                    committed recordings waiting for upload
//       <recording_id>.wav        the audio, complete and validated
//       <recording_id>.json       the metadata sidecar (commit marker)
//     uploading/                  an attempt is in flight (crash marker)
//       <recording_id>.wav
//       <recording_id>.json
//     tmp/                        never a valid recording
//       <recording_id>.wav.tmp
//       <recording_id>.json.tmp
//     corrupt/                    quarantined: metadata or audio failed checks
//     sent/                       optional retention (VM_SD_KEEP_SENT = 1)
//
// The commit protocol is: write both .tmp files, rename the metadata first and
// the WAV second. The WAV rename is the single operation that turns a
// recording into a committed one, and recovery can complete an interrupted
// commit because the metadata sidecar is already in place (see
// recording_store.cpp).
//
// Arduino free so the path rules are host-tested. Nothing outside
// recording_store.{h,cpp} should build these paths.

#include <string>

#include "config.h"
#include "identifier_utils.h"

namespace voice_memo_firmware {
namespace store_paths {

// Directory names, not paths. `constexpr` (not `inline`) keeps this valid in
// C++11, which the host tests are compiled with.
constexpr const char* kPendingDirName = "pending";
constexpr const char* kUploadingDirName = "uploading";
constexpr const char* kTmpDirName = "tmp";
constexpr const char* kCorruptDirName = "corrupt";
constexpr const char* kSentDirName = "sent";

inline std::string root() {
    return std::string(VM_SD_ROOT);
}

inline std::string directory(const char* name) {
    return root() + "/" + name;
}

inline std::string join(const std::string& dir, const std::string& name) {
    return dir + "/" + name;
}

inline std::string wavIn(const char* dirName, const std::string& recordingId) {
    return join(directory(dirName), recordingId + ".wav");
}

inline std::string metaIn(const char* dirName, const std::string& recordingId) {
    return join(directory(dirName), recordingId + ".json");
}

inline std::string tmpWav(const std::string& recordingId) {
    return join(directory(kTmpDirName), recordingId + ".wav.tmp");
}

inline std::string tmpMeta(const std::string& recordingId) {
    return join(directory(kTmpDirName), recordingId + ".json.tmp");
}

// A recording id becomes a file name, so it is validated before any path is
// built. is_valid_recording_id() accepts only [A-Za-z0-9][A-Za-z0-9._-]* with a
// bounded length, which rules out separators and traversal sequences.
inline bool isSafeRecordingId(const std::string& recordingId) {
    return is_valid_recording_id(recordingId.c_str());
}

// "<id>.wav" -> "<id>"; returns false when the suffix is not present or the id
// is not safe. Used by boot recovery while scanning a directory.
inline bool idFromFileName(const std::string& name, const char* suffix, std::string* outId) {
    if (outId == nullptr || suffix == nullptr) {
        return false;
    }
    const std::string suffixString(suffix);
    if (name.size() <= suffixString.size()) {
        return false;
    }
    if (name.compare(name.size() - suffixString.size(), suffixString.size(), suffixString) != 0) {
        return false;
    }
    const std::string id = name.substr(0, name.size() - suffixString.size());
    if (!isSafeRecordingId(id)) {
        return false;
    }
    *outId = id;
    return true;
}

}  // namespace store_paths
}  // namespace voice_memo_firmware
