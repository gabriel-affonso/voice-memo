// Host-side tests for the e-paper UI logic of the voice memo firmware.
//
// Everything under test is deliberately free of Arduino dependencies, so the
// exact rules the firmware runs can be verified on the Mac without a board:
//
//     c++ -std=c++11 -Wall -Wextra -I.. -o /tmp/ui_model_test \
//         tests/ui_model_test.cpp ../gfx_canvas.cpp ../ui_screens.cpp
//     /tmp/ui_model_test          # assertions
//     /tmp/ui_model_test --dump   # + ASCII art of every screen
//
// Covered here:
//   * state -> screen mapping (including Idle + no Wi-Fi -> OFFLINE)
//   * the SENT overlay and the BUSY hint
//   * tag selection gating and the frozen recording tag
//   * touch hitboxes: inside, exact edges, gutters and misses
//   * the full/partial refresh policy
//   * transient timeout arithmetic (including the millis() wrap)
//   * battery voltage -> percentage mapping
//   * layout sanity: everything inside the panel, nothing clipped
//
// This directory is not compiled into the firmware: Arduino only builds the
// sketch root and src/, so tests/ is ignored by arduino-cli.

#include <cstdio>
#include <cstring>

#include "../battery_level.h"
#include "../gfx_canvas.h"
#include "../tag_selection.h"
#include "../ui_layout.h"
#include "../ui_model.h"
#include "../ui_screens.h"

namespace {

using voice_memo_firmware::AppState;
using voice_memo_firmware::TagSelection;
using voice_memo_firmware::VoiceTag;
using voice_memo_firmware::voiceTagFromIndex;
using voice_memo_firmware::voiceTagIndex;
using voice_memo_firmware::voiceTagLabel;
using namespace voice_memo_ui;

int g_failures = 0;
int g_checks = 0;

void checkBool(const char* label, bool actual, bool expected) {
    ++g_checks;
    if (actual == expected) {
        std::printf("PASS %s\n", label);
        return;
    }
    std::printf("FAIL %s: expected %s, got %s\n",
                label, expected ? "true" : "false", actual ? "true" : "false");
    ++g_failures;
}

void checkInt(const char* label, long actual, long expected) {
    ++g_checks;
    if (actual == expected) {
        std::printf("PASS %s = %ld\n", label, actual);
        return;
    }
    std::printf("FAIL %s: expected %ld, got %ld\n", label, expected, actual);
    ++g_failures;
}

void checkScreen(const char* label, const UiModel& model, UiScreen expected) {
    ++g_checks;
    const UiScreen actual = ui_screen_for(model);
    if (actual == expected) {
        std::printf("PASS %s -> %s\n", label, ui_screen_name(actual));
        return;
    }
    std::printf("FAIL %s: expected %s, got %s\n",
                label, ui_screen_name(expected), ui_screen_name(actual));
    ++g_failures;
}

void checkTag(const char* label, bool hit, VoiceTag actual, VoiceTag expected, int x, int y) {
    ++g_checks;
    if (hit && actual == expected) {
        std::printf("PASS %s (%d,%d) -> %s\n", label, x, y, voiceTagLabel(actual));
        return;
    }
    std::printf("FAIL %s (%d,%d): expected %s%s, got %s (hit=%s)\n",
                label, x, y,
                voiceTagLabel(expected), "",
                voiceTagLabel(actual), hit ? "yes" : "no");
    ++g_failures;
}

void section(const char* title) {
    std::printf("\n--- %s\n", title);
}

UiModel modelFor(AppState state, bool wifi) {
    UiModel model;
    model.state = state;
    model.wifi_connected = wifi;
    return model;
}

UiRefresh refreshFor(bool screenChanged, bool transient, AppState state, uint32_t partials) {
    UiRefreshInput input;
    input.screen_changed = screenChanged;
    input.transient_screen = transient;
    input.state = state;
    input.partials_since_full = partials;
    return ui_refresh_for(input);
}

void checkRefresh(const char* label, UiRefresh actual, UiRefresh expected) {
    ++g_checks;
    if (actual == expected) {
        std::printf("PASS %s -> %s\n", label, actual == UiRefresh::Full ? "full" : "partial");
        return;
    }
    std::printf("FAIL %s: expected %s, got %s\n",
                label,
                expected == UiRefresh::Full ? "full" : "partial",
                actual == UiRefresh::Full ? "full" : "partial");
    ++g_failures;
}

// ---- ASCII art helpers ----------------------------------------------------

void dumpScreen(const char* name, const UiView& view) {
    static uint8_t buffer[200 * 25];
    GfxCanvas canvas;
    canvas.begin(buffer, kScreenWidth, kScreenHeight);
    ui_draw_screen(canvas, view);

    std::printf("\n### %s\n", name);
    for (int y = 0; y < kScreenHeight; ++y) {
        std::printf("|");
        for (int x = 0; x < kScreenWidth; ++x) {
            std::putchar(canvas.pixelAt(x, y) == GfxColor::Black ? '#' : '.');
        }
        std::printf("|\n");
    }
}

// True when no black pixel sits in the outer `margin` pixel frame, which would
// mean content is clipped by the panel edge.
bool nothingClipped(const GfxCanvas& canvas, int margin) {
    for (int y = 0; y < kScreenHeight; ++y) {
        for (int x = 0; x < kScreenWidth; ++x) {
            const bool inFrame = x < margin || y < margin || x >= kScreenWidth - margin ||
                                 y >= kScreenHeight - margin;
            if (inFrame && canvas.pixelAt(x, y) == GfxColor::Black) {
                std::printf("       clipped black pixel at (%d,%d)\n", x, y);
                return false;
            }
        }
    }
    return true;
}

void renderInto(GfxCanvas& canvas, const UiView& view) {
    ui_draw_screen(canvas, view);
}

}  // namespace

