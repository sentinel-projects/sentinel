// SPDX-FileCopyrightText: 2026 Sopwit <sopwith.osdev@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Effects
import Sentinel.Desktop

Rectangle {
    id: panel

    radius: SentinelTheme.radiusPanel
    color: panelColor
    border.color: borderColor
    border.width: borderWidth

    // Drop shadow
    // Software scene graph cannot render MultiEffect; keep panel content visible.
    layer.enabled: GraphicsInfo.api !== GraphicsInfo.Software
    layer.smooth: false
    layer.effect: MultiEffect {
        shadowEnabled: true
        shadowColor: SentinelTheme.lightTheme
                     ? SentinelTheme.withAlpha(SentinelTheme.textPrimary, 0.10)
                     : SentinelTheme.withAlpha(SentinelTheme.textPrimary, 0.35)
        shadowVerticalOffset: SentinelTheme.lightTheme ? 2 : 4
        shadowBlur: SentinelTheme.shadowBlurPanel * 0.03
        shadowOpacity: 1.0
    }

    property color panelColor: SentinelTheme.panel
    property color borderColor: SentinelTheme.withAlpha(SentinelTheme.textPrimary, 0.070)
    property int borderWidth: 1
    property color bracketColor: SentinelTheme.withAlpha(SentinelTheme.textPrimary, 0.0)
    property bool showBrackets: false
    property int bracketSize: 12
    property color edgeLightColor: bracketColor
    property real edgeLightOpacity: 0.0
    property bool hoverDepth: false

    Behavior on color {
        ColorAnimation {
            duration: MotionTokens.normal
            easing.type: MotionTokens.standard
        }
    }

    Behavior on border.color {
        ColorAnimation {
            duration: MotionTokens.normal
            easing.type: MotionTokens.standard
        }
    }

    Behavior on opacity {
        NumberAnimation {
            duration: MotionTokens.fast
            easing.type: MotionTokens.standard
        }
    }

    Rectangle {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.leftMargin: SentinelTheme.spaceLg
        anchors.rightMargin: SentinelTheme.spaceLg
        height: 1
        color: panel.edgeLightColor
        opacity: panel.edgeLightOpacity
    }

    Rectangle {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.leftMargin: SentinelTheme.space2Xl
        anchors.rightMargin: SentinelTheme.space2Xl
        height: 1
        color: panel.edgeLightColor
        opacity: panel.edgeLightOpacity * 0.46
    }

    Rectangle {
        visible: false
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.leftMargin: SentinelTheme.spaceSm
        anchors.topMargin: SentinelTheme.spaceSm
        width: panel.bracketSize
        height: 1
        color: panel.bracketColor
        z: 20
    }

    Rectangle {
        visible: false
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.leftMargin: SentinelTheme.spaceSm
        anchors.topMargin: SentinelTheme.spaceSm
        width: 1
        height: panel.bracketSize
        color: panel.bracketColor
        z: 20
    }

    Rectangle {
        visible: false
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.rightMargin: SentinelTheme.spaceSm
        anchors.topMargin: SentinelTheme.spaceSm
        width: panel.bracketSize
        height: 1
        color: panel.bracketColor
        z: 20
    }

    Rectangle {
        visible: false
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.rightMargin: SentinelTheme.spaceSm
        anchors.topMargin: SentinelTheme.spaceSm
        width: 1
        height: panel.bracketSize
        color: panel.bracketColor
        z: 20
    }

    Rectangle {
        visible: false
        anchors.left: parent.left
        anchors.bottom: parent.bottom
        anchors.leftMargin: SentinelTheme.spaceSm
        anchors.bottomMargin: SentinelTheme.spaceSm
        width: panel.bracketSize
        height: 1
        color: panel.bracketColor
        z: 20
    }

    Rectangle {
        visible: false
        anchors.left: parent.left
        anchors.bottom: parent.bottom
        anchors.leftMargin: SentinelTheme.spaceSm
        anchors.bottomMargin: SentinelTheme.spaceSm
        width: 1
        height: panel.bracketSize
        color: panel.bracketColor
        z: 20
    }

    Rectangle {
        visible: false
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.rightMargin: SentinelTheme.spaceSm
        anchors.bottomMargin: SentinelTheme.spaceSm
        width: panel.bracketSize
        height: 1
        color: panel.bracketColor
        z: 20
    }

    Rectangle {
        visible: false
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.rightMargin: SentinelTheme.spaceSm
        anchors.bottomMargin: SentinelTheme.spaceSm
        width: 1
        height: panel.bracketSize
        color: panel.bracketColor
        z: 20
    }
}
