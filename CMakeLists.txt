cmake_minimum_required(VERSION 3.16)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# VSCode(C/C++拡張)がtarget_include_directories等の実際のビルドオプションを
# 認識できるよう、compile_commands.jsonを生成する(.vscode/c_cpp_properties.json
# から参照する)。
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

project(Tiepolo VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 有料版(Pro)ビルドかどうか。OFF(既定)なら無料版で、Pro専用コードは
# #ifdef TIEPOLO_PRO_BUILD で囲まれた部分ごとバイナリに一切含まれない。
# Pro版としてビルドするには -DTIEPOLO_PRO_BUILD=ON を付けてcmakeを実行する。
# 詳細: src/licensing/LicenseManager.h / tools/license/ を参照。
option(TIEPOLO_PRO_BUILD "Build the Pro (paid) edition with pro-only features enabled" OFF)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)
set(QT_NO_QTPATHS_DEPLOYMENT_WARNING ON)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets OpenGLWidgets Concurrent)

qt_add_resources(APP_RESOURCES
    "resources/resources.qrc"
)

set(APP_SOURCES
src/main.cpp
    src/README.md

    # Application shell and canvas integration hubs
    src/canvas/CanvasWidget.h
    src/canvas/CanvasWidget.cpp
    src/canvas/CanvasWidgetSelection.cpp
    src/canvas/CanvasWidgetActions.cpp
    src/canvas/CanvasWidgetRendering.cpp
    src/canvas/CanvasWidgetInput.cpp
    src/canvas/CanvasWidgetLayers.cpp
    src/canvas/CanvasWidgetUndo.cpp
    src/canvas/CanvasWidgetCompositing.cpp
    src/canvas/CanvasWidgetGeometry.cpp
    src/app/MainWindow.h
    src/app/MainWindowHelpers.h
    src/app/MainWindow.cpp
    src/app/MainWindowWorkspace.cpp
    src/app/MainWindowChrome.cpp
    src/app/MainWindowFiles.cpp
    src/app/MainWindowSettings.cpp
    src/app/MainWindowEvents.cpp
    src/app/MainWindowShortcuts.cpp
    src/app/MainWindowNative.cpp
    src/app/NativeWindowLog.h
    src/app/NativeWindowLog.cpp
    src/app/StartPage.h
    src/app/StartPage.cpp
    src/canvas/CanvasPane.h
    src/canvas/CanvasPane.cpp
    src/canvas/CanvasTabPage.h
    src/canvas/CanvasTabPage.cpp
    src/canvas/CanvasTabBar.h
    src/canvas/CanvasTabBar.cpp

    # キーボードショートカット/コマンド登録の基盤(ツールそのものではなく、
    # メニュー・ショートカットキーの割り当て・保存/読込を担う)
    src/shortcuts/ActionSpec.h
    src/shortcuts/ShortcutRegistry.h
    src/shortcuts/ShortcutRegistry.cpp

    src/docks/LayerDock.h
    src/docks/LayerDock.cpp
    src/docks/layers/LayerDockWidgets.h
    src/docks/ColorCircleDock.h
    src/docks/ColorCircleDock.cpp
    src/docks/ToolDock.h
    src/docks/ToolDock.cpp
    src/docks/ToolPropDock.h
    src/docks/ToolPropDock.cpp
    src/docks/ToolPresetDock.h
    src/docks/ToolPresetDock.cpp
    src/docks/BrushSizeDock.h
    src/docks/BrushSizeDock.cpp
    src/docks/NavigatorDock.h
    src/docks/NavigatorDock.cpp

    src/dialogs/SettingsDlg.h
    src/dialogs/SettingsDlg.cpp
    src/dialogs/CalibrationDlg.h
    src/dialogs/CalibrationDlg.cpp
    src/dialogs/ShortcutsDlg.h
    src/dialogs/ShortcutsDlg.cpp
    src/dialogs/NewCanvasDlg.h
    src/dialogs/NewCanvasDlg.cpp
    src/dialogs/HueSatLightPanel.h
    src/dialogs/HueSatLightPanel.cpp
    src/dialogs/BrightnessContrastPanel.h
    src/dialogs/BrightnessContrastPanel.cpp
    src/dialogs/ColorBalancePanel.h
    src/dialogs/ColorBalancePanel.cpp
    src/dialogs/ToneCurveEditor.h
    src/dialogs/ToneCurveEditor.cpp
    src/dialogs/ToneCurvePanel.h
    src/dialogs/ToneCurvePanel.cpp
    src/dialogs/DraggablePanel.h
    src/dialogs/GaussianBlurPanel.h
    src/dialogs/GaussianBlurPanel.cpp
    src/dialogs/CustomShaderPanel.h
    src/dialogs/CustomShaderPanel.cpp
    src/dialogs/MosaicPanel.h
    src/dialogs/MosaicPanel.cpp
    src/dialogs/MotionBlurPanel.h
    src/dialogs/MotionBlurPanel.cpp
    src/dialogs/NoisePanel.h
    src/dialogs/NoisePanel.cpp
    src/dialogs/CanvasSizePanel.h
    src/dialogs/CanvasSizePanel.cpp
    src/dialogs/ImageResolutionPanel.h
    src/dialogs/ImageResolutionPanel.cpp
    src/dialogs/TextLayerPanel.h
    src/dialogs/TextLayerPanel.cpp
    src/dialogs/SolidColorPickerPanel.h
    src/dialogs/SolidColorPickerPanel.cpp
    src/dialogs/ProFeatureDialog.h
    src/dialogs/ProFeatureDialog.cpp

    src/components/DockTitleBar.h
    src/components/DockTitleBar.cpp
    src/components/DockResizeController.h
    src/components/DockResizeController.cpp
    src/components/ThemeColors.h
    src/components/MultiColumnDelegate.h
    src/components/ColorWheelWidget.h
    src/components/ColorWheelWidget.cpp
    src/components/CollapsibleSection.h
    src/components/CollapsibleSection.cpp
    src/components/ImagePresetPicker.h
    src/components/ImagePresetPicker.cpp
    src/components/ToolPreview.h
    src/components/ToolPreview.cpp

    # Document model and project-level state
    src/document/CanvasDocument.h
    src/document/CanvasDocument.cpp
    src/document/CanvasSerializer.h
    src/document/CanvasSerializer.cpp
    src/document/RecentFiles.h
    src/document/RecentFiles.cpp
    # External file formats
    src/io/PsdCodec.h
    src/io/PsdCodec.cpp
    src/io/PsdDescriptor.h
    src/io/PsdDescriptor.cpp
    # Shared GPU/rendering infrastructure
    src/rendering/ShaderCache.h
    src/rendering/ShaderCache.cpp
    src/io/AbrCodec.h
    src/io/AbrCodec.cpp
    src/io/AbrPenMapping.h
    src/io/AbrPenMapping.cpp
    src/rendering/ViewTransform.h
    src/document/StrokeUndoRecorder.h
    src/document/StrokeUndoRecorder.cpp
    src/rendering/CanvasCompositor.h
    src/rendering/CanvasCompositor.cpp
    src/rendering/LayerSliceAllocator.h

    # キャンバスアクション基盤(色調整/フィルター/変形/レイヤー編集などの一回限りの
    # アクションを、CanvasWidget から切り出した CanvasAction 階層 + コントローラ)
    src/actions/CanvasActionHost.h
    src/actions/CanvasAction.h
    src/actions/CanvasAction.cpp
    src/actions/CanvasActionController.h
    src/actions/CanvasActionController.cpp
    src/actions/AdjustmentActions.h
    src/actions/AdjustmentActions.cpp
    src/actions/FilterActions.h
    src/actions/FilterActions.cpp
    src/actions/MotionBlurAction.h
    src/actions/MotionBlurAction.cpp
    src/actions/TransformActions.h
    src/actions/TransformActions.cpp
    src/actions/CanvasSizeActions.h
    src/actions/CanvasSizeActions.cpp
    src/actions/LayerEditActions.h
    src/actions/LayerEditActions.cpp

    # フィルターレイヤーの編集アクション(無料版でも使えるガウスぼかしを含むため
    # 常時コンパイル。Pro限定の色収差はファイル内で#ifdef TIEPOLO_PRO_BUILDにより
    # 分離してある。パネルは src/dialogs/GaussianBlurPanel と、Pro版のみ
    # src/dialogs/ChromaticAberrationPanel を使い回す)
    src/actions/FilterLayerEditAction.h
    src/actions/FilterLayerEditAction.cpp

    # ツール共通基盤(Tool基底クラス、ToolType列挙、設定値、レジストリ等。
    # canvas/actionsどちらのツールからも使われる)
    src/tools/core/Tool.h
    src/tools/core/ToolType.h
    src/tools/core/ToolContext.h
    src/tools/core/ToolConfig.h
    src/tools/core/ToolConfig.cpp
    src/tools/core/ToolRegistry.h
    src/tools/core/ToolRegistry.cpp
    src/tools/core/ToolPresetList.h
    src/tools/core/DispatchBounds.h
    src/tools/core/BrushShape.h
    src/tools/core/MaskBrush.h
    src/tools/core/ToolDispatchUtil.h
    src/tools/core/CursorUtils.h
    src/tools/core/CursorUtils.cpp

    # キャンバスツール(左のツールドックから選ぶ、持続する描画/操作モード)
    src/tools/canvas/PenEraserTool.h
    src/tools/canvas/PenEraserTool.cpp
    src/tools/canvas/AirbrushTool.h
    src/tools/canvas/AirbrushTool.cpp
    src/tools/canvas/FillTool.h
    src/tools/canvas/FillTool.cpp
    src/tools/canvas/DropperTool.h
    src/tools/canvas/MoveTool.h
    src/tools/canvas/RotateTool.h
    src/tools/canvas/BlurTool.h
    src/tools/canvas/BlurTool.cpp
    src/tools/canvas/WarpTool.h
    src/tools/canvas/WarpTool.cpp
    src/tools/canvas/SelectTool.h
    src/tools/canvas/SelectTool.cpp
    src/tools/canvas/TextTool.h
    src/tools/canvas/TextTool.cpp

    # 画像処理アクション(上部ツールバー/メニューから呼ぶ一回限りの処理。
    # 常設のツールモードではない)
    src/tools/actions/TransformTool.h
    src/tools/actions/TransformTool.cpp
    src/tools/actions/FreeTransformTool.h
    src/tools/actions/FreeTransformTool.cpp
    src/tools/actions/HueSatLightTool.h
    src/tools/actions/HueSatLightTool.cpp
    src/tools/actions/BrightnessContrastTool.h
    src/tools/actions/BrightnessContrastTool.cpp
    src/tools/actions/ColorBalanceTool.h
    src/tools/actions/ColorBalanceTool.cpp
    src/tools/actions/ToneCurveTool.h
    src/tools/actions/ToneCurveTool.cpp
    src/tools/actions/GaussianBlurTool.h
    src/tools/actions/GaussianBlurTool.cpp
    src/tools/actions/CustomShaderTool.h
    src/tools/actions/CustomShaderTool.cpp
    src/tools/actions/MosaicTool.h
    src/tools/actions/MosaicTool.cpp
    src/tools/actions/MotionBlurTool.h
    src/tools/actions/MotionBlurTool.cpp
    src/tools/actions/NoiseTool.h
    src/tools/actions/NoiseTool.cpp
    src/tools/actions/CanvasSizeTool.h
    src/tools/actions/CanvasSizeTool.cpp
    src/tools/actions/ImageResolutionTool.h
    src/tools/actions/ImageResolutionTool.cpp

    resources/style.qss

    platform/app.rc

    ${APP_RESOURCES}
    )

