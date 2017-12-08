//
//  highlight.qml
//  developer/utilities/render
//
//  Olivier Prat, created on 08/08/2017.
//  Copyright 2017 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or https://www.apache.org/licenses/LICENSE-2.0.html
//
import QtQuick 2.7
import QtQuick.Controls 1.4
import QtQuick.Layouts 1.3

import "qrc:///qml/styles-uit"
import "qrc:///qml/controls-uit" as HifiControls
import  "configSlider"
import  "../lib/plotperf"
import  "highlight"

Item {
    id: root
    HifiConstants { id: hifi;}
    anchors.margins: 0
    property var listName: "contextOverlayHighlightList"
    
    property var styleList: Selection.getHighlightedListNames()

    signal sendToScript(var message);

    Component.onCompleted: {
        // Connect the signal from Selection when any selection content change and use it to refresh the current selection view
        Selection.selectedItemsListChanged.connect(resetSelectionView)
    }

    function resetSelectionView() {
        if (selectionView !== undefined) {
            selectionView.resetSelectionView();
        }
    }

    Column {
        id: col
        spacing: 5
        anchors.fill: root     
        anchors.margins: hifi.dimensions.contentMargin.x  

        Row {
            id: controlbar
            spacing: 10
            anchors.left: parent.left
            anchors.right: parent.right 
            height: 24

            HifiControls.Button {
                id: debug
                text: "Refresh"
                height: 24
                width: 82
                onClicked: {
                    print("list of highlight styles")
                    root.styleList = Selection.getHighlightedListNames()

                    print(root.styleList)
                    styleSelectorLoader.sourceComponent = undefined;
                    styleSelectorLoader.sourceComponent = selectorWidget;
                }
            }

            Loader {
                id: styleSelectorLoader
                sourceComponent: selectorWidget
                width: 300
                //anchors.right: parent.right    
            } 
            Component {
                id: selectorWidget
                HifiControls.ComboBox {
                    id: box
                    z: 999
                    editable: true
                    colorScheme: hifi.colorSchemes.dark
                    model: root.styleList
                    label: ""

                    Timer {
                        id: postpone
                        interval: 100; running: false; repeat: false
                        onTriggered: {
                            styleWidgetLoader.sourceComponent = styleWidget
                            resetSelectionView();
                        }
                    }
                    onCurrentIndexChanged: {
                        root.listName = model[currentIndex];
                        // This is a hack to be sure the widgets below properly reflect the change of category: delete the Component
                        // by setting the loader source to Null and then recreate it 100ms later
                        styleWidgetLoader.sourceComponent = undefined;
                        postpone.interval = 100
                        postpone.start()
                    }
                }
            }
        }

        Separator {}
        Loader {
            id: styleWidgetLoader
            sourceComponent: styleWidget
            anchors.left: parent.left
            anchors.right: parent.right 
            height: 240
        }   

        Separator {}

        HifiControls.CheckBox {
            text: "Highlight Hovered"
            checked: false
            onCheckedChanged: {
                if (checked) {
                    root.sendToScript("pick true")
                } else {
                    root.sendToScript("pick false")                    
                }
            } 
        }
        Separator {}
        Rectangle {
            id: selectionView
            anchors.left: parent.left
            anchors.right: parent.right 
            height: 250
           color: hifi.colors.lightGray

            function resetSelectionView() {
                myModel.clear()
                var entities = Selection.getSelectedItemsList(root.listName)["entities"]
                if (entities === undefined) {
                    return;
                }
                var fLen = entities.length;
                for (var i = 0; i < fLen; i++) {
                    myModel.append({ "objectID": JSON.stringify(entities[i]) })
                 }
            }            

            ListModel {
                id: myModel
            }
        
            Component {
                id: myDelegate
                Row {
                    id: fruit
                    Text { text: JSON.stringify(objectID) }
                }
            }
            
            ListView {
                id: selectionListView
                anchors.fill: parent
                anchors.topMargin: 30
                model: myModel
                delegate: myDelegate
            }
        }
    } 

    Component {
        id: styleWidget

        HighlightStyle {
            id: highlightStyle
            anchors.left: parent.left
            anchors.right: parent.right       
            highlightStyle: Selection.getListHighlightStyle(root.listName)

            onNewStyle: {
                var style = getStyle()
                //  print("new style " + JSON.stringify(style) )
                Selection.enableListHighlight(root.listName, style)
            }
        }
    }
}
