// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Window
import QtTest
import Sentinel.Desktop

TestCase {
    id: tests
    name: "CommandPaletteTest"
    visible: true
    when: windowShown
    width: 1000
    height: 720
    QtObject {
        id: backend
        property string currentModeName: "Sentinel"
        property bool notificationCenterVisible: false
    }
    SentinelTextField { id: origin; x: 30; y: 30; placeholderText: "Composer" }
    CommandPalette { id: palette; viewModel: backend }
    SignalSpy { id: requested; target: palette; signalName: "openSettingsRequested" }
    function initTestCase() {
        verify(tests.Window.window)
        tests.Window.window.width = 1000
        tests.Window.window.height = 720
    }
    function init() {
        requested.clear()
        MotionTokens.reducedMotion = false
        origin.forceActiveFocus()
        palette.openPalette()
        tryCompare(palette, "opened", true)
    }
    function cleanup() { palette.close(); tryCompare(palette, "visible", false) }
    function typeText(text) { for (var i = 0; i < text.length; ++i) keyClick(text[i]) }
    function test_keyboard_filter_and_settings() {
        typeText("Open Settings")
        tryCompare(palette, "query", "Open Settings")
        compare(palette.filteredActions.length, 1)
        keyClick(Qt.Key_Return)
        tryCompare(requested, "count", 1)
        compare(requested.signalArguments[0][0], "Interface")
        tryCompare(palette, "visible", false)
    }
    function test_unimplemented_search_cannot_activate() {
        typeText("Universal Search")
        tryCompare(palette, "query", "Universal Search")
        compare(palette.filteredActions.length, 1)
        compare(palette.filteredActions[0].enabled, false)
        keyClick(Qt.Key_Return)
        compare(requested.count, 0)
        compare(palette.visible, true)
    }
    function test_empty_filter_and_escape_restores_focus() {
        typeText("zzzz-no-command")
        tryCompare(palette, "filteredActions", [])
        keyClick(Qt.Key_Return)
        compare(palette.visible, true)
        keyClick(Qt.Key_Escape)
        tryCompare(palette, "visible", false)
        tryCompare(origin, "activeFocus", true)
    }
    function test_reduced_motion_reopen_resets_query_and_focus() {
        typeText("old query")
        palette.close()
        tryCompare(palette, "visible", false)
        MotionTokens.reducedMotion = true
        palette.openPalette()
        tryCompare(palette, "query", "")
        typeText("Open Settings")
        tryCompare(palette, "query", "Open Settings")
        keyClick(Qt.Key_Return)
        tryCompare(requested, "count", 1)
    }
}
