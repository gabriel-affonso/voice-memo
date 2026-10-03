// Host-side tests for the persistent microSD recording store.
//
// Everything under test is Arduino free and runs against an in-memory volume
// (tests/memory_file_system.h), so the exact on-card rules the firmware relies
// on are verified without a board:
//
//     c++ -std=c++11 -Wall -Wextra -I. -o /tmp/recording_store_test \
//         tests/recording_store_test.cpp recording_store.cpp recording_meta.cpp \
//         record_crc32.cpp wav_format.cpp identifier_utils.cpp
//     /tmp/recording_store_test
//
// Covered: path construction, the metadata schema, CRC-32, WAV structural
// validation, the atomic tmp -> pending commit, boot recovery (tmp leftovers,
// interrupted uploads, orphan sidecars, WAVs without sidecars, corrupt audio),
// the upload lifecycle, oldest-first ordering, backpressure and the "a
// confirmed upload deletes only its own recording" rule.

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "../identifier_utils.h"
#include "../record_crc32.h"
#include "../recording_meta.h"
#include "../recording_paths.h"
#include "../recording_store.h"
#include "../vm_fs.h"
#include "../wav_format.h"
#include "memory_file_system.h"

namespace {

using voice_memo_firmware::FileHandle;
using voice_memo_firmware::RecordingMeta;
using voice_memo_firmware::RecordingStore;
using voice_memo_firmware::SaveOutcome;
using voice_memo_firmware::store_paths::kPendingDirName;
using voice_memo_firmware::store_paths::kUploadingDirName;
using voice_memo_firmware::store_paths::kTmpDirName;

int g_failures = 0;
int g_checks = 0;

void check(const char* label, bool ok) {
    ++g_checks;
    if (ok) {
        std::printf("PASS %s\n", label);
        return;
    }
    std::printf("FAIL %s\n", label);
    ++g_failures;
}

void checkStr(const char* label, const std::string& actual, const std::string& expected) {
    ++g_checks;
    if (actual == expected) {
        std::printf("PASS %s = %s\n", label, actual.c_str());
        return;
    }
    std::printf("FAIL %s: expected '%s', got '%s'\n", label, expected.c_str(), actual.c_str());
    ++g_failures;
}

void checkU32(const char* label, uint32_t actual, uint32_t expected) {
    ++g_checks;
    if (actual == expected) {
        std::printf("PASS %s = %lu\n", label, static_cast<unsigned long>(actual));
        return;
    }
    std::printf("FAIL %s: expected %lu, got %lu\n",
                label,
                static_cast<unsigned long>(expected),
                static_cast<unsigned long>(actual));
    ++g_failures;
}

void section(const char* title) {
    std::printf("\n--- %s\n", title);
}

// A structurally valid 16 kHz mono 16-bit WAV with deterministic content.
std::vector<uint8_t> makeWav(size_t payloadBytes, uint8_t seed) {
    std::vector<uint8_t> wav(44 + payloadBytes, 0);
    voice_memo_firmware::write_wav_header(
        wav.data(), 16000, 1, 16, static_cast<uint32_t>(payloadBytes));
    for (size_t i = 0; i < payloadBytes; ++i) {
        wav[44 + i] = static_cast<uint8_t>(seed + (i & 0x3FU));
    }
    return wav;
}

RecordingMeta metaFor(const std::string& id, uint32_t tagIndex, uint32_t durationMs) {
    RecordingMeta meta;
    meta.recording_id = id;
    meta.device_id = "bel-esp32-test";
    meta.tag_index = tagIndex;
    meta.duration_ms = durationMs;
    meta.sample_rate = 16000;
    meta.created_utc = 0;
    return meta;
}

std::vector<uint8_t> readAll(RecordingStore& store, const std::string& id, bool* ok) {
    std::vector<uint8_t> out;
    FileHandle* handle = store.openRead(id);
    if (handle == nullptr) {
        *ok = false;
        return out;
    }
    uint8_t chunk[512];
    for (;;) {
        const size_t got = handle->read(chunk, sizeof(chunk));
        if (got == 0) {
            break;
        }
        out.insert(out.end(), chunk, chunk + got);
    }
    *ok = true;
    handle->close();
    delete handle;
    return out;
}

std::string pendingWav(const std::string& id) {
    return voice_memo_firmware::store_paths::wavIn(kPendingDirName, id);
}

std::string uploadingWav(const std::string& id) {
    return voice_memo_firmware::store_paths::wavIn(kUploadingDirName, id);
}

std::string tmpWavPath(const std::string& id) {
    return voice_memo_firmware::store_paths::tmpWav(id);
}

std::string pendingMeta(const std::string& id) {
    return voice_memo_firmware::store_paths::metaIn(kPendingDirName, id);
}

}  // namespace

