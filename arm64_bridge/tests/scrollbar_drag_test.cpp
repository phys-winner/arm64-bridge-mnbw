#include "scrollbar_drag.h"

#include <cmath>
#include <cstdio>
#include <limits>

using warband::input::ScrollbarDragModel;
using warband::input::ScrollableContentDragModel;
using warband::input::EstimateContentExtentPixels;
using warband::input::ScrollbarValueForContentPointer;
using warband::input::ScrollbarValueForPointer;
using warband::input::IsConversationResponseGeometry;
using warband::input::MirroredContentPointerY;
using warband::input::ShouldInspectUiScrollbars;
using warband::input::ShouldApplyScrollableContentDragSample;
using warband::input::ValidScrollbarDragModel;
using warband::input::ValidScrollableContentDragModel;

namespace {

struct TestState {
    int checks = 0;
    int failures = 0;

    void Check(bool condition, const char* expression, int line) {
        ++checks;
        if (!condition) {
            ++failures;
            std::fprintf(stderr, "FAIL line %d: %s\n", line, expression);
        }
    }
};

#define CHECK(state, expression) \
    (state).Check(static_cast<bool>(expression), #expression, __LINE__)

void CheckClose(TestState& state, float actual, float expected,
                float epsilon = 0.0001f) {
    CHECK(state, std::fabs(actual - expected) <= epsilon);
}

void TestRelativeDragAcrossNormalizedRange(TestState& state) {
    const ScrollbarDragModel model{100.0f, 20.0f, 420.0f,
                                   0.75f, 0.0f, 1.0f};
    float value = -1.0f;
    CHECK(state, ValidScrollbarDragModel(model));
    CHECK(state, ScrollbarValueForPointer(model, 100.0f, &value));
    CheckClose(state, value, 0.75f);
    CHECK(state, ScrollbarValueForPointer(model, 300.0f, &value));
    CheckClose(state, value, 0.25f);
}

void TestDragClampsAtBothEnds(TestState& state) {
    const ScrollbarDragModel model{200.0f, 0.0f, 400.0f,
                                   0.5f, 0.0f, 1.0f};
    float value = -1.0f;
    CHECK(state, ScrollbarValueForPointer(model, -800.0f, &value));
    CheckClose(state, value, 1.0f);
    CHECK(state, ScrollbarValueForPointer(model, 1200.0f, &value));
    CheckClose(state, value, 0.0f);
}

void TestNonUnitGuestRange(TestState& state) {
    const ScrollbarDragModel model{50.0f, 10.0f, 210.0f,
                                   1.0f, -2.0f, 2.0f};
    float value = 0.0f;
    CHECK(state, ScrollbarValueForPointer(model, 150.0f, &value));
    CheckClose(state, value, -1.0f);
}

void TestContentDragUsesDocumentExtent(TestState& state) {
    const ScrollableContentDragModel model{
        930.0f, 1080.0f * 1.411f, 1.0f, 0.0f, 1.0f};
    float value = -1.0f;
    CHECK(state, ValidScrollableContentDragModel(model));
    CHECK(state, ScrollbarValueForContentPointer(model, 930.0f, &value));
    CheckClose(state, value, 1.0f);
    CHECK(state, ScrollbarValueForContentPointer(model, 760.0f, &value));
    CheckClose(state, value, 1.0f - (170.0f / 1080.0f) / 1.411f);

    // Content follows the finger direction, opposite to direct thumb motion:
    // moving up reveals later rows and therefore decreases the bottom-origin
    // scrollbar value.
    CHECK(state, value < 0.90f && value > 0.88f);
    CHECK(state, ScrollbarValueForContentPointer(model, -5000.0f, &value));
    CheckClose(state, value, 0.0f);
}

void TestContentExtentCanBeEstimatedFromVisibleRows(TestState& state) {
    float extent = -1.0f;
    CHECK(state, EstimateContentExtentPixels(60.0f, 7u, 20u, &extent));
    CheckClose(state, extent, 780.0f);
    CHECK(state, !EstimateContentExtentPixels(60.0f, 7u, 7u, &extent));
    CHECK(state, !EstimateContentExtentPixels(0.0f, 7u, 20u, &extent));
    CHECK(state, !EstimateContentExtentPixels(60.0f, 0u, 20u, &extent));
}

void TestContentDragRejectsInvalidGeometry(TestState& state) {
    float value = 123.0f;
    CHECK(state, !ScrollbarValueForContentPointer(
        ScrollableContentDragModel{100.0f, 0.0f,
                                   0.5f, 0.0f, 1.0f},
        50.0f, &value));
    CHECK(state, !ScrollbarValueForContentPointer(
        ScrollableContentDragModel{100.0f, 1080.0f,
                                   0.5f, 0.0f, 1.0f},
        std::numeric_limits<float>::infinity(), &value));
    CheckClose(state, value, 123.0f);
}

void TestContentDragUsesOnlyMoveSamples(TestState& state) {
    CHECK(state, !ShouldApplyScrollableContentDragSample(true, true));
    CHECK(state, ShouldApplyScrollableContentDragSample(true, false));
    // A release coordinate must never replace the last rendered MOVE value.
    CHECK(state, !ShouldApplyScrollableContentDragSample(false, false));
    CHECK(state, !ShouldApplyScrollableContentDragSample(false, true));
}

void TestRejectsInvalidGuestState(TestState& state) {
    float value = 123.0f;
    const ScrollbarDragModel flat_track{50.0f, 10.0f, 10.0f,
                                        0.5f, 0.0f, 1.0f};
    const ScrollbarDragModel reversed_range{50.0f, 10.0f, 210.0f,
                                            0.5f, 1.0f, 0.0f};
    ScrollbarDragModel non_finite{50.0f, 10.0f, 210.0f,
                                  0.5f, 0.0f, 1.0f};
    non_finite.maximum = std::numeric_limits<float>::quiet_NaN();
    CHECK(state, !ScrollbarValueForPointer(flat_track, 60.0f, &value));
    CHECK(state, !ScrollbarValueForPointer(reversed_range, 60.0f, &value));
    CHECK(state, !ScrollbarValueForPointer(non_finite, 60.0f, &value));
    CHECK(state, !ScrollbarValueForPointer(
        ScrollbarDragModel{50.0f, 10.0f, 210.0f, 2.0f, 0.0f, 1.0f},
        60.0f, &value));
    CHECK(state, !ScrollbarValueForPointer(
        ScrollbarDragModel{50.0f, 10.0f, 210.0f, 0.5f, 0.0f, 1.0f},
        std::numeric_limits<float>::infinity(), &value));
    CHECK(state, !ScrollbarValueForPointer(
        ScrollbarDragModel{50.0f, 10.0f, 210.0f, 0.5f, 0.0f, 1.0f},
        60.0f, nullptr));
    CheckClose(state, value, 123.0f);
}

void TestMissionUiPolicy(TestState& state) {
    CHECK(state, !ShouldInspectUiScrollbars(false, false, false));
    CHECK(state, ShouldInspectUiScrollbars(true, false, false));
    CHECK(state, !ShouldInspectUiScrollbars(true, true, false));
    CHECK(state, ShouldInspectUiScrollbars(true, true, true));
}

void TestConversationResponseGeometry(TestState& state) {
    CHECK(state, IsConversationResponseGeometry(
        1360.0f, 48.0f, 760.0f, 2424.0f, 1080.0f));
    CHECK(state, !IsConversationResponseGeometry(
        300.0f, 48.0f, 760.0f, 2424.0f, 1080.0f));
    CHECK(state, !IsConversationResponseGeometry(
        1360.0f, 48.0f, 300.0f, 2424.0f, 1080.0f));
    CHECK(state, !IsConversationResponseGeometry(
        1360.0f, 240.0f, 760.0f, 2424.0f, 1080.0f));
    CHECK(state, !IsConversationResponseGeometry(
        std::numeric_limits<float>::quiet_NaN(), 48.0f, 760.0f,
        2424.0f, 1080.0f));
    CheckClose(state, MirroredContentPointerY(700.0f, 400.0f), 1000.0f);
    CheckClose(state, MirroredContentPointerY(700.0f, 900.0f), 500.0f);
}

}  // namespace

int main() {
    TestState state;
    TestRelativeDragAcrossNormalizedRange(state);
    TestDragClampsAtBothEnds(state);
    TestNonUnitGuestRange(state);
    TestContentDragUsesDocumentExtent(state);
    TestContentExtentCanBeEstimatedFromVisibleRows(state);
    TestContentDragRejectsInvalidGeometry(state);
    TestContentDragUsesOnlyMoveSamples(state);
    TestRejectsInvalidGuestState(state);
    TestMissionUiPolicy(state);
    TestConversationResponseGeometry(state);
    if (state.failures != 0) {
        std::fprintf(stderr, "%d/%d checks failed\n",
                     state.failures, state.checks);
        return 1;
    }
    std::printf("%d checks passed\n", state.checks);
    return 0;
}
