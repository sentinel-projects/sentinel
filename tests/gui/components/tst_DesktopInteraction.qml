// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Window
import QtQuick.Controls.Basic
import QtTest
import Sentinel.Desktop

TestCase {
    id: tests
    name: "DesktopInteractionTest"
    visible: true
    when: windowShown
    width: 1000
    height: 720
    property int denials: 0
    property int approvals: 0
    property string selectedPage: "Dashboard"
    QtObject {
        id: backend
        property bool conversationRuntimeStreaming: false
        property bool localInferenceBusy: false
        property string chatSendLifecycleState: "idle"
        property bool globalErrorVisible: false
        property string globalErrorMessage: ""
        property string conversationRuntimeLastErrorSummary: "No error or refusal yet."
        property string activeRuntimeProviderLabel: "Fixture provider"
        property string activeRuntimeReadinessState: "Unavailable"
        property string localRuntimeHealth: "Unknown"
        property string localInferenceStreamStatus: "Idle"
        property string latestToolExecutionStatus: ""
    }
    Rectangle {
        id: canvas
        x: 10; y: 10; width: 900; height: 480
        color: SentinelTheme.backgroundBase
        NavigationRail {
            id: rail
            height: parent.height
            currentPage: tests.selectedPage
            connectionStatus: "Disconnected"
            runtimeStatus: backend.activeRuntimeReadinessState
            onPageRequested: function(pageName) { tests.selectedPage = pageName }
        }
        Label { x: 80; y: 24; text: "Desktop verification fixture"; color: SentinelTheme.textPrimary; font.family: uiTestFontFamily }
        RuntimeStateStrip { id: strip; x: 80; y: 80; width: parent.width - 96; viewModel: backend }
        ErrorBanner { id: error; x: 80; y: 180; width: parent.width - 96; height: implicitHeight; show: backend.globalErrorVisible; message: backend.globalErrorMessage }
        SentinelTextField { id: composer; x: 80; y: 280; width: parent.width - 120; placeholderText: "Disposable composer" }
        SentinelButton { id: openButton; x: 80; y: 350; text: "Review tool request"; onClicked: approval.open() }
    }
    SentinelOverlayModal {
        id: approval
        preferredWidth: 480
        preferredHeight: 280
        closePolicy: Popup.NoAutoClose
        initialFocusItem: deny
        contentItem: Column {
            spacing: 24
            Accessible.role: Accessible.Dialog
            Accessible.name: "Tool approval required"
            Label { text: "Tool approval required"; color: SentinelTheme.textPrimary }
            Row {
                spacing: 20
                SentinelButton { id: deny; text: "Deny"; onClicked: { tests.denials++; approval.close() } }
                SentinelButton { id: allow; text: "Allow once"; onClicked: { tests.approvals++; approval.close() } }
            }
        }
    }
    function hasText(item, text) {
        if (item.text !== undefined && item.text === text) return true
        var children = item.children || []
        for (var i = 0; i < children.length; ++i)
            if (hasText(children[i], text)) return true
        return false
    }
    function initTestCase() {
        verify(tests.Window.window)
        tests.Window.window.width = 1000
        tests.Window.window.height = 720
    }
    function init() {
        MotionTokens.reducedMotion = true
        SentinelTheme.activeTheme = "Paper"
        SentinelTheme.fontFamily = uiTestFontFamily
        denials = 0; approvals = 0; selectedPage = "Dashboard"
        backend.conversationRuntimeStreaming = false
        backend.localInferenceBusy = false
        backend.localInferenceStreamStatus = "Idle"
        backend.chatSendLifecycleState = "idle"
        backend.latestToolExecutionStatus = ""
        backend.globalErrorVisible = false
        backend.globalErrorMessage = ""
        openButton.font.pixelSize = SentinelTheme.fontControl
        canvas.width = 900
    }
    function cleanup() { approval.close(); tryCompare(approval, "visible", false) }
    function test_navigation_data() { return [{tag: "models", page: "Models"}, {tag: "inspector", page: "Inspector"}, {tag: "dashboard", page: "Dashboard"}] }
    function test_navigation(data) {
        var control = findChild(rail, data.page + "NavigationButton")
        verify(control)
        verify(control.Accessible.name.length > 0)
        control.forceActiveFocus()
        keyClick(Qt.Key_Space)
        compare(selectedPage, data.page)
        compare(control.checked, true)
    }
    function test_stream_cancel_and_denial_projection() {
        verify(hasText(strip, "Readiness: Unavailable"))
        backend.chatSendLifecycleState = "streaming"
        tryVerify(function() { return hasText(strip, "Stream: Streaming") })
        backend.chatSendLifecycleState = "cancelled"
        backend.localInferenceStreamStatus = "Cancelled"
        tryVerify(function() { return hasText(strip, "Stream: Cancelled") })
        backend.latestToolExecutionStatus = "Denied"
        tryVerify(function() { return hasText(strip, "Tools: Denied") })
        backend.globalErrorMessage = "Daemon disconnected; retry when connected"
        backend.globalErrorVisible = true
        tryVerify(function() { return hasText(strip, backend.globalErrorMessage) })
        compare(backend.activeRuntimeReadinessState, "Unavailable", "connected/streamed projections must not promote readiness")
    }
    function test_default_denial_data() { return [{tag: "return", key: Qt.Key_Return}, {tag: "enter", key: Qt.Key_Enter}, {tag: "space", key: Qt.Key_Space}] }
    function test_default_denial(data) {
        composer.forceActiveFocus()
        approval.open()
        verify(waitForRendering(approval.contentItem, 2000))
        tryCompare(deny, "activeFocus", true)
        keyClick(Qt.Key_Escape)
        compare(approval.visible, true)
        keyClick(data.key)
        compare(denials, 1)
        compare(approvals, 0)
        tryCompare(approval, "visible", false)
    }
    function test_modal_focus_trap_and_restore() {
        composer.forceActiveFocus()
        tryCompare(composer, "activeFocus", true)
        approval.open()
        verify(waitForRendering(approval.contentItem, 2000))
        tryCompare(deny, "activeFocus", true)
        for (var i = 0; i < 8; ++i) {
            keyClick(Qt.Key_Tab)
            verify(!composer.activeFocus && !openButton.activeFocus, "modal must trap focus")
            verify(deny.activeFocus || allow.activeFocus)
        }
        approval.close()
        tryCompare(composer, "activeFocus", true)
        compare(approvals, 0)
    }
    function test_explicit_allow_requires_navigation() {
        mouseClick(openButton, openButton.width / 2, openButton.height / 2)
        verify(waitForRendering(approval.contentItem, 2000))
        tryCompare(deny, "activeFocus", true)
        compare(approvals, 0)
        keyClick(Qt.Key_Tab)
        tryCompare(allow, "activeFocus", true)
        keyClick(Qt.Key_Return)
        compare(approvals, 1)
        compare(denials, 0)
    }
    function test_visual_preparation_data() {
        return [{tag: "paper-compact", theme: "Paper", width: 640, textSize: 14},
                {tag: "dark-wide", theme: "Midnight Blue", width: 900, textSize: 14},
                {tag: "large-text", theme: "Paper", width: 640, textSize: 28}]
    }
    function test_visual_preparation(data) {
        SentinelTheme.activeTheme = data.theme
        canvas.width = data.width
        openButton.font.pixelSize = data.textSize
        backend.globalErrorMessage = "Fixture service unavailable"
        backend.globalErrorVisible = true
        canvas.forceActiveFocus()
        mouseMove(tests, 0, 0)
        verify(waitForRendering(canvas, 2000))
        verify(openButton.width >= openButton.implicitWidth)
        verify(openButton.x + openButton.width <= canvas.width)
        verify(openButton.height >= openButton.contentItem.implicitHeight)
        var image = grabImage(canvas)
        compare(image.width, data.width)
        compare(image.height, 480)
        verify(image.alpha(image.width - 1, image.height - 1) > 0)
        verify(image.equals(grabImage(canvas)), "stable within-run capture, not automatic baseline acceptance")
        if (uiEvidenceDirectory.length > 0)
            image.save(uiEvidenceDirectory + "/" + data.tag + ".png")
        console.info("P605_CAPTURE " + JSON.stringify({tag: data.tag, width: data.width, height: 480, dpr: image.width / canvas.width, windowWidth: tests.Window.window.width, windowHeight: tests.Window.window.height, theme: data.theme, font: uiTestFontFamily, textSize: data.textSize, motion: "reduced", backend: "synthetic-unavailable", renderer: "software"}))
    }
}