# 有料版(Pro)専用ソース: ライセンスファイルの署名検証。TIEPOLO_PRO_BUILD=OFFの
# 無料版ビルドにはこれらのファイル自体が一切コンパイル・リンクされない。
if(TIEPOLO_PRO_BUILD)
    list(APPEND APP_SOURCES
        src/licensing/LicenseManager.h
        src/licensing/LicenseManager.cpp
        src/licensing/LicensePublicKey.h

        # 色収差フィルター(Pro限定機能第1号)
        src/tools/actions/ChromaticAberrationTool.h
        src/tools/actions/ChromaticAberrationTool.cpp
        src/dialogs/ChromaticAberrationPanel.h
        src/dialogs/ChromaticAberrationPanel.cpp

        src/actions/ChromaticAberrationAction.h
        src/actions/ChromaticAberrationAction.cpp

        # レンズぼかし(玉ボケ)フィルター
        src/tools/actions/LensBlurTool.h
        src/tools/actions/LensBlurTool.cpp
        src/dialogs/LensBlurPanel.h
        src/dialogs/LensBlurPanel.cpp

        src/actions/LensBlurAction.h
        src/actions/LensBlurAction.cpp

        # グラデーションマップ(帯へ色を足していく1次元グラデーションエディタ付き)
        src/tools/actions/GradientMapTool.h
        src/tools/actions/GradientMapTool.cpp
        src/dialogs/GradientStripEditor.h
        src/dialogs/GradientStripEditor.cpp
        src/dialogs/GradientMapPanel.h
        src/dialogs/GradientMapPanel.cpp

        src/actions/GradientMapAction.h
        src/actions/GradientMapAction.cpp
    )
