// SPDX-FileCopyrightText: 2026 Sopwit <sopwith.osdev@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Effects
import QtQuick.Layouts
import Sentinel.Desktop

ComboBox {
    id: control

    property color accent: SentinelTheme.calmAccent
    property string placeholderText: ""
    property string accessibleDescription: ""
    property var delegateTextResolver: null
    property string delegateSuffix: ""
    property int popupMaxHeight: 300
    property bool popupShowsCheckmark: true

    // Accessibility contract: exposed as a combo box whose expanded state tracks the
    // popup. The usage site must supply Accessible.name (e.g. "Provider") because a
    // combo box has no intrinsic label; the placeholder is used only as a fallback.
    // Accessible.expandable/expanded only exist since Qt 6.11, so the popup state is not
    // reported on older Qt versions.
    Accessible.role: Accessible.ComboBox
    Accessible.name: control.placeholderText
    Accessible.description: control.accessibleDescription

    implicitHeight: SentinelTheme.controlHeight
    font.pixelSize: SentinelTheme.fontBody
    hoverEnabled: true
    focusPolicy: Qt.StrongFocus
    opacity: enabled ? 1.0 : InteractionTokens.disabledOpacity
    leftPadding: SentinelTheme.spaceMd
    rightPadding: SentinelTheme.spaceMd + 18
    padding: SentinelTheme.spaceSm

    contentItem: Text {
        text: control.displayText.length > 0 ? control.displayText : control.placeholderText
        color: control.displayText.length > 0 ? SentinelTheme.textPrimary : SentinelTheme.textPlaceholder
        font: control.font
        verticalAlignment: Text.AlignVCenter
        maximumLineCount: 1
        elide: Text.ElideRight

        Behavior on color {
            ColorAnimation {
                duration: MotionTokens.fast
                easing.type: MotionTokens.standard
            }
        }
    }

    background: Rectangle {
        radius: SentinelTheme.radiusMd
        color: InteractionTokens.surfaceColor(control.hovered || control.popup.visible,
                                              control.down,
                                              false,
                                              control.accent)
        border.color: InteractionTokens.borderColor(control.activeFocus,
                                                    control.hovered || control.popup.visible,
                                                    false,
                                                    control.accent)
        border.width: 1

        layer.enabled: control.popup.visible
        layer.effect: MultiEffect {
            shadowEnabled: true
            shadowColor: SentinelTheme.withAlpha(SentinelTheme.textPrimary, 0.14)
            shadowVerticalOffset: 2
            shadowBlur: 0.10
            shadowOpacity: 1.0
        }

        Behavior on color {
            ColorAnimation {
                duration: MotionTokens.fast
                easing.type: MotionTokens.standard
            }
        }

        Behavior on border.color {
            ColorAnimation {
                duration: MotionTokens.fast
                easing.type: MotionTokens.standard
            }
        }
    }

    indicator: TablerGlyph {
        x: parent.width - width - SentinelTheme.spaceMd
        y: parent.height / 2 - height / 2
        text: "chevron-right"
        rotation: control.popup.visible ? 270 : 90
        color: control.popup.visible ? control.accent : SentinelTheme.textMuted
        font.pixelSize: SentinelTheme.fontSmall

        Behavior on rotation {
            NumberAnimation {
                duration: MotionTokens.normal
                easing.type: MotionTokens.standard
            }
        }

        Behavior on color {
            ColorAnimation {
                duration: MotionTokens.fast
                easing.type: MotionTokens.standard
            }
        }
    }

    // Focus glow ring (visible on keyboard focus)
    Rectangle {
        anchors.fill: parent
        anchors.margins: -3
        radius: SentinelTheme.radiusMd + 3
        color: "transparent"
        border.color: SentinelTheme.withAlpha(control.accent, InteractionTokens.focusOpacity)
        border.width: 2
        opacity: control.activeFocus ? 1.0 : 0.0
        z: -1

        Behavior on opacity {
            NumberAnimation {
                duration: MotionTokens.fast
                easing.type: MotionTokens.standard
            }
        }
    }

    Behavior on opacity {
        NumberAnimation {
            duration: MotionTokens.fast
            easing.type: MotionTokens.standard
        }
    }

    popup: Popup {
        id: popup
        y: control.height + 6 + popupOffset
        width: Math.max(control.width, 160)
        implicitHeight: Math.min(contentItem.implicitHeight + 2 * padding, control.popupMaxHeight)
        padding: SentinelTheme.spaceXs

        property real popupOffset: 0

        enter: Transition {
            ParallelAnimation {
                NumberAnimation {
                    property: "popupOffset"
                    from: 8
                    to: 0
                    duration: MotionTokens.menu
                    easing.type: MotionTokens.enter
                }
                NumberAnimation {
                    property: "opacity"
                    from: 0.0
                    to: 1.0
                    duration: MotionTokens.menu
                    easing.type: MotionTokens.enter
                }
            }
        }

        exit: Transition {
            ParallelAnimation {
                NumberAnimation {
                    property: "popupOffset"
                    to: 6
                    duration: MotionTokens.fast
                    easing.type: MotionTokens.exit
                }
                NumberAnimation {
                    property: "opacity"
                    to: 0.0
                    duration: MotionTokens.fast
                    easing.type: MotionTokens.exit
                }
            }
        }

        background: Rectangle {
            radius: SentinelTheme.radiusLg
            color: SentinelTheme.withAlpha(SentinelTheme.backgroundRaised, 0.98)
            border.color: SentinelTheme.withAlpha(control.accent, 0.20)
            border.width: 1

            layer.enabled: true
            layer.effect: MultiEffect {
                shadowEnabled: true
                shadowColor: SentinelTheme.withAlpha(SentinelTheme.textPrimary, 0.22)
                shadowVerticalOffset: 4
                shadowBlur: 0.22
                shadowOpacity: 1.0
            }
        }

        contentItem: ListView {
            clip: true
            implicitHeight: contentHeight
            model: control.delegateModel
            currentIndex: control.highlightedIndex

            ScrollBar.vertical: ScrollBar {
                policy: ScrollBar.AsNeeded
                width: 6
                background: Rectangle { color: "transparent" }
                contentItem: Rectangle {
                    radius: 3
                    color: SentinelTheme.withAlpha(SentinelTheme.textPrimary, 0.18)
                }
            }
        }
    }

    delegate: ItemDelegate {
        required property var model
        id: delegateScope1
        required property var modelData
        required property int index
        width: popup.availableWidth
        height: 34
        highlighted: control.highlightedIndex === index
        hoverEnabled: true

        // Accessibility contract: each option is a labelled list item whose selected
        // state mirrors the current index (the default ItemDelegate exposes an empty name).
        Accessible.role: Accessible.ListItem
        Accessible.name: delegateText.text
        Accessible.selectable: true
        Accessible.selected: control.currentIndex === delegateScope1.index
        Accessible.checkable: true
        Accessible.checked: control.currentIndex === delegateScope1.index

        contentItem: RowLayout {
            spacing: SentinelTheme.spaceSm
            anchors.fill: parent
            anchors.leftMargin: SentinelTheme.spaceMd
            anchors.rightMargin: SentinelTheme.spaceMd

            Text {
                id: delegateText
                Layout.fillWidth: true
                text: {
                    var base = control.textRole ? delegateScope1.model[control.textRole] : delegateScope1.modelData
                    if (control.delegateTextResolver)
                        base = control.delegateTextResolver(base)
                    return base + control.delegateSuffix
                }
                color: (delegateScope1.highlighted || control.currentIndex === delegateScope1.index)
                       ? SentinelTheme.textPrimary
                       : SentinelTheme.textMuted
                font.family: control.font.family
                font.pixelSize: control.font.pixelSize
                font.bold: control.currentIndex === delegateScope1.index
                verticalAlignment: Text.AlignVCenter
                maximumLineCount: 1
                elide: Text.ElideRight
            }

            TablerGlyph {
                visible: control.popupShowsCheckmark && control.currentIndex === delegateScope1.index
                text: "check"
                color: control.accent
                font.pixelSize: SentinelTheme.fontSmall
                font.bold: true
            }
        }

        background: Rectangle {
            radius: SentinelTheme.radiusSm
            color: delegateScope1.highlighted
                   ? SentinelTheme.withAlpha(control.accent, 0.12)
                   : control.currentIndex === delegateScope1.index
                     ? SentinelTheme.withAlpha(control.accent, 0.07)
                     : delegateScope1.hovered
                       ? SentinelTheme.withAlpha(SentinelTheme.textPrimary, 0.04)
                       : "transparent"

            Behavior on color {
                ColorAnimation {
                    duration: MotionTokens.fast
                    easing.type: MotionTokens.standard
                }
            }
        }
    }
}
