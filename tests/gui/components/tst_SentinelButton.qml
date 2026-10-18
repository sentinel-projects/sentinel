// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Window
import QtTest
import Sentinel.Desktop

TestCase {
    id: tests
    name: "SentinelButtonTest"
    visible: true
    when: windowShown
    width: 500
    height: 300
    SentinelButton { id: button; x: 30; y: 30; text: "Save changes"; tooltipText: "Persist settings" }
    SignalSpy { id: clicked; target: button; signalName: "clicked" }
    function initTestCase() {
        verify(tests.Window.window)
        tests.Window.window.width = 500
        tests.Window.window.height = 300
    }
    function init() { button.enabled = true; clicked.clear(); MotionTokens.reducedMotion = true }
    function test_accessible_contract() {
        compare(button.Accessible.name, "Save changes")
        compare(button.Accessible.role, Accessible.Button)
        compare(button.Accessible.description, "Persist settings")
    }
    function test_keyboard_activation_data() {
        return [{tag: "return", key: Qt.Key_Return}, {tag: "enter", key: Qt.Key_Enter}, {tag: "space", key: Qt.Key_Space}]
    }
    function test_keyboard_activation(data) {
        button.forceActiveFocus()
        tryCompare(button, "activeFocus", true)
        keyClick(data.key)
        compare(clicked.count, 1, "one key press must cause exactly one action")
    }
    function test_disabled_action_is_inert() {
        button.enabled = false
        mouseClick(button)
        keyClick(Qt.Key_Return)
        compare(clicked.count, 0)
    }
}