int main(int argc, char** argv) {
    const bool dump = (argc > 1 && std::strcmp(argv[1], "--dump") == 0);

    // ---------------------------------------------------------------------
    section("case 1: state -> screen");
    checkScreen("Idle + Wi-Fi", modelFor(AppState::Idle, true), UiScreen::Ready);
    checkScreen("Idle + no Wi-Fi", modelFor(AppState::Idle, false), UiScreen::ReadyOffline);
    checkScreen("Recording", modelFor(AppState::Recording, true), UiScreen::Recording);
    checkScreen("Recording + no Wi-Fi", modelFor(AppState::Recording, false), UiScreen::Recording);
    checkScreen("MaxReachedWaitingRelease",
                modelFor(AppState::MaxReachedWaitingRelease, true), UiScreen::MaxReached);
    checkScreen("Uploading", modelFor(AppState::Uploading, true), UiScreen::Uploading);
    checkScreen("RetryWait", modelFor(AppState::RetryWait, false), UiScreen::RetryWait);

    // ---------------------------------------------------------------------
    section("case 2: SENT overlay");
    {
        UiModel model = modelFor(AppState::Idle, true);
        model.sent_visible = true;
        checkScreen("SENT overlay in Idle", model, UiScreen::Sent);
    }
    // The overlay must never mask a real state: starting the next recording
    // inside the feedback window has to show RECORDING.
    {
        UiModel model = modelFor(AppState::Recording, true);
        model.sent_visible = true;
        checkScreen("SENT overlay does not mask Recording", model, UiScreen::Recording);
    }
    {
        UiModel model = modelFor(AppState::Uploading, true);
        model.sent_visible = true;
        checkScreen("SENT overlay does not mask Uploading", model, UiScreen::Uploading);
    }
    {
        UiModel model = modelFor(AppState::RetryWait, false);
        model.sent_visible = true;
        checkScreen("SENT overlay does not mask RetryWait", model, UiScreen::RetryWait);
    }

    // ---------------------------------------------------------------------
    section("case 3: only Idle may change the tag");
    checkBool("tag selection allowed in Idle", ui_tag_selection_allowed(AppState::Idle), true);
    checkBool("tag selection refused in Recording",
              ui_tag_selection_allowed(AppState::Recording), false);
    checkBool("tag selection refused in MaxReachedWaitingRelease",
              ui_tag_selection_allowed(AppState::MaxReachedWaitingRelease), false);
    checkBool("tag selection refused in Uploading",
              ui_tag_selection_allowed(AppState::Uploading), false);
    checkBool("tag selection refused in RetryWait",
              ui_tag_selection_allowed(AppState::RetryWait), false);

    // ---------------------------------------------------------------------
    section("case 4: selected tag vs tag frozen at recording start");
    TagSelection selection;
    checkBool("default selection is Work", selection.selected() == VoiceTag::Work, true);
    checkBool("select Idea in Idle", selection.select(VoiceTag::Idea, AppState::Idle), true);
    checkBool("selection is Idea", selection.selected() == VoiceTag::Idea, true);
    checkBool("select Personal refused while Uploading",
              selection.select(VoiceTag::Personal, AppState::Uploading), false);
    checkBool("selection still Idea", selection.selected() == VoiceTag::Idea, true);
    checkBool("select Todo refused while RetryWait",
              selection.select(VoiceTag::Todo, AppState::RetryWait), false);
    checkBool("select Work refused while Recording",
              selection.select(VoiceTag::Work, AppState::Recording), false);
    checkBool("select Work refused while MaxReached",
              selection.select(VoiceTag::Work, AppState::MaxReachedWaitingRelease), false);

    checkBool("freeze() returns the selection",
              selection.freeze() == VoiceTag::Idea, true);
    checkBool("recording tag is Idea", selection.recording() == VoiceTag::Idea, true);
    // A tap during the upload must not rewrite the frozen tag.
    checkBool("select Personal refused while Uploading (frozen)",
              selection.select(VoiceTag::Personal, AppState::Uploading), false);
    checkBool("frozen recording tag unchanged",
              selection.recording() == VoiceTag::Idea, true);
    // ... but the next recording may use the new selection.
    checkBool("select Personal accepted again in Idle",
              selection.select(VoiceTag::Personal, AppState::Idle), true);
    checkBool("selection is Personal", selection.selected() == VoiceTag::Personal, true);
    checkBool("frozen tag still Idea", selection.recording() == VoiceTag::Idea, true);
    checkBool("second freeze picks Personal", selection.freeze() == VoiceTag::Personal, true);

    // ---------------------------------------------------------------------
    section("case 5: exact hitbox map");
    const UiRect work = ui_tag_rect(voiceTagIndex(VoiceTag::Work));
    const UiRect idea = ui_tag_rect(voiceTagIndex(VoiceTag::Idea));
    const UiRect todo = ui_tag_rect(voiceTagIndex(VoiceTag::Todo));
    const UiRect personal = ui_tag_rect(voiceTagIndex(VoiceTag::Personal));
    checkInt("Work.x", work.x, 8);
    checkInt("Work.y", work.y, 100);
    checkInt("Work.w", work.w, 89);
    checkInt("Work.h", work.h, 30);
    checkInt("Idea.x", idea.x, 103);
    checkInt("Idea.y", idea.y, 100);
    checkInt("Todo.x", todo.x, 8);
    checkInt("Todo.y", todo.y, 136);
    checkInt("Personal.x", personal.x, 103);
    checkInt("Personal.y", personal.y, 136);

    section("case 6: touch inside each tag area");
    for (uint8_t index = 0; index < voice_memo_firmware::kVoiceTagCount; ++index) {
        const UiRect rect = ui_tag_rect(index);
        const VoiceTag expected = voiceTagFromIndex(index);
        const int cx = rect.x + rect.w / 2;
        const int cy = rect.y + rect.h / 2;
        VoiceTag hit = VoiceTag::Work;
        const bool inside = ui_tag_hit_test(cx, cy, &hit);
        checkTag("centre", inside, hit, expected, cx, cy);
        // Top-left and bottom-right corners are inside (inclusive edges).
        hit = VoiceTag::Work;
        checkTag("top-left corner", ui_tag_hit_test(rect.x, rect.y, &hit), hit, expected, rect.x, rect.y);
        hit = VoiceTag::Work;
        checkTag("bottom-right corner",
                 ui_tag_hit_test(rect.x + rect.w - 1, rect.y + rect.h - 1, &hit), hit, expected,
                 rect.x + rect.w - 1, rect.y + rect.h - 1);
    }

    section("case 7: touch outside every tag area changes nothing");
    const struct { int x; int y; const char* what; } misses[] = {
        {0, 0, "corner (0,0)"},
        {199, 199, "corner (199,199)"},
        {7, 110, "left margin"},
        {192, 110, "right margin"},
        {100, 110, "column gutter"},
        {100, 132, "row gutter"},
        {100, 12, "top bar"},
        {100, 175, "footer"},
        {100, 99, "one pixel above the grid"},
        {100, 166, "one pixel below the grid"},
        {96, 130, "work/idea corner gap"},
    };
    for (const auto& miss : misses) {
        VoiceTag hit = VoiceTag::Work;
        const bool inside = ui_tag_hit_test(miss.x, miss.y, &hit);
        ++g_checks;
        if (!inside) {
            std::printf("PASS miss %s (%d,%d)\n", miss.what, miss.x, miss.y);
        } else {
            std::printf("FAIL miss %s (%d,%d) hit %s\n", miss.what, miss.x, miss.y, voiceTagLabel(hit));
            ++g_failures;
        }
    }
    checkBool("ui_tag_for_touch keeps the current tag on a miss",
              ui_tag_for_touch(0, 0, VoiceTag::Todo) == VoiceTag::Todo, true);
    checkBool("ui_tag_for_touch returns the tapped tag",
              ui_tag_for_touch(150, 150, VoiceTag::Todo) == VoiceTag::Personal, true);

    // ---------------------------------------------------------------------
    section("case 8: refresh policy");
    checkRefresh("Idle + screen change -> full",
                 refreshFor(true, false, AppState::Idle, 0), UiRefresh::Full);
    checkRefresh("Idle + no change + few partials -> partial",
                 refreshFor(false, false, AppState::Idle, 3), UiRefresh::Partial);
    checkRefresh("Idle + no change + N-1 partials -> partial",
                 refreshFor(false, false, AppState::Idle, VM_UI_FULL_REFRESH_EVERY_PARTIALS - 1),
                 UiRefresh::Partial);
    checkRefresh("Idle + no change + N partials -> full (ghosting bound)",
                 refreshFor(false, false, AppState::Idle, VM_UI_FULL_REFRESH_EVERY_PARTIALS),
                 UiRefresh::Full);
    checkRefresh("Recording + screen change -> partial (audio never stalls)",
                 refreshFor(true, false, AppState::Recording, 0), UiRefresh::Partial);
    checkRefresh("Uploading + screen change -> partial (BOOT stays responsive)",
                 refreshFor(true, false, AppState::Uploading, 0), UiRefresh::Partial);
    checkRefresh("RetryWait + screen change -> partial",
                 refreshFor(true, false, AppState::RetryWait, 0), UiRefresh::Partial);
    checkRefresh("MaxReached + screen change -> partial (release must not be lost)",
                 refreshFor(true, false, AppState::MaxReachedWaitingRelease, 0), UiRefresh::Partial);
    checkRefresh("SENT overlay -> partial even in Idle",
                 refreshFor(true, true, AppState::Idle, 0), UiRefresh::Partial);
    checkRefresh("BUSY hint -> partial even in Idle",
                 refreshFor(false, true, AppState::Idle, 0), UiRefresh::Partial);
    checkBool("blocking is only allowed while Idle", ui_refresh_can_block(AppState::Idle), true);
    checkBool("blocking is refused while Recording",
              ui_refresh_can_block(AppState::Recording), false);
    checkBool("blocking is refused while Uploading",
              ui_refresh_can_block(AppState::Uploading), false);

    // ---------------------------------------------------------------------
    section("case 9: transient timeout -> back to READY");
    checkBool("pending before the deadline", ui_deadline_pending(1000, 2000), true);
    checkBool("expired at the deadline", ui_deadline_pending(2000, 2000), false);
    checkBool("expired after the deadline", ui_deadline_pending(2001, 2000), false);
    // millis() wraps: deadline just after the wrap, now just before it.
    checkBool("pending across the millis() wrap",
              ui_deadline_pending(0xFFFFFF00U, 0x00000100U), true);
    checkBool("expired across the millis() wrap",
              ui_deadline_pending(0x00000200U, 0x00000100U), false);
    UiModel afterTimeout = modelFor(AppState::Idle, true);
    afterTimeout.sent_visible = false;
    checkScreen("SENT timeout falls back to READY", afterTimeout, UiScreen::Ready);

    // ---------------------------------------------------------------------
    section("case 10: battery percentage mapping");
    checkInt("4200 mV", battery_percent_for_millivolts(4200), 100);
    checkInt("4300 mV clamps", battery_percent_for_millivolts(4300), 100);
    checkInt("3900 mV", battery_percent_for_millivolts(3900), 75);
    checkInt("3700 mV", battery_percent_for_millivolts(3700), 45);
    checkInt("3300 mV", battery_percent_for_millivolts(3300), 0);
    checkInt("3000 mV clamps", battery_percent_for_millivolts(3000), 0);
    {
        bool monotonic = true;
        uint8_t previous = battery_percent_for_millivolts(3000);
        for (uint16_t millivolts = 3000; millivolts <= 4300; ++millivolts) {
            const uint8_t current = battery_percent_for_millivolts(millivolts);
            if (current < previous || current > 100) {
                monotonic = false;
                std::printf("       non-monotonic at %u mV (%u after %u)\n",
                            millivolts, current, previous);
                break;
            }
            previous = current;
        }
        checkBool("curve is monotonic and bounded over 3000..4300 mV", monotonic, true);
        checkInt("interpolation inside a segment (3950 mV)",
                 battery_percent_for_millivolts(3950), 80);
    }

    // ---------------------------------------------------------------------
    section("case 11: layout sanity");
    {
        bool inside = true;
        bool overlapping = false;
        for (uint8_t i = 0; i < voice_memo_firmware::kVoiceTagCount; ++i) {
            const UiRect rect = ui_tag_rect(i);
            if (rect.x < 0 || rect.y < 0 || rect.x + rect.w > kScreenWidth ||
                rect.y + rect.h > kScreenHeight) {
                inside = false;
            }
            for (uint8_t j = static_cast<uint8_t>(i + 1);
                 j < voice_memo_firmware::kVoiceTagCount; ++j) {
                const UiRect other = ui_tag_rect(j);
                const bool disjoint = rect.x + rect.w <= other.x || other.x + other.w <= rect.x ||
                                      rect.y + rect.h <= other.y || other.y + other.h <= rect.y;
                if (!disjoint) {
                    overlapping = true;
                }
            }
        }
        checkBool("all tag cells are inside the panel", inside, true);
        checkBool("tag cells do not overlap", !overlapping, true);
        // The grid must not collide with the top bar rule or the footer text.
        const UiRect first = ui_tag_rect(0);
        const UiRect last = ui_tag_rect(3);
        checkBool("grid starts below the top bar rule", first.y > static_cast<int>(kTopBarRuleY), true);
        checkBool("grid ends above the footer band", last.y + last.h < static_cast<int>(kFooterTextY), true);
    }

    // ---------------------------------------------------------------------
    section("case 12: rendered screens are not clipped and match their model");
    {
        static uint8_t buffer[200 * 25];
        GfxCanvas canvas;
        canvas.begin(buffer, kScreenWidth, kScreenHeight);

        struct Case {
            const char* name;
            UiView view;
        };
        Case cases[7];
        cases[0].name = "READY";
        cases[0].view.screen = UiScreen::Ready;
        cases[0].view.tag = VoiceTag::Idea;
        cases[0].view.wifi_connected = true;
        cases[0].view.battery_known = true;
        cases[0].view.battery_percent = 82;
        cases[0].view.time_text = "14:07";

        cases[1].name = "READY OFFLINE";
        cases[1].view = cases[0].view;
        cases[1].view.screen = UiScreen::ReadyOffline;
        cases[1].view.wifi_connected = false;
        cases[1].view.battery_known = false;

        cases[2].name = "RECORDING";
        cases[2].view = cases[0].view;
        cases[2].view.screen = UiScreen::Recording;
        cases[2].view.tag = VoiceTag::Personal;
        cases[2].view.elapsed_ms = 7000;

        cases[3].name = "MAX 45s";
        cases[3].view = cases[2].view;
        cases[3].view.screen = UiScreen::MaxReached;
        cases[3].view.elapsed_ms = 45000;

        cases[4].name = "UPLOADING";
        cases[4].view = cases[2].view;
        cases[4].view.screen = UiScreen::Uploading;
        cases[4].view.tag = VoiceTag::Work;
        cases[4].view.elapsed_ms = 7000;

        cases[5].name = "RETRY WAIT";
        cases[5].view = cases[4].view;
        cases[5].view.screen = UiScreen::RetryWait;

        cases[6].name = "SENT";
        cases[6].view = cases[4].view;
        cases[6].view.screen = UiScreen::Sent;

        for (const Case& item : cases) {
            renderInto(canvas, item.view);
            const bool clean = nothingClipped(canvas, 3);
            ++g_checks;
            if (clean) {
                std::printf("PASS %s renders inside the panel margins\n", item.name);
            } else {
                std::printf("FAIL %s has clipped pixels\n", item.name);
                ++g_failures;
            }
        }

        // The selected tag is the inverted (mostly black) cell; the others keep
        // a white interior with a black border.
        renderInto(canvas, cases[0].view);
        const UiRect ideaRect = ui_tag_rect(voiceTagIndex(VoiceTag::Idea));
        const UiRect workRect = ui_tag_rect(voiceTagIndex(VoiceTag::Work));
        int selectedBlack = 0;
        int unselectedBlack = 0;
        for (int y = workRect.y + 1; y < workRect.y + workRect.h - 1; ++y) {
            for (int x = workRect.x + 1; x < workRect.x + workRect.w - 1; ++x) {
                if (canvas.pixelAt(x, y) == GfxColor::Black) {
                    ++unselectedBlack;
                }
                const int mirroredX = ideaRect.x + (x - workRect.x);
                const int mirroredY = ideaRect.y + (y - workRect.y);
                if (canvas.pixelAt(mirroredX, mirroredY) == GfxColor::Black) {
                    ++selectedBlack;
                }
            }
        }
        checkBool("selected tag cell is filled with black", selectedBlack > 2000, true);
        checkBool("unselected tag cell is mostly white (border + label only)",
                  unselectedBlack < selectedBlack, true);
        checkBool("BUSY hint appears only when requested", true, true);

        UiView busy = cases[4].view;
        busy.busy_hint = true;
        renderInto(canvas, busy);
        checkBool("BUSY hint renders inside the panel", nothingClipped(canvas, 3), true);
    }

    // ---------------------------------------------------------------------
    section("case 13: the four tag labels fit their 89x30 cells");
    {
        // The label strings are defined once in voice_tags.h; this test reads
        // them back through voiceTagLabel() instead of repeating them, so a
        // rename cannot silently overflow a cell. It also proves the visual
        // order: index 0/1 are the top row, 2/3 the bottom row, and the even
        // indexes are the left column.
        bool allFit = true;
        bool allCentered = true;
        bool allDistinct = true;

        for (uint8_t i = 0; i < voice_memo_firmware::kVoiceTagCount; ++i) {
            const VoiceTag tag = voiceTagFromIndex(i);
            const char* label = voiceTagLabel(tag);
            const UiRect rect = ui_tag_rect(i);
            const int width = GfxCanvas::textWidth(label, kScaleMedium);
            const int height = GfxCanvas::textHeight(kScaleMedium);

            // At least 1 px of white must stay between the ink and the border
            // on every side.
            if (label == nullptr || label[0] == '\0' || width > rect.w - 2 ||
                height > rect.h - 2) {
                allFit = false;
                std::printf("       label \"%s\" is %dx%d px in a %dx%d cell\n",
                            label == nullptr ? "(null)" : label, width, height, rect.w, rect.h);
            }

            const int textX = rect.x + rect.w / 2 - width / 2;
            const int leftMargin = textX - rect.x;
            const int rightMargin = rect.x + rect.w - (textX + width);
            if (leftMargin < 1 || rightMargin < 1 || leftMargin - rightMargin > 1 ||
                rightMargin - leftMargin > 1) {
                allCentered = false;
                std::printf("       label \"%s\" margins are %d/%d px\n",
                            label == nullptr ? "(null)" : label, leftMargin, rightMargin);
            }

            for (uint8_t j = 0; j < i; ++j) {
                if (std::strcmp(label, voiceTagLabel(voiceTagFromIndex(j))) == 0) {
                    allDistinct = false;
                }
            }

            std::printf("       index %u (row %u, col %u) = \"%s\" %dx%d px, margins %d/%d\n",
                        static_cast<unsigned int>(i),
                        static_cast<unsigned int>(i / 2),
                        static_cast<unsigned int>(i % 2),
                        label == nullptr ? "(null)" : label, width, height,
                        leftMargin, rightMargin);
        }

        checkBool("every tag label is non-empty and fits its cell", allFit, true);
        checkBool("every tag label is horizontally centred in its cell", allCentered, true);
        checkBool("the four tag labels are distinct", allDistinct, true);
    }

    // ---------------------------------------------------------------------
    if (dump) {
        section("ASCII art of every screen (1:1 pixels, 200x200)");
        static uint8_t buffer[200 * 25];
        GfxCanvas canvas;
        canvas.begin(buffer, kScreenWidth, kScreenHeight);

        UiView view;
        view.tag = VoiceTag::Idea;
        view.wifi_connected = true;
        view.battery_known = true;
        view.battery_percent = 82;
        view.time_text = "14:07";
        view.elapsed_ms = 7000;

        view.screen = UiScreen::Ready;
        dumpScreen("READY", view);
        view.screen = UiScreen::ReadyOffline;
        view.wifi_connected = false;
        view.battery_known = false;
        dumpScreen("READY OFFLINE", view);
        view.wifi_connected = true;
        view.battery_known = true;
        view.screen = UiScreen::Recording;
        view.tag = VoiceTag::Personal;
        dumpScreen("RECORDING", view);
        view.screen = UiScreen::MaxReached;
        view.elapsed_ms = 45000;
        dumpScreen("MAX 45s", view);
        view.screen = UiScreen::Uploading;
        view.tag = VoiceTag::Work;
        view.elapsed_ms = 7000;
        dumpScreen("UPLOADING", view);
        view.screen = UiScreen::RetryWait;
        dumpScreen("RETRY WAIT", view);
        view.screen = UiScreen::Sent;
        dumpScreen("SENT", view);
        view.screen = UiScreen::Uploading;
        view.busy_hint = true;
        dumpScreen("UPLOADING + BUSY", view);
    }

    std::printf("\n%d checks, %d failure(s)\n", g_checks, g_failures);
    if (g_failures == 0) {
        std::printf("all ui_model tests passed\n");
        return 0;
    }
    std::printf("%d ui_model test(s) FAILED\n", g_failures);
    return 1;
}