int main() {
    // ---------------------------------------------------------------------
    section("1. path construction");
    checkStr("root", voice_memo_firmware::store_paths::root(), "/voice_memo");
    checkStr("pending wav",
             voice_memo_firmware::store_paths::wavIn(kPendingDirName, "rec-1"),
             "/voice_memo/pending/rec-1.wav");
    checkStr("uploading json",
             voice_memo_firmware::store_paths::metaIn(kUploadingDirName, "rec-1"),
             "/voice_memo/uploading/rec-1.json");
    checkStr("tmp wav",
             voice_memo_firmware::store_paths::tmpWav("rec-1"),
             "/voice_memo/tmp/rec-1.wav.tmp");
    checkStr("tmp meta",
             voice_memo_firmware::store_paths::tmpMeta("rec-1"),
             "/voice_memo/tmp/rec-1.json.tmp");
    {
        std::string id;
        check("id from file name",
              voice_memo_firmware::store_paths::idFromFileName("rec-AB12.wav", ".wav", &id) &&
                  id == "rec-AB12");
        id.clear();
        check("traversal is rejected",
              !voice_memo_firmware::store_paths::idFromFileName("../evil.wav", ".wav", &id));
        id.clear();
        check("separator is rejected",
              !voice_memo_firmware::store_paths::idFromFileName("a/b.wav", ".wav", &id));
        id.clear();
        check("suffix-only is rejected",
              !voice_memo_firmware::store_paths::idFromFileName(".wav", ".wav", &id));
        id.clear();
        check("wrong suffix is rejected",
              !voice_memo_firmware::store_paths::idFromFileName("rec-1.json", ".wav", &id));
        check("valid recording id", voice_memo_firmware::is_valid_recording_id("rec-00000001-0000ABCD"));
        check("empty recording id rejected", !voice_memo_firmware::is_valid_recording_id(""));
        check("dotted recording id rejected", !voice_memo_firmware::is_valid_recording_id(".."));
    }

    // ---------------------------------------------------------------------
    section("2. CRC-32 (IEEE)");
    {
        const uint8_t text[] = "123456789";
        checkU32("known vector", voice_memo_firmware::crc32_ieee(text, 9), 0xCBF43926UL);
        const uint32_t first = voice_memo_firmware::crc32_ieee(text, 4);
        const uint32_t chained = voice_memo_firmware::crc32_ieee(text + 4, 5, first);
        checkU32("chained equals one-shot", chained, 0xCBF43926UL);
        checkU32("empty", voice_memo_firmware::crc32_ieee(text, 0), 0U);
    }

    // ---------------------------------------------------------------------
    section("3. WAV structural validation");
    {
        const std::vector<uint8_t> wav = makeWav(3200, 0x10);
        uint32_t dataBytes = 0;
        check("valid wav accepted",
              voice_memo_firmware::validate_wav_structure(
                  wav.data(), wav.size(), 16000, 1, 16, &dataBytes));
        checkU32("data bytes", dataBytes, 3200);
        check("truncated wav rejected",
              !voice_memo_firmware::validate_wav_structure(
                  wav.data(), wav.size() - 2, 16000, 1, 16, nullptr));
        check("short buffer rejected",
              !voice_memo_firmware::validate_wav_structure(wav.data(), 33, 16000, 1, 16, nullptr));
        check("wrong sample rate rejected",
              !voice_memo_firmware::validate_wav_structure(
                  wav.data(), wav.size(), 8000, 1, 16, nullptr));
        std::vector<uint8_t> stereo = makeWav(3200, 0x10);
        voice_memo_firmware::write_wav_header(stereo.data(), 16000, 2, 16, 3200);
        check("stereo rejected",
              !voice_memo_firmware::validate_wav_structure(
                  stereo.data(), stereo.size(), 16000, 1, 16, nullptr));
    }

    // ---------------------------------------------------------------------
    section("4. metadata schema");
    {
        RecordingMeta meta = metaFor("rec-00000001-0000ABCD", 2, 1500);
        meta.wav_bytes = 3200;
        meta.crc32 = 0xDEADBEEFUL;
        meta.has_crc = true;
        meta.attempt_count = 3;
        meta.sequence = 7;
        meta.created_utc = 1768478400UL;

        const std::string json = voice_memo_firmware::recording_meta_to_json(meta);
        RecordingMeta parsed;
        check("round trip parses", voice_memo_firmware::recording_meta_from_json(json, parsed));
        checkStr("id preserved", parsed.recording_id, meta.recording_id);
        checkStr("device preserved", parsed.device_id, meta.device_id);
        checkU32("tag preserved", parsed.tag_index, 2);
        checkU32("duration preserved", parsed.duration_ms, 1500);
        checkU32("crc preserved", parsed.crc32, 0xDEADBEEFUL);
        check("has_crc preserved", parsed.has_crc);
        checkU32("attempt preserved", parsed.attempt_count, 3);
        checkU32("sequence preserved", parsed.sequence, 7);
        checkU32("created preserved", parsed.created_utc, 1768478400UL);

        RecordingMeta ignored;
        check("truncated json rejected",
              !voice_memo_firmware::recording_meta_from_json("{\"recording_id\": \"x\"", ignored));
        check("missing recording_id rejected",
              !voice_memo_firmware::recording_meta_from_json("{\"wav_bytes\": 100}", ignored));
        check("missing wav_bytes rejected",
              !voice_memo_firmware::recording_meta_from_json("{\"recording_id\": \"rec-1\"}", ignored));
        check("nested value rejected",
              !voice_memo_firmware::recording_meta_from_json(
                  "{\"recording_id\": {\"a\":1}, \"wav_bytes\": 4}", ignored));
        check("wrong value type rejected",
              !voice_memo_firmware::recording_meta_from_json(
                  "{\"recording_id\": 5, \"wav_bytes\": 4}", ignored));
        check("invalid id rejected",
              !voice_memo_firmware::recording_meta_from_json(
                  "{\"recording_id\": \"../x\", \"wav_bytes\": 4}", ignored));
        check("future schema rejected",
              !voice_memo_firmware::recording_meta_from_json(
                  "{\"schema_version\": 99, \"recording_id\": \"rec-1\", \"wav_bytes\": 4}",
                  ignored));
        check("trailing garbage rejected",
              !voice_memo_firmware::recording_meta_from_json(
                  "{\"recording_id\": \"rec-1\", \"wav_bytes\": 4} garbage", ignored));
        check("negative number rejected",
              !voice_memo_firmware::recording_meta_from_json(
                  "{\"recording_id\": \"rec-1\", \"wav_bytes\": -1}", ignored));
        // An unknown field must not break an older reader.
        RecordingMeta forward;
        check("unknown field tolerated",
              voice_memo_firmware::recording_meta_from_json(
                  "{\"recording_id\": \"rec-1\", \"wav_bytes\": 4, \"future\": 9}", forward));
        checkU32("unknown field value ignored", forward.wav_bytes, 4);

        // has_crc must survive a round trip: writing "crc32": 0 for an unknown
        // CRC would come back as has_crc = true and make a later
        // VM_SD_VERIFY_CRC_ON_BOOT compare the real hash against 0.
        RecordingMeta noCrc;
        noCrc.recording_id = "rec-nocrc";
        noCrc.wav_bytes = 3200;
        noCrc.has_crc = false;
        noCrc.crc32 = 0;
        RecordingMeta noCrcBack;
        check("has_crc=false round trips",
              voice_memo_firmware::recording_meta_from_json(
                  voice_memo_firmware::recording_meta_to_json(noCrc), noCrcBack));
        check("has_crc stayed false", !noCrcBack.has_crc);
    }

    // ---------------------------------------------------------------------
    section("5. commit, queue and sidecar");
    {
        vm_test::MemoryFileSystem fs;
        RecordingStore store(fs, 64, 1024, false, false);
        check("begin mounts and creates the layout", store.begin());
        check("available", store.available());
        check("healthy", store.healthy());
        check("pending dir exists", fs.hasDir("/voice_memo/pending"));
        check("uploading dir exists", fs.hasDir("/voice_memo/uploading"));
        check("tmp dir exists", fs.hasDir("/voice_memo/tmp"));
        check("corrupt dir exists", fs.hasDir("/voice_memo/corrupt"));
        check("sent dir exists", fs.hasDir("/voice_memo/sent"));
        checkU32("empty queue", store.pendingCount(), 0);

        const std::vector<uint8_t> wavA = makeWav(3200, 0x20);
        const SaveOutcome outcome =
            store.savePending(metaFor("rec-00000001-00000001", 1, 100), wavA.data(), wavA.size());
        check("save returns Ok", outcome == SaveOutcome::Ok);
        checkU32("queue grew", store.pendingCount(), 1);
        check("wav committed", fs.hasFile(pendingWav("rec-00000001-00000001")));
        check("sidecar committed", fs.hasFile(pendingMeta("rec-00000001-00000001")));
        check("no tmp left behind", !fs.hasFile(tmpWavPath("rec-00000001-00000001")));
        check("no stray tmp meta",
              !fs.hasFile("/voice_memo/tmp/rec-00000001-00000001.json.tmp"));

        RecordingMeta reloaded;
        check("sidecar parses",
              voice_memo_firmware::recording_meta_from_json(
                  fs.readText(pendingMeta("rec-00000001-00000001")), reloaded));
        check("sidecar carries the tag", reloaded.tag_index == 1);
        check("sidecar carries the duration", reloaded.duration_ms == 100);
        check("sidecar carries the size", reloaded.wav_bytes == wavA.size());
        check("sidecar carries a crc", reloaded.has_crc);
        checkU32("sidecar crc matches",
                 reloaded.crc32,
                 voice_memo_firmware::crc32_ieee(wavA.data(), wavA.size()));
        checkU32("sidecar sequence is the first one", reloaded.sequence, 1);

        bool ok = false;
        const std::vector<uint8_t> readBack = readAll(store, "rec-00000001-00000001", &ok);
        check("streamed bytes read back", ok && readBack == wavA);

        check("duplicate id refused",
              store.savePending(metaFor("rec-00000001-00000001", 1, 100), wavA.data(), wavA.size()) ==
                  SaveOutcome::DuplicateId);
        checkU32("duplicate did not grow the queue", store.pendingCount(), 1);
        check("invalid id refused",
              store.savePending(metaFor("../evil", 0, 10), wavA.data(), wavA.size()) ==
                  SaveOutcome::InvalidId);

        // Every volume lock must be released on every path, including the
        // failure returns. The firmware backend additionally makes its mutex
        // recursive, because RecordingStore nests a multi-step commit around
        // per-operation calls; a leak here would strand the background worker.
        checkU32("volume lock balanced", static_cast<uint32_t>(fs.lock_count),
                 static_cast<uint32_t>(fs.unlock_count));
        check("volume lock was actually exercised", fs.lock_count > 0);
    }

    // ---------------------------------------------------------------------
    section("6. tmp files never become pending (atomic commit)");
    {
        vm_test::MemoryFileSystem fs;
        RecordingStore store(fs, 64, 1024, false, false);
        check("begin", store.begin());

        // A leftover tmp from a crashed write: no sidecar was committed, so the
        // recording was never confirmed and must be discarded.
        const std::vector<uint8_t> wav = makeWav(3200, 0x30);
        fs.putFile("/voice_memo/tmp/rec-orphan.wav.tmp", wav);
        fs.putText("/voice_memo/tmp/rec-orphan.json.tmp", "{\"recording_id\":\"rec-orphan\"}");

        RecordingStore reboot(fs, 64, 1024, false, false);
        check("reboot", reboot.begin());
        checkU32("orphan tmp not pending", reboot.pendingCount(), 0);
        check("orphan tmp removed", !fs.hasFile("/voice_memo/tmp/rec-orphan.wav.tmp"));
        check("orphan tmp meta removed", !fs.hasFile("/voice_memo/tmp/rec-orphan.json.tmp"));
        checkU32("tmp discard counted", reboot.discardedTmpCount(), 2);
    }

    // ---------------------------------------------------------------------
    section("7. interrupted commit is completed by recovery");
    {
        vm_test::MemoryFileSystem fs;
        RecordingStore store(fs, 64, 1024, false, false);
        check("begin", store.begin());

        // Metadata reached pending/, the WAV rename did not: recovery must
        // finish the commit because the sidecar already marked the intent.
        const std::vector<uint8_t> wav = makeWav(3200, 0x40);
        RecordingMeta meta = metaFor("rec-half", 3, 100);
        meta.wav_bytes = static_cast<uint32_t>(wav.size());
        fs.putText(pendingMeta("rec-half"), voice_memo_firmware::recording_meta_to_json(meta));
        fs.putFile(tmpWavPath("rec-half"), wav);

        RecordingStore reboot(fs, 64, 1024, false, false);
        check("reboot", reboot.begin());
        checkU32("recording recovered", reboot.pendingCount(), 1);
        check("wav promoted to pending", fs.hasFile(pendingWav("rec-half")));
        check("tmp cleared", !fs.hasFile(tmpWavPath("rec-half")));
        checkU32("recovery counted", reboot.recoveredCount(), 1);
    }

    // ---------------------------------------------------------------------
    section("8. reboot during an upload returns the item to pending");
    {
        vm_test::MemoryFileSystem fs;
        RecordingStore store(fs, 64, 1024, false, false);
        check("begin", store.begin());
        const std::vector<uint8_t> wav = makeWav(3200, 0x50);
        check("save",
              store.savePending(metaFor("rec-00000009-00000009", 0, 200), wav.data(), wav.size()) ==
                  SaveOutcome::Ok);
        check("mark uploading", store.markUploading("rec-00000009-00000009"));
        check("wav moved to uploading", fs.hasFile(uploadingWav("rec-00000009-00000009")));
        check("wav gone from pending", !fs.hasFile(pendingWav("rec-00000009-00000009")));

        // Power cut here: a fresh store on the same volume is a reboot.
        RecordingStore reboot(fs, 64, 1024, false, false);
        check("boot recovery", reboot.begin());
        checkU32("still one pending recording", reboot.pendingCount(), 1);
        check("back in pending", fs.hasFile(pendingWav("rec-00000009-00000009")));
        check("uploading dir empty of it", !fs.hasFile(uploadingWav("rec-00000009-00000009")));
        const voice_memo_firmware::StoreItem* item = reboot.find("rec-00000009-00000009");
        check("item is not marked uploading", item != nullptr && !item->in_uploading);
        check("recording_id preserved", item != nullptr && item->meta.recording_id == "rec-00000009-00000009");
    }

    // ---------------------------------------------------------------------
    section("9. recovery of WAVs without / with broken sidecars");
    {
        vm_test::MemoryFileSystem fs;
        RecordingStore store(fs, 64, 1024, false, false);
        check("begin", store.begin());

        // (a) valid WAV with no sidecar: the audio must survive.
        const std::vector<uint8_t> good = makeWav(6400, 0x60);
        fs.putFile(pendingWav("rec-nometa"), good);
        // (b) corrupt audio.
        std::vector<uint8_t> garbage(200, 0xAB);
        fs.putFile(pendingWav("rec-corrupt"), garbage);
        // (c) sidecar with no audio at all.
        RecordingMeta orphanMeta = metaFor("rec-orphanmeta", 1, 100);
        orphanMeta.wav_bytes = 44;
        fs.putText(pendingMeta("rec-orphanmeta"), voice_memo_firmware::recording_meta_to_json(orphanMeta));

        RecordingStore reboot(fs, 64, 1024, false, false);
        check("reboot", reboot.begin());
        checkU32("only the good WAV is queued", reboot.pendingCount(), 1);
        check("good WAV recovered", reboot.find("rec-nometa") != nullptr);
        check("recovered WAV got a sidecar", fs.hasFile(pendingMeta("rec-nometa")));
        {
            RecordingMeta recovered;
            check("recovered sidecar parses",
                  voice_memo_firmware::recording_meta_from_json(
                      fs.readText(pendingMeta("rec-nometa")), recovered));
            checkU32("recovered duration derived", recovered.duration_ms, 200);
            checkU32("recovered size", recovered.wav_bytes, good.size());
        }
        check("corrupt audio quarantined", fs.hasFile("/voice_memo/corrupt/rec-corrupt.wav"));
        check("corrupt audio not queued", reboot.find("rec-corrupt") == nullptr);
        check("orphan sidecar quarantined", fs.hasFile("/voice_memo/corrupt/rec-orphanmeta.json"));
        check("orphan sidecar not queued", reboot.find("rec-orphanmeta") == nullptr);
        checkU32("corrupt count", reboot.corruptCount(), 2);
    }

    // ---------------------------------------------------------------------
    section("10. card absent and card full");
    {
        vm_test::MemoryFileSystem fs;
        fs.mount_succeeds = false;
        RecordingStore store(fs, 64, 1024, false, false);
        check("begin fails without a card", !store.begin());
        check("not available", !store.available());
        const std::vector<uint8_t> wav = makeWav(3200, 0x70);
        check("save refused when unmounted",
              store.savePending(metaFor("rec-nosd", 0, 10), wav.data(), wav.size()) ==
                  SaveOutcome::NotMounted);
        checkU32("queue empty", store.pendingCount(), 0);
    }
    {
        vm_test::MemoryFileSystem fs;
        RecordingStore store(fs, 8, 4ULL * 1024ULL * 1024ULL, false, false);
        check("begin", store.begin());
        const std::vector<uint8_t> wav = makeWav(3200, 0x80);
        fs.free_bytes = 1024;  // far below wav size + reserve
        check("free space reserve enforced",
              store.savePending(metaFor("rec-full1", 0, 10), wav.data(), wav.size()) ==
                  SaveOutcome::StorageFull);
        checkU32("nothing queued", store.pendingCount(), 0);
        check("storage full flagged", store.storageFull());
        check("no partial file left", !fs.hasFile(pendingWav("rec-full1")));
        check("no tmp left", !fs.hasFile(tmpWavPath("rec-full1")));
    }
    {
        vm_test::MemoryFileSystem fs;
        RecordingStore store(fs, 2, 0, false, false);
        check("begin", store.begin());
        const std::vector<uint8_t> wav = makeWav(3200, 0x90);
        check("save 1", store.savePending(metaFor("rec-q1", 0, 10), wav.data(), wav.size()) == SaveOutcome::Ok);
        check("save 2", store.savePending(metaFor("rec-q2", 0, 10), wav.data(), wav.size()) == SaveOutcome::Ok);
        check("queue limit enforced",
              store.savePending(metaFor("rec-q3", 0, 10), wav.data(), wav.size()) ==
                  SaveOutcome::StorageFull);
        checkU32("existing items untouched", store.pendingCount(), 2);
        check("first still there", fs.hasFile(pendingWav("rec-q1")));
        check("second still there", fs.hasFile(pendingWav("rec-q2")));
    }

    // ---------------------------------------------------------------------
    section("11. upload lifecycle and retry identity");
    {
        vm_test::MemoryFileSystem fs;
        RecordingStore store(fs, 64, 1024, false, false);
        check("begin", store.begin());
        const std::vector<uint8_t> wav = makeWav(3200, 0xA0);
        check("save", store.savePending(metaFor("rec-retry", 2, 300), wav.data(), wav.size()) == SaveOutcome::Ok);

        check("mark uploading", store.markUploading("rec-retry"));
        check("in uploading dir", fs.hasFile(uploadingWav("rec-retry")));
        check("mark uploading again is idempotent", store.markUploading("rec-retry"));

        // Failure: same recording_id, same bytes, back in pending, attempt bumped.
        check("mark pending again", store.markPendingAgain("rec-retry", 1));
        check("back in pending", fs.hasFile(pendingWav("rec-retry")));
        const voice_memo_firmware::StoreItem* item = store.find("rec-retry");
        check("same recording_id", item != nullptr && item->meta.recording_id == "rec-retry");
        checkU32("attempt persisted", item == nullptr ? 9999 : item->meta.attempt_count, 1);
        checkU32("count unchanged by a retry", store.pendingCount(), 1);

        RecordingMeta persisted;
        check("attempt persisted to the sidecar",
              voice_memo_firmware::recording_meta_from_json(fs.readText(pendingMeta("rec-retry")), persisted));
        checkU32("sidecar attempt", persisted.attempt_count, 1);

        bool ok = false;
        const std::vector<uint8_t> bytesAgain = readAll(store, "rec-retry", &ok);
        check("retry reads the same bytes", ok && bytesAgain == wav);

        // Success: only now is the recording removed.
        check("mark uploaded", store.markUploaded("rec-retry"));
        check("wav removed", !fs.hasFile(pendingWav("rec-retry")));
        check("sidecar removed", !fs.hasFile(pendingMeta("rec-retry")));
        checkU32("queue empty", store.pendingCount(), 0);
    }

    // ---------------------------------------------------------------------
    section("12. a confirmed upload never deletes another recording");
    {
        vm_test::MemoryFileSystem fs;
        RecordingStore store(fs, 64, 1024, false, false);
        check("begin", store.begin());
        const std::vector<uint8_t> wavA = makeWav(3200, 0xB0);
        const std::vector<uint8_t> wavB = makeWav(3200, 0xC0);
        check("save A", store.savePending(metaFor("rec-A", 0, 10), wavA.data(), wavA.size()) == SaveOutcome::Ok);
        check("save B", store.savePending(metaFor("rec-B", 1, 20), wavB.data(), wavB.size()) == SaveOutcome::Ok);

        check("mark A uploading", store.markUploading("rec-A"));
        check("mark A uploaded", store.markUploaded("rec-A"));
        check("B still on the card", fs.hasFile(pendingWav("rec-B")));
        check("B sidecar intact", fs.hasFile(pendingMeta("rec-B")));
        checkU32("one recording left", store.pendingCount(), 1);
        check("only B remains", store.find("rec-A") == nullptr && store.find("rec-B") != nullptr);
        bool ok = false;
        check("B bytes intact", readAll(store, "rec-B", &ok) == wavB && ok);
    }

    // ---------------------------------------------------------------------
    section("13. oldest-first ordering across a reboot");
    {
        vm_test::MemoryFileSystem fs;
        RecordingStore store(fs, 64, 1024, false, false);
        check("begin", store.begin());
        const std::vector<uint8_t> wav = makeWav(1600, 0xD0);
        check("save 1", store.savePending(metaFor("rec-00000001", 0, 10), wav.data(), wav.size()) == SaveOutcome::Ok);
        check("save 2", store.savePending(metaFor("rec-00000002", 1, 10), wav.data(), wav.size()) == SaveOutcome::Ok);
        check("save 3", store.savePending(metaFor("rec-00000003", 2, 10), wav.data(), wav.size()) == SaveOutcome::Ok);
        checkU32("three queued", store.pendingCount(), 3);
        check("oldest is the first saved",
              store.oldestPending() != nullptr &&
                  store.oldestPending()->meta.recording_id == "rec-00000001");

        // Reboot: order must come from the persisted sequence, not from the
        // directory listing order.
        RecordingStore reboot(fs, 64, 1024, false, false);
        check("reboot", reboot.begin());
        checkU32("three queued after reboot", reboot.pendingCount(), 3);
        check("oldest survived the reboot",
              reboot.oldestPending() != nullptr &&
                  reboot.oldestPending()->meta.recording_id == "rec-00000001");
        check("mark first uploading", reboot.markUploading("rec-00000001"));
        check("back to pending", reboot.markPendingAgain("rec-00000001", 1));
        check("order preserved after a retry",
              reboot.oldestPending() != nullptr &&
                  reboot.oldestPending()->meta.recording_id == "rec-00000001");
        check("mark first uploaded", reboot.markUploaded("rec-00000001"));
        check("next oldest is the second",
              reboot.oldestPending() != nullptr &&
                  reboot.oldestPending()->meta.recording_id == "rec-00000002");
    }

    // ---------------------------------------------------------------------
    section("14. retention policy (VM_SD_KEEP_SENT)");
    {
        vm_test::MemoryFileSystem fs;
        RecordingStore store(fs, 64, 1024, true, false);
        check("begin", store.begin());
        const std::vector<uint8_t> wav = makeWav(1600, 0xE0);
        check("save", store.savePending(metaFor("rec-sent", 0, 10), wav.data(), wav.size()) == SaveOutcome::Ok);
        check("mark uploaded", store.markUploaded("rec-sent"));
        check("moved to sent", fs.hasFile("/voice_memo/sent/rec-sent.wav"));
        check("sidecar moved to sent", fs.hasFile("/voice_memo/sent/rec-sent.json"));
        check("removed from pending", !fs.hasFile(pendingWav("rec-sent")));
        checkU32("queue empty", store.pendingCount(), 0);
    }

    // ---------------------------------------------------------------------
    section("15. card removed during an attempt");
    {
        vm_test::MemoryFileSystem fs;
        RecordingStore store(fs, 64, 1024, false, false);
        check("begin", store.begin());
        const std::vector<uint8_t> wav = makeWav(1600, 0xF0);
        check("save", store.savePending(metaFor("rec-yank", 0, 10), wav.data(), wav.size()) == SaveOutcome::Ok);
        check("mark uploading", store.markUploading("rec-yank"));

        // The card is pulled: every further operation fails.
        fs.fail_removes = true;
        fs.fail_renames = true;
        fs.fail_writes = true;
        check("pending again still succeeds in memory", store.markPendingAgain("rec-yank", 1));
        check("item kept, never forgotten", store.find("rec-yank") != nullptr);
        checkU32("count stable", store.pendingCount(), 1);
        bool readOk = false;
        check("openRead still yields the bytes", readAll(store, "rec-yank", &readOk) == wav && readOk);

        // markUploaded on a dead volume drops it from the cache but must not
        // crash; the file remains for the next boot to recover and re-deliver
        // (the ingress is idempotent on recording_id).
        check("mark uploaded on a dead volume returns false", !store.markUploaded("rec-yank"));
        checkU32("cache consistent", store.pendingCount(), 0);
        check("file still on the (inaccessible) card", fs.hasFile(uploadingWav("rec-yank")));

        fs.fail_removes = false;
        fs.fail_renames = false;
        fs.fail_writes = false;
        RecordingStore reboot(fs, 64, 1024, false, false);
        check("reboot recovers it", reboot.begin());
        checkU32("re-delivered on the next boot", reboot.pendingCount(), 1);
    }
    {
        // A rename that fails *during recovery* must not hide the recording:
        // the file stays in uploading/, and it is still queued from there.
        vm_test::MemoryFileSystem fs;
        RecordingStore store(fs, 64, 1024, false, false);
        check("begin", store.begin());
        const std::vector<uint8_t> wav = makeWav(1600, 0x22);
        check("save", store.savePending(metaFor("rec-stuck", 0, 10), wav.data(), wav.size()) == SaveOutcome::Ok);
        check("mark uploading", store.markUploading("rec-stuck"));
        fs.fail_renames = true;
        RecordingStore reboot(fs, 64, 1024, false, false);
        check("recovery survives a rename failure", reboot.begin());
        checkU32("item queued from uploading/", reboot.pendingCount(), 1);
        const voice_memo_firmware::StoreItem* stuck = reboot.find("rec-stuck");
        check("item still known and marked uploading",
              stuck != nullptr && stuck->in_uploading);
        bool ok = false;
        check("its audio is still readable", readAll(reboot, "rec-stuck", &ok) == wav && ok);
    }

    // ---------------------------------------------------------------------
    section("16. pending count is consistent with the card");
    {
        vm_test::MemoryFileSystem fs;
        RecordingStore store(fs, 64, 1024, false, false);
        check("begin", store.begin());
        const std::vector<uint8_t> wav = makeWav(1600, 0x11);
        for (int i = 0; i < 5; ++i) {
            char id[32];
            std::snprintf(id, sizeof(id), "rec-count-%d", i);
            check("save", store.savePending(metaFor(id, 0, 10), wav.data(), wav.size()) == SaveOutcome::Ok);
        }
        checkU32("count", store.pendingCount(), 5);
        checkU32("one wav per recording", static_cast<uint32_t>(
                     fs.countFilesWithSuffixIn("/voice_memo/pending", ".wav")), 5U);
        checkU32("one sidecar per recording", static_cast<uint32_t>(
                     fs.countFilesWithSuffixIn("/voice_memo/pending", ".json")), 5U);
        check("mark 2 uploading", store.markUploading("rec-count-1") && store.markUploading("rec-count-2"));
        checkU32("count includes uploading", store.pendingCount(), 5);
        check("mark one uploaded", store.markUploaded("rec-count-1"));
        checkU32("count after upload", store.pendingCount(), 4);
        check("mark one corrupt", store.markCorrupt("rec-count-2"));
        checkU32("count after quarantine", store.pendingCount(), 3);
        check("quarantined audio kept", fs.hasFile("/voice_memo/corrupt/rec-count-2.wav"));
    }

    // ---------------------------------------------------------------------
    section("17. a failed promotion of an interrupted commit loses nothing");
    {
        vm_test::MemoryFileSystem fs;
        RecordingStore store(fs, 64, 1024, false, false);
        check("begin", store.begin());

        // Case C plus a failing rename: the sidecar proved the intent, but the
        // promotion to pending/ fails. The tmp WAV is the ONLY copy of the
        // audio, so it must survive, and so must its sidecar.
        const std::vector<uint8_t> wav = makeWav(3200, 0x77);
        RecordingMeta meta = metaFor("rec-c1", 1, 100);
        meta.wav_bytes = static_cast<uint32_t>(wav.size());
        fs.putText(pendingMeta("rec-c1"), voice_memo_firmware::recording_meta_to_json(meta));
        fs.putFile(tmpWavPath("rec-c1"), wav);

        fs.fail_renames = true;
        RecordingStore reboot(fs, 64, 1024, false, false);
        check("recovery runs", reboot.begin());
        check("tmp audio kept", fs.hasFile(tmpWavPath("rec-c1")));
        check("committed sidecar kept", fs.hasFile(pendingMeta("rec-c1")));
        check("audio not quarantined", !fs.hasFile("/voice_memo/corrupt/rec-c1.wav"));
        checkU32("not queued while it cannot be promoted", reboot.pendingCount(), 0);

        // A later boot with a healthy card finishes the commit.
        fs.fail_renames = false;
        RecordingStore reboot2(fs, 64, 1024, false, false);
        check("second boot", reboot2.begin());
        checkU32("commit completed", reboot2.pendingCount(), 1);
        check("wav promoted to pending", fs.hasFile(pendingWav("rec-c1")));
        bool ok = false;
        check("bytes intact", readAll(reboot2, "rec-c1", &ok) == wav && ok);
    }

    // ---------------------------------------------------------------------
    section("18. a half-moved pair is read from wherever the files are");
    {
        // markUploading renames the WAV first: a crash (or a failed rename)
        // between the two renames leaves the WAV in uploading/ and the sidecar
        // in pending/. Both must be honoured, and the valid audio must not be
        // quarantined for "missing metadata".
        vm_test::MemoryFileSystem fs;
        RecordingStore store(fs, 64, 1024, false, false);
        check("begin", store.begin());
        const std::vector<uint8_t> wav = makeWav(3200, 0x88);
        RecordingMeta meta = metaFor("rec-m2", 2, 100);
        meta.wav_bytes = static_cast<uint32_t>(wav.size());
        fs.putFile(uploadingWav("rec-m2"), wav);
        fs.putText(pendingMeta("rec-m2"), voice_memo_firmware::recording_meta_to_json(meta));

        fs.fail_renames = true;
        RecordingStore reboot(fs, 64, 1024, false, false);
        check("recovery runs", reboot.begin());
        checkU32("queued from uploading/", reboot.pendingCount(), 1);
        const voice_memo_firmware::StoreItem* item = reboot.find("rec-m2");
        check("item found", item != nullptr);
        check("tag read from the pending sidecar", item != nullptr && item->meta.tag_index == 2);
        check("known to be in uploading/", item != nullptr && item->in_uploading);
        check("not quarantined", !fs.hasFile("/voice_memo/corrupt/rec-m2.wav"));
        bool ok = false;
        check("audio readable", readAll(reboot, "rec-m2", &ok) == wav && ok);
    }

    // ---------------------------------------------------------------------
    section("19. attempt cap does not starve the queue");
    {
        vm_test::MemoryFileSystem fs;
        RecordingStore store(fs, 64, 1024, false, false);
        check("begin", store.begin());
        const std::vector<uint8_t> wav = makeWav(1600, 0xAA);
        check("A", store.savePending(metaFor("rec-capA", 0, 10), wav.data(), wav.size()) == SaveOutcome::Ok);
        check("B", store.savePending(metaFor("rec-capB", 0, 10), wav.data(), wav.size()) == SaveOutcome::Ok);
        check("C", store.savePending(metaFor("rec-capC", 0, 10), wav.data(), wav.size()) == SaveOutcome::Ok);

        // A has hit the cap, B has not: the queue must offer B first rather
        // than retrying A forever.
        check("mark A uploading", store.markUploading("rec-capA"));
        check("A pending again with a high attempt count",
              store.markPendingAgain("rec-capA", 50));
        check("oldest is still A",
              store.oldestPending() != nullptr &&
                  store.oldestPending()->meta.recording_id == "rec-capA");
        const voice_memo_firmware::StoreItem* next = store.nextPending(10);
        check("next under the cap is B",
              next != nullptr && next->meta.recording_id == "rec-capB");

        store.markUploading("rec-capB");
        store.markPendingAgain("rec-capB", 11);
        store.markUploading("rec-capC");
        store.markPendingAgain("rec-capC", 12);
        check("all capped -> no candidate", store.nextPending(10) == nullptr);
        check("nothing was dropped by the cap", store.pendingCount() == 3);
        check("oldest is still available as the slow fallback",
              store.oldestPending() != nullptr &&
                  store.oldestPending()->meta.recording_id == "rec-capA");
    }

    // ---------------------------------------------------------------------
    section("20. a provably missing file is dropped; a sick volume is not");
    {
        vm_test::MemoryFileSystem fs;
        RecordingStore store(fs, 64, 1024, false, false);
        check("begin", store.begin());
        const std::vector<uint8_t> wav = makeWav(1600, 0xBB);
        check("save", store.savePending(metaFor("rec-gone", 0, 10), wav.data(), wav.size()) == SaveOutcome::Ok);

        check("present file is not dropped", !store.dropIfMissing("rec-gone"));

        // Removed behind the firmware's back.
        fs.remove(pendingWav("rec-gone"));
        fs.remove(pendingMeta("rec-gone"));
        check("missing file is dropped", store.dropIfMissing("rec-gone"));
        checkU32("queue shrinks", store.pendingCount(), 0);

        // On an unhealthy volume absence must never be assumed.
        check("save 2", store.savePending(metaFor("rec-sick", 0, 10), wav.data(), wav.size()) == SaveOutcome::Ok);
        store.markUnhealthy();
        fs.fail_removes = true;
        check("unhealthy volume never drops", !store.dropIfMissing("rec-sick"));
        checkU32("item kept", store.pendingCount(), 1);
    }

    // ---------------------------------------------------------------------
    section("21. a pre-existing card file is never overwritten");
    {
        vm_test::MemoryFileSystem fs;
        RecordingStore store(fs, 64, 1024, false, false);
        check("begin", store.begin());
        const std::vector<uint8_t> older = makeWav(1600, 0xCC);
        const std::vector<uint8_t> newer = makeWav(3200, 0xDD);
        // A recording the store has not seen (for example left by an earlier
        // boot whose recovery has not run) must be treated as a duplicate.
        fs.putFile(pendingWav("rec-dup"), older);
        check("commit refused",
              store.savePending(metaFor("rec-dup", 0, 10), newer.data(), newer.size()) ==
                  SaveOutcome::DuplicateId);
        // Read the raw bytes straight out of the volume: the store does not know
        // this id, so its own reader is not the right way to check.
        check("older audio untouched", fs.bytesAt(pendingWav("rec-dup")) == older);
        check("still on the card under pending/", fs.hasFile(pendingWav("rec-dup")));
        checkU32("queue untouched by the refusal", store.pendingCount(), 0);
    }

    // ---------------------------------------------------------------------
    section("22. a rebuilt sidecar carries a real CRC");
    {
        // A WAV recovered without a sidecar gets one with the true CRC, so the
        // next boot with VM_SD_VERIFY_CRC_ON_BOOT=1 must not quarantine it.
        vm_test::MemoryFileSystem fs;
        RecordingStore first(fs, 64, 1024, false, false);
        check("begin", first.begin());
        const std::vector<uint8_t> wav = makeWav(3200, 0x99);
        fs.putFile(pendingWav("rec-nometa2"), wav);

        RecordingStore verified(fs, 64, 1024, false, true);
        check("verified boot recovers it", verified.begin());
        checkU32("queued", verified.pendingCount(), 1);
        checkU32("nothing quarantined", verified.corruptCount(), 0);
        check("sidecar written", fs.hasFile(pendingMeta("rec-nometa2")));
        RecordingMeta rebuilt;
        check("sidecar parses",
              voice_memo_firmware::recording_meta_from_json(
                  fs.readText(pendingMeta("rec-nometa2")), rebuilt));
        check("sidecar claims a CRC", rebuilt.has_crc);
        checkU32("CRC is the real one",
                 rebuilt.crc32,
                 voice_memo_firmware::crc32_ieee(wav.data(), wav.size()));

        RecordingStore again(fs, 64, 1024, false, true);
        check("second verified boot", again.begin());
        checkU32("still queued", again.pendingCount(), 1);
        checkU32("still nothing quarantined", again.corruptCount(), 0);
    }

    // ---------------------------------------------------------------------
    std::printf("\n%u checks, %u failed\n", static_cast<unsigned>(g_checks), static_cast<unsigned>(g_failures));
    if (g_failures == 0) {
        std::printf("all recording_store tests passed\n");
        return 0;
    }
    std::printf("%d recording_store test(s) FAILED\n", g_failures);
    return 1;
}