endif()

# Debug は WIN32 なし、Release は WIN32 あり
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_executable(${PROJECT_NAME} ${APP_SOURCES})
else()
    add_executable(${PROJECT_NAME} WIN32 ${APP_SOURCES})
endif()

# src以下のヘッダを、includeする側のディレクトリ階層に関わらず
# 常に"src"からの相対パス(例: "tools/core/Tool.h")でincludeできるようにする。
target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::OpenGLWidgets
    Qt6::Concurrent
)
target_compile_definitions(${PROJECT_NAME} PRIVATE QT_NO_DEPRECATED_WARNINGS)

if(TIEPOLO_PRO_BUILD)
    target_compile_definitions(${PROJECT_NAME} PRIVATE TIEPOLO_PRO_BUILD)
    # LicenseManager.cppがWindows CNG(bcrypt.dll)でECDSA署名検証を行うために必要。
    target_link_libraries(${PROJECT_NAME} PRIVATE bcrypt)
endif()

if(NOT VCPKG_TARGET_TRIPLET MATCHES "static")
    qt_generate_deploy_app_script(
        TARGET ${PROJECT_NAME}
        OUTPUT_SCRIPT deploy_script
        NO_TRANSLATIONS
    )
    
    get_target_property(QMAKE_EXE Qt${QT_VERSION_MAJOR}::qmake IMPORTED_LOCATION)
    get_filename_component(QT_BIN_DIR "${QMAKE_EXE}" DIRECTORY)
    set(QT_DEPLOY_TOOL "${QT_BIN_DIR}/windeployqt.exe")

    install(CODE "
        execute_process(
            COMMAND \"${QT_DEPLOY_TOOL}\" \"$<TARGET_FILE:Tiepolo>\"
        )
    ")

    qt_finalize_target(${PROJECT_NAME})
endif()
