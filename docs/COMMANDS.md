# PSP9 command inventory

Command names observed in the bundled PspScript files (`App.Do(Environment, '<Name>', {...})`).
PSP9's Python API mirrors its internal command layer, so this is the closest thing
to a spec for what the core has to implement. Count = occurrences in scripts.

`App::do_command` (`app/src/Script.cpp`) implements 96 of the 115 names
below. The parameter names come from the scripts themselves (including the
original's own misspellings, such as `Agressive`); never invent one. What
is left needs features we do not have (Duplicate Window, Export Tube,
vector node editing) or is script-runner plumbing
(StartForeignWindow, GetString, EventNotify, the preferences queries).

| Count | Command |
|---|---|
| 30 | LayerProperties |
| 23 | SelectLayer |
| 22 | LayerDuplicate |
| 21 | Fill |
| 20 | MsgBox |
| 18 | SelectDocument |
| 17 | SelectNone |
| 11 | SelectInvert |
| 11 | NewRasterLayer |
| 10 | FileClose |
| 10 | Colorize |
| 9 | SelectAll |
| 9 | LayerMergeAll |
| 9 | IncreaseColorsTo16Million |
| 9 | GaussianBlur |
| 8 | Copy |
| 8 | AddGuide |
| 7 | LayerPromoteBackground |
| 7 | BrushStrokes |
| 7 | AddNoise |
| 6 | StartForeignWindow |
| 6 | NegativeImage |
| 6 | EnhanceEdgesMore |
| 6 | ClearSelection |
| 5 | UserDefinedFilter |
| 5 | ReturnImageInfo |
| 5 | NewLayerGroup |
| 5 | ModifySelection |
| 5 | FindEdges |
| 5 | EnableOptimizedScriptUndo |
| 5 | DropShadow |
| 4 | ShowGuides |
| 4 | SharpenMore |
| 4 | SelectLoadAlpha |
| 4 | SelectContract |
| 4 | ReturnVectorObjectProperties |
| 4 | ReturnLayerProperties |
| 4 | PasteAsNewLayer |
| 4 | NodeEditAddPath |
| 4 | NewFile |
| 4 | LayerArrange |
| 4 | Dilate |
| 4 | AddBorders |
| 3 | TextEx |
| 3 | SelectSaveAlpha |
| 3 | Selection |
| 3 | Resize |
| 3 | LayerMergeVisible |
| 3 | EdgePreservingSmooth |
| 3 | ColorAdjustHSL |
| 2 | SplitToRGB |
| 2 | SelectTool |
| 2 | SelectPromote |
| 2 | SavePalette |
| 2 | ResizeCanvas |
| 2 | PasteIntoSelection |
| 2 | NewVectorLayer |
| 2 | MaskShowAll |
| 2 | JPEGArtifactRemoval |
| 2 | HistogramEqualize |
| 2 | GridGuideSnapProperties |
| 2 | GlowingEdges |
| 2 | GenPreferences |
| 2 | DuplicateWindow |
| 2 | DecreaseColorsToX |
| 2 | ConvertToPath |
| 2 | ColoredEdges |
| 2 | ColorAdjustHueMap |
| 2 | ColorAdjustCurves |
| 2 | ColorAdjustBrightnessContrast |
| 2 | BlurAverage |
| 2 | AutoSaturationEnhancement |
| 2 | AutoContrastEnhancement |
| 2 | AddGroupsAndObjects |
| 1 | Wave |
| 1 | SplitToHSL |
| 1 | SplitToCMYK |
| 1 | ShowGrid |
| 1 | Sharpen |
| 1 | SelectSmooth |
| 1 | SelectPreviousTool |
| 1 | SelectFeather |
| 1 | SelectExpand |
| 1 | SaltAndPepper |
| 1 | ReturnGeneralPreferences |
| 1 | ReturnFileLocations |
| 1 | Posterize |
| 1 | PasteGraphicAsNewImage |
| 1 | PasteAsNewSelection |
| 1 | PaintBrush |
| 1 | NewAdjustmentLayerColorBalance |
| 1 | MoveSelection |
| 1 | Mover |
| 1 | MotionBlur |
| 1 | LayerSetVisibility |
| 1 | LayerConvertToRaster |
| 1 | LayerArrangeMoveIn |
| 1 | InnerBevel |
| 1 | HistogramAdjustment |
| 1 | Greyscale |
| 1 | GetString |
| 1 | GetMaterial |
| 1 | FloatSelection |
| 1 | FileSaveAs |
| 1 | FileOpen |
| 1 | FileLocations |
| 1 | ExportTube |
| 1 | EventNotify |
| 1 | Emboss |
| 1 | DigitalCameraNoiseRemoval |
| 1 | DecreaseColorsTo256 |
| 1 | CountImageColors |
| 1 | CombineRGB |
| 1 | ColorAdjustChannelMixer |
| 1 | Buttonize |

## Original module layout (from the install directory)

| Original DLL family | Role | Firn location |
|---|---|---|
| JascCmd{Artistic,Bevels,Color,Geometry,Layers,Lighting,Photo,Selections,Texture,Vector,...} | Command categories | core/src/commands/ |
| JascTool{Paint,Select,Text,Warp,Object,Standard} | Interactive canvas tools | app/src/tools/ |
| JascLayerPalette, JascHistoryPalette, JascMaterialPalette | Dockable palettes | app/src/ui/ |
| JascFileFormats + *.FLT + ig*13d.dll (ImageGear) | Codecs incl. .PspImage | core/src/io/ |
| JascColorMgr, JascCMYK | Color management | core (later, LittleCMS) |
| JascRender | Compositing / display | core/src/document.cpp composite() |
| JascCommandBase, JascHistoryPalette | Undo stack | core/include/firn/commands.h |
| JascCmdPyScript, Python Libraries | Scripting | out of scope for now |
