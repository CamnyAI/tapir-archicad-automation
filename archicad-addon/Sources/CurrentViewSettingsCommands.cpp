#include "CurrentViewSettingsCommands.hpp"
#include "MigrationHelper.hpp"

// The model view options and the pen table in effect in the current window. Each model
// window keeps its own: the floor plan (one for every story), the 3D window, each section
// and elevation. A saved view stores a copy of them, which SetViewSettings changes and
// ChangeWindow with a navigatorItemId applies.

#ifdef ServerMainVers_2700

namespace {

struct ModelViewEnumSetting {
    const char*                     key;
    char API_ModelViewOptions::*    field;
    Int32                           firstValue;
    const char* const*              names;
    UInt32                          nameCount;
};

struct ModelViewBoolSetting {
    const char*                     key;
    bool API_ModelViewOptions::*    field;
};

// The names follow the API constants, in their order: API_Hole_ShowWithDim .. API_Hole_HideHole,
// API_Section_Marker_Normal .. API_Section_Marker_AsInSettings, API_Beam_Drawing_Full ..
// API_Beam_Drawing_Contour, API_RoofShellShow_AllDetails .. API_RoofShellShow_ContourDrawing
// and API_Slab_ShowCommonEdges .. API_Slab_HiddenCommonEdges.
const char* const OpeningDisplayNames[]   = { "ShowWithDimensions", "ShowOnPlan", "HideOnPlan", "ReflectedCeiling", "HideOpening" };
const char* const SectionMarkerNames[]    = { "Normal", "KeyPlan", "AsInSettings" };
const char* const BeamDisplayNames[]      = { "Full", "ReferenceLine", "Contour" };
const char* const RoofShellDisplayNames[] = { "AllDetails", "TopSurface", "Contour" };
const char* const SlabCommonEdgeNames[]   = { "ShowCommonEdges", "EliminateCommonEdges", "HiddenCommonEdges" };

const ModelViewEnumSetting ModelViewEnumSettings[] = {
    { "doorDisplay",      &API_ModelViewOptions::doorMode,      API_Hole_ShowWithDim,         OpeningDisplayNames,   5 },
    { "windowDisplay",    &API_ModelViewOptions::windowMode,    API_Hole_ShowWithDim,         OpeningDisplayNames,   5 },
    { "skylightDisplay",  &API_ModelViewOptions::skylightMode,  API_Hole_ShowWithDim,         OpeningDisplayNames,   5 },
    { "sectionMarkers",   &API_ModelViewOptions::sectionMarker, API_Section_Marker_Normal,    SectionMarkerNames,    3 },
    { "beamDisplay",      &API_ModelViewOptions::beamMethod,    API_Beam_Drawing_Full,        BeamDisplayNames,      3 },
    { "roofShellDisplay", &API_ModelViewOptions::roofShellMode, API_RoofShellShow_AllDetails, RoofShellDisplayNames, 3 },
    { "slabCommonEdges",  &API_ModelViewOptions::slabMethod,    API_Slab_ShowCommonEdges,     SlabCommonEdgeNames,   3 }
};

const ModelViewBoolSetting ModelViewBoolSettings[] = {
    { "columnHiddenLinesUnderSlabs", &API_ModelViewOptions::coluHiddenlineUnderSlabs },
    { "beamHiddenLinesUnderSlabs",   &API_ModelViewOptions::beamHiddenlineUnderSlabs },
    { "hideZoneStamps",              &API_ModelViewOptions::zoneHideZoneStamp }
};

GS::ObjectState ModelViewOptionsToSettings (const API_ModelViewOptions& modelViewOptions)
{
    GS::ObjectState settings;
    for (const ModelViewEnumSetting& setting : ModelViewEnumSettings) {
        const Int32 offset = static_cast<Int32> (modelViewOptions.*(setting.field)) - setting.firstValue;
        if (offset >= 0 && offset < static_cast<Int32> (setting.nameCount)) {
            settings.Add (setting.key, GS::UniString (setting.names[offset]));
        }
    }
    settings.Add ("showColumnSymbols", modelViewOptions.showColumnSymbol == API_Column_Symbol_Show);
    for (const ModelViewBoolSetting& setting : ModelViewBoolSettings) {
        settings.Add (setting.key, modelViewOptions.*(setting.field));
    }
    return settings;
}

bool ApplySettingsToModelViewOptions (const GS::ObjectState& settings, API_ModelViewOptions& modelViewOptions, GS::UniString& failure)
{
    for (const ModelViewEnumSetting& setting : ModelViewEnumSettings) {
        GS::UniString value;
        if (!settings.Get (setting.key, value)) {
            continue;
        }
        bool found = false;
        for (UInt32 i = 0; i < setting.nameCount && !found; ++i) {
            if (value == setting.names[i]) {
                modelViewOptions.*(setting.field) = static_cast<char> (setting.firstValue + static_cast<Int32> (i));
                found = true;
            }
        }
        if (!found) {
            failure = GS::UniString ("Invalid value '") + value + "' of " + setting.key + ".";
            return false;
        }
    }
    bool showColumnSymbols = false;
    if (settings.Get ("showColumnSymbols", showColumnSymbols)) {
        modelViewOptions.showColumnSymbol = showColumnSymbols ? API_Column_Symbol_Show : API_Column_Symbol_Hide;
    }
    for (const ModelViewBoolSetting& setting : ModelViewBoolSettings) {
        bool value = false;
        if (settings.Get (setting.key, value)) {
            modelViewOptions.*(setting.field) = value;
        }
    }
    return true;
}

GS::UniString ModelViewOptionSettingsSchema ()
{
    return R"({
            "type": "object",
            "description": "Settings of the Model View Options dialog (Document > Model View > Model View Options) that are not options of GDL objects.",
            "properties": {
                "doorDisplay": {
                    "type": "string",
                    "description": "How doors are shown.",
                    "enum": ["ShowWithDimensions", "ShowOnPlan", "HideOnPlan", "ReflectedCeiling", "HideOpening"]
                },
                "windowDisplay": {
                    "type": "string",
                    "description": "How windows are shown.",
                    "enum": ["ShowWithDimensions", "ShowOnPlan", "HideOnPlan", "ReflectedCeiling", "HideOpening"]
                },
                "skylightDisplay": {
                    "type": "string",
                    "description": "How skylights are shown.",
                    "enum": ["ShowWithDimensions", "ShowOnPlan", "HideOnPlan", "ReflectedCeiling", "HideOpening"]
                },
                "sectionMarkers": {
                    "type": "string",
                    "description": "How section markers are shown on the floor plan.",
                    "enum": ["Normal", "KeyPlan", "AsInSettings"]
                },
                "beamDisplay": {
                    "type": "string",
                    "description": "How beams are drawn on the floor plan.",
                    "enum": ["Full", "ReferenceLine", "Contour"]
                },
                "roofShellDisplay": {
                    "type": "string",
                    "description": "How roofs and shells are drawn on the floor plan.",
                    "enum": ["AllDetails", "TopSurface", "Contour"]
                },
                "slabCommonEdges": {
                    "type": "string",
                    "description": "How the common edges of adjoining slabs with the same top surface and top height are drawn.",
                    "enum": ["ShowCommonEdges", "EliminateCommonEdges", "HiddenCommonEdges"]
                },
                "showColumnSymbols": {
                    "type": "boolean",
                    "description": "Whether column symbols are shown on the floor plan."
                },
                "columnHiddenLinesUnderSlabs": {
                    "type": "boolean",
                    "description": "Whether the parts of columns under slabs are drawn with hidden lines."
                },
                "beamHiddenLinesUnderSlabs": {
                    "type": "boolean",
                    "description": "Whether the parts of beams under slabs are drawn with hidden lines."
                },
                "hideZoneStamps": {
                    "type": "boolean",
                    "description": "Whether zone stamps are hidden."
                }
            },
            "additionalProperties": false
        })";
}

// Model view options and pen tables belong to model windows; layouts, schedules and the
// other windows have none (Archicad reports values for them that mean nothing).
GSErrCode GetCurrentModelWindow (API_WindowInfo& window, GS::UniString& failure)
{
    window = {};
    GSErrCode err = ACAPI_Window_GetCurrentWindow (&window);
    if (err != NoError) {
        failure = "Failed to get the current window.";
        return err;
    }
    switch (window.typeID) {
        case APIWind_FloorPlanID:
        case APIWind_SectionID:
        case APIWind_ElevationID:
        case APIWind_InteriorElevationID:
        case APIWind_DetailID:
        case APIWind_WorksheetID:
        case APIWind_DocumentFrom3DID:
        case APIWind_3DModelID:
            return NoError;
        default:
            failure = "The current window must be a model window: a floor plan, section, elevation, interior elevation, detail, worksheet, 3D document or the 3D window.";
            return APIERR_BADWINDOW;
    }
}

// Reads the plain settings and the name of a model view option combination. The GDL object
// options of the combination are not kept.
GSErrCode GetModelViewOptionCombination (Int32 index, API_ModelViewOptions& modelViewOptions, GS::UniString& name)
{
    API_ModelViewOptionsType combination = {};
    combination.head.index = index;
    combination.head.uniStringNamePtr = &name;
    const GSErrCode err = ACAPI_Navigator_ModelViewOptions_Get (&combination);
    ACAPI_FreeGDLModelViewOptionsPtr (&combination.modelViewOpt.gdlOptions);
    modelViewOptions = combination.modelViewOpt;
    return err;
}

// ACAPI_Navigator_ModelViewOptions_Search finds no combination by its Unicode name in
// Archicad 30, and the combinations carry no guid there, so they are walked through by index
// (deleted combinations leave gaps among the indices) and compared by name. The indices are
// those of the moment: while the current window has custom settings, Archicad lists them as an
// unnamed entry in front and numbers every combination one higher than while it uses a
// combination, so a combination is only ever looked up by name right before it is used.
GSErrCode FindModelViewOptionCombinationByName (const GS::UniString& name, Int32& index)
{
    // The unnamed entry is the window's own custom settings, not a combination.
    if (name.IsEmpty ()) {
        return APIERR_BADNAME;
    }
    UInt32 count = 0;
    ACAPI_Navigator_ModelViewOptions_GetNum (count);
    for (Int32 i = 1; i <= static_cast<Int32> (count); ++i) {
        GS::UniString combinationName;
        API_ModelViewOptions modelViewOptions = {};
        if (GetModelViewOptionCombination (i, modelViewOptions, combinationName) == NoError && combinationName == name) {
            index = i;
            return NoError;
        }
    }
    return APIERR_BADNAME;
}

// Compares the settings in effect with those of a combination, without the GDL object options.
bool IsSameModelViewOptions (API_ModelViewOptions first, API_ModelViewOptions second)
{
    first.gdlOptions = nullptr;
    second.gdlOptions = nullptr;
    return memcmp (&first, &second, sizeof (API_ModelViewOptions)) == 0;
}

// Switches the current window to the combination with the given name, and checks that the
// window reports that combination and its settings afterwards.
GSErrCode SwitchToModelViewOptionCombination (const GS::UniString& name)
{
    Int32 index = 0;
    GSErrCode err = FindModelViewOptionCombinationByName (name, index);
    if (err != NoError) {
        return err;
    }

    // Only the index is passed: with settings and an index together, Archicad reports a
    // combination index that does not belong to the settings applied.
    short indexToApply = static_cast<short> (index);
    err = ACAPI_Navigator_ChangeViewOptions (nullptr, &indexToApply);
    if (err != NoError) {
        return err;
    }

    API_ViewOptions viewOptions = {};
    short currentIndex = 0;
    err = ACAPI_Navigator_GetViewOptions (&viewOptions, &currentIndex);
    ACAPI_FreeGDLModelViewOptionsPtr (&viewOptions.modelViewOpt.gdlOptions);
    if (err != NoError) {
        return err;
    }

    GS::UniString currentName;
    API_ModelViewOptions combinationOptions = {};
    if (currentIndex <= 0 || GetModelViewOptionCombination (currentIndex, combinationOptions, currentName) != NoError ||
        currentName != name || !IsSameModelViewOptions (viewOptions.modelViewOpt, combinationOptions)) {
        return APIERR_GENERAL;
    }
    return NoError;
}

} // namespace

#endif

GetCurrentModelViewOptionCommand::GetCurrentModelViewOptionCommand () :
    CommandBase (CommonSchema::NotUsed)
{}

GS::String GetCurrentModelViewOptionCommand::GetName () const
{
    return "GetCurrentModelViewOption";
}

GS::Optional<GS::UniString> GetCurrentModelViewOptionCommand::GetRawResponseSchema () const
{
#ifdef ServerMainVers_2700
    return GS::UniString (R"({
        "type": "object",
        "description": "The model view options in effect in the current window.",
        "properties": {
            "index": {
                "type": "integer",
                "description": "The index Archicad reports for the model view option combination the current window uses. 0 when the window uses custom settings, for example after SetCurrentModelViewOption changed single settings. Archicad numbers the combinations differently while the window uses custom settings, so use the name to switch back to a combination."
            },
            "name": {
                "type": "string",
                "description": "The name of that combination. Missing when the index is 0."
            },
            "matchesCombination": {
                "type": "boolean",
                "description": "Whether the settings in effect, apart from the options of GDL objects, are still those of that combination. Missing when the index is 0."
            },
            "settings": )") + ModelViewOptionSettingsSchema () + R"(
        },
        "additionalProperties": false,
        "required": [
            "index",
            "settings"
        ]
    })";
#else
    return R"({
        "type": "object"
    })";
#endif
}

GS::ObjectState GetCurrentModelViewOptionCommand::Execute (const GS::ObjectState& /*parameters*/, GS::ProcessControl& /*processControl*/) const
{
#ifdef ServerMainVers_2700
    API_WindowInfo window = {};
    GS::UniString failure;
    GSErrCode err = GetCurrentModelWindow (window, failure);
    if (err != NoError) {
        return CreateErrorResponse (err, failure);
    }

    API_ViewOptions viewOptions = {};
    short combinationIndex = 0;
    err = ACAPI_Navigator_GetViewOptions (&viewOptions, &combinationIndex);
    ACAPI_FreeGDLModelViewOptionsPtr (&viewOptions.modelViewOpt.gdlOptions);
    if (err != NoError) {
        return CreateErrorResponse (err, "Failed to get the model view options of the current window.");
    }

    GS::ObjectState response;
    response.Add ("index", static_cast<Int32> (combinationIndex));

    GS::UniString name;
    API_ModelViewOptions combinationOptions = {};
    if (combinationIndex > 0 && GetModelViewOptionCombination (combinationIndex, combinationOptions, name) == NoError) {
        response.Add ("name", name);
        response.Add ("matchesCombination", IsSameModelViewOptions (viewOptions.modelViewOpt, combinationOptions));
    }

    response.Add ("settings", ModelViewOptionsToSettings (viewOptions.modelViewOpt));
    return response;
#else
    return CreateErrorResponse (APIERR_NOTSUPPORTED, "Available from Archicad 27.");
#endif
}

SetCurrentModelViewOptionCommand::SetCurrentModelViewOptionCommand () :
    CommandBase (CommonSchema::Used)
{}

GS::String SetCurrentModelViewOptionCommand::GetName () const
{
    return "SetCurrentModelViewOption";
}

GS::Optional<GS::UniString> SetCurrentModelViewOptionCommand::GetInputParametersSchema () const
{
#ifdef ServerMainVers_2700
    return GS::UniString (R"({
        "type": "object",
        "description": "The model view option combination to switch the current window to, given by name, and/or single settings to change. With both, the combination is applied first and the settings on top of it.",
        "properties": {
            "name": {
                "type": "string",
                "description": "The name of the model view option combination, as GetModelViewOptions lists it and GetCurrentModelViewOption returns it."
            },
            "settings": )") + ModelViewOptionSettingsSchema () + R"(
        },
        "additionalProperties": false,
        "minProperties": 1
    })";
#else
    return R"({
        "type": "object"
    })";
#endif
}

GS::Optional<GS::UniString> SetCurrentModelViewOptionCommand::GetRawResponseSchema () const
{
    return R"({
        "$ref": "#/ExecutionResult"
    })";
}

GS::ObjectState SetCurrentModelViewOptionCommand::Execute (const GS::ObjectState& parameters, GS::ProcessControl& /*processControl*/) const
{
#ifdef ServerMainVers_2700
    GS::UniString name;
    const bool hasName = parameters.Get ("name", name);
    const GS::ObjectState* settingsOS = parameters.Get ("settings");
    if (!hasName && settingsOS == nullptr) {
        return CreateFailedExecutionResult (APIERR_BADPARS, "Give name, settings or both.");
    }

    API_WindowInfo window = {};
    GS::UniString failure;
    GSErrCode err = GetCurrentModelWindow (window, failure);
    if (err != NoError) {
        return CreateFailedExecutionResult (err, failure);
    }

    if (hasName) {
        err = SwitchToModelViewOptionCombination (name);
        if (err == APIERR_BADNAME) {
            return CreateFailedExecutionResult (err, "No model view option combination is named '" + name + "'.");
        }
        if (err != NoError) {
            return CreateFailedExecutionResult (err, "Failed to switch the current window to the model view option combination '" + name + "'.");
        }
    }

    if (settingsOS != nullptr) {
        API_ViewOptions viewOptions = {};
        short currentIndex = 0;
        err = ACAPI_Navigator_GetViewOptions (&viewOptions, &currentIndex);
        if (err == NoError && !ApplySettingsToModelViewOptions (*settingsOS, viewOptions.modelViewOpt, failure)) {
            ACAPI_FreeGDLModelViewOptionsPtr (&viewOptions.modelViewOpt.gdlOptions);
            return CreateFailedExecutionResult (APIERR_BADPARS, failure);
        }
        if (err == NoError) {
            err = ACAPI_Navigator_ChangeViewOptions (&viewOptions, nullptr);
        }
        ACAPI_FreeGDLModelViewOptionsPtr (&viewOptions.modelViewOpt.gdlOptions);
        if (err != NoError) {
            return CreateFailedExecutionResult (err, "Failed to change the model view options of the current window.");
        }
    }

    return CreateSuccessfulExecutionResult ();
#else
    (void) parameters;
    return CreateFailedExecutionResult (APIERR_NOTSUPPORTED, "Available from Archicad 27.");
#endif
}

#ifdef ServerMainVers_2700
namespace {

bool IsProjectMapItemOfWindow (const API_NavigatorItem& item, const API_WindowInfo& window, short actStory, bool isPerspective)
{
    switch (window.typeID) {
        case APIWind_FloorPlanID:           return item.itemType == API_StoryNavItem && item.floorNum == actStory;
        case APIWind_3DModelID:             return item.itemType == (isPerspective ? API_PerspectiveNavItem : API_AxonometryNavItem);
        case APIWind_SectionID:             if (item.itemType != API_SectionNavItem)           return false; break;
        case APIWind_ElevationID:           if (item.itemType != API_ElevationNavItem)         return false; break;
        case APIWind_InteriorElevationID:   if (item.itemType != API_InteriorElevationNavItem) return false; break;
        case APIWind_DetailID:              if (item.itemType != API_DetailDrawingNavItem)     return false; break;
        case APIWind_WorksheetID:           if (item.itemType != API_WorksheetDrawingNavItem)  return false; break;
        case APIWind_DocumentFrom3DID:      if (item.itemType != API_DocumentFrom3DNavItem)    return false; break;
        default:                            return false;
    }
    return item.db.databaseUnId == window.databaseUnId;
}

bool FindProjectMapItemInTree (const API_NavigatorItem& parent, const API_WindowInfo& window, short actStory, bool isPerspective, API_NavigatorItem& found)
{
    API_NavigatorItem parentItem = parent;
    parentItem.mapId = API_ProjectMap;
    GS::Array<API_NavigatorItem> children;
    if (ACAPI_Navigator_GetNavigatorChildrenItems (&parentItem, &children) != NoError) {
        return false;
    }
    for (const API_NavigatorItem& child : children) {
        if (IsProjectMapItemOfWindow (child, window, actStory, isPerspective)) {
            found = child;
            return true;
        }
        if (FindProjectMapItemInTree (child, window, actStory, isPerspective, found)) {
            return true;
        }
    }
    return false;
}

// The Project Map item (story, section, the generic perspective or axonometry, ...) the
// current window shows.
GSErrCode FindProjectMapItemOfWindow (const API_WindowInfo& window, API_NavigatorItem& found)
{
    short actStory = 0;
    API_StoryInfo storyInfo = {};
    if (ACAPI_ProjectSetting_GetStorySettings (&storyInfo) == NoError) {
        actStory = storyInfo.actStory;
    }
    BMKillHandle (reinterpret_cast<GSHandle*> (&storyInfo.data));

    API_3DProjectionInfo projectionInfo = {};
    const bool isPerspective = window.typeID == APIWind_3DModelID && ACAPI_View_Get3DProjectionSets (&projectionInfo) == NoError && projectionInfo.isPersp;

    API_NavigatorSet projectMap = {};
    projectMap.mapId = API_ProjectMap;
    Int32 setIndex = 0;
    GSErrCode err = ACAPI_Navigator_GetNavigatorSet (&projectMap, &setIndex);
    if (err != NoError) {
        return err;
    }
    API_NavigatorItem root = {};
    err = ACAPI_Navigator_GetNavigatorItem (&projectMap.rootGuid, &root);
    if (err != NoError) {
        return err;
    }
    return FindProjectMapItemInTree (root, window, actStory, isPerspective, found) ? NoError : APIERR_BADID;
}

void DisposeNavigatorView (API_NavigatorView& view)
{
    if (view.modelViewOpt != nullptr) {
        ACAPI_FreeGDLModelViewOptionsPtr (&view.modelViewOpt->gdlOptions);
        BMKillPtr (reinterpret_cast<GSPtr*> (&view.modelViewOpt));
    }
    delete view.layerStats;
    view.layerStats = nullptr;
    delete view.pens;
    view.pens = nullptr;
    if (view.dimPrefs != nullptr) {
        BMKillPtr (reinterpret_cast<GSPtr*> (&view.dimPrefs));
    }
}

// The API has no function that puts a pen table into effect (ACAPI_Attribute_GetCurrPenSet
// only reads it, and ACAPI_Attribute_Modify refuses a changed inEffectForModel flag). Opening
// a view applies the pen table the view stores, so a temporary View Map view of the window's
// Project Map item is made, with the window's current settings and the given pen table, opened
// and deleted again, together with the temporary folder it is made in.
GSErrCode ApplyPenTableThroughTemporaryView (const API_WindowInfo& window, const GS::UniString& penTableName, GS::UniString& failure)
{
    API_NavigatorItem source = {};
    GSErrCode err = FindProjectMapItemOfWindow (window, source);
    if (err != NoError) {
        failure = "The Project Map item of the current window was not found.";
        return err;
    }

    API_NavigatorItem folderItem = {};
    folderItem.itemType   = API_FolderNavItem;
    folderItem.mapId      = API_PublicViewMap;
    folderItem.customName = true;
    GS::ucscpy (folderItem.uName, GS::UniString ("Temporary view for SetCurrentPenTable").ToUStr ());
    err = ACAPI_Navigator_NewNavigatorView (&folderItem, nullptr, nullptr, nullptr);
    if (err != NoError) {
        failure = "Failed to create a temporary View Map folder. In a Teamwork project the View Map must be reserved.";
        return err;
    }

    API_Guid viewGuid = APINULLGuid;
    err = ACAPI_Navigator_CloneProjectMapItemToViewMap (&source.guid, &folderItem.guid, &viewGuid);
    if (err != NoError) {
        failure = "Failed to create a temporary view of the current window.";
    } else {
        API_NavigatorItem viewItem = {};
        err = ACAPI_Navigator_GetNavigatorItem (&viewGuid, &viewItem);
        viewItem.mapId = API_PublicViewMap;
        API_NavigatorView view = {};
        if (err == NoError) {
            err = ACAPI_Navigator_GetNavigatorView (&viewItem, &view);
        }
        if (err == NoError) {
            CHTruncate (penTableName.ToCStr ().Get (), view.penSetName, sizeof (view.penSetName));
            view.savePenSet = true;
            err = ACAPI_Navigator_ChangeNavigatorView (&viewItem, &view);
        }
        DisposeNavigatorView (view);
        if (err == NoError) {
            err = ACAPI_View_GoToView (APIGuidToString (viewGuid).ToCStr ().Get ());
        }
        if (err != NoError) {
            failure = "Failed to open the temporary view with the pen table.";
        }
        bool silentMode = true;
        ACAPI_Navigator_DeleteNavigatorView (&viewGuid, &silentMode);
    }

    bool silentMode = true;
    ACAPI_Navigator_DeleteNavigatorView (&folderItem.guid, &silentMode);
    return err;
}

} // namespace

GSErrCode FindProjectMapItemOfCurrentWindow (const API_WindowInfo& window, API_NavigatorItem& item)
{
    return FindProjectMapItemOfWindow (window, item);
}
#endif

SetCurrentPenTableCommand::SetCurrentPenTableCommand () :
    CommandBase (CommonSchema::Used)
{}

GS::String SetCurrentPenTableCommand::GetName () const
{
    return "SetCurrentPenTable";
}

GS::Optional<GS::UniString> SetCurrentPenTableCommand::GetInputParametersSchema () const
{
    return R"({
        "type": "object",
        "description": "The Pen Table attribute to put into effect in the current window, given by attribute id or by name.",
        "properties": {
            "attributeId": {
                "$ref": "#/AttributeId"
            },
            "name": {
                "type": "string",
                "description": "The name of the Pen Table attribute, as GetAttributesByType lists it."
            }
        },
        "additionalProperties": false,
        "oneOf": [
            {
                "required": [
                    "attributeId"
                ]
            },
            {
                "required": [
                    "name"
                ]
            }
        ]
    })";
}

GS::Optional<GS::UniString> SetCurrentPenTableCommand::GetRawResponseSchema () const
{
    return R"({
        "$ref": "#/ExecutionResult"
    })";
}

GS::ObjectState SetCurrentPenTableCommand::Execute (const GS::ObjectState& parameters, GS::ProcessControl& /*processControl*/) const
{
#ifdef ServerMainVers_2700
    GS::UniString name;
    API_Attribute penTable = {};
    penTable.header.typeID = API_PenTableID;
    penTable.header.uniStringNamePtr = &name;

    const GS::ObjectState* attributeIdOS = parameters.Get ("attributeId");
    GS::UniString nameToFind;
    if (attributeIdOS != nullptr) {
        penTable.header.guid = GetGuidFromObjectState (*attributeIdOS);
        if (penTable.header.guid == APINULLGuid) {
            return CreateFailedExecutionResult (APIERR_BADPARS, "attributeId is corrupt or missing.");
        }
    } else if (parameters.Get ("name", nameToFind)) {
        GS::Array<API_Attribute> penTables;
        ACAPI_Attribute_GetAttributesByType (API_PenTableID, penTables);
        for (API_Attribute& candidate : penTables) {
            GS::UniString candidateName;
            API_Attribute named = {};
            named.header.typeID = API_PenTableID;
            named.header.index = candidate.header.index;
            named.header.uniStringNamePtr = &candidateName;
            if (penTable.header.guid == APINULLGuid && ACAPI_Attribute_Get (&named) == NoError && candidateName == nameToFind) {
                penTable.header.guid = named.header.guid;
            }
            DisposeAttribute (candidate);
        }
        if (penTable.header.guid == APINULLGuid) {
            return CreateFailedExecutionResult (APIERR_BADNAME, "No Pen Table is named '" + nameToFind + "'.");
        }
    } else {
        return CreateFailedExecutionResult (APIERR_BADPARS, "Either attributeId or name is required.");
    }

    GSErrCode err = ACAPI_Attribute_Get (&penTable);
    if (err != NoError) {
        return CreateFailedExecutionResult (err, "No Pen Table has the given attributeId.");
    }

    API_WindowInfo window = {};
    GS::UniString failure;
    err = GetCurrentModelWindow (window, failure);
    if (err != NoError) {
        return CreateFailedExecutionResult (err, failure + " The pen table of layouts cannot be changed through the Archicad API.");
    }

    err = ApplyPenTableThroughTemporaryView (window, name, failure);
    if (err != NoError) {
        return CreateFailedExecutionResult (err, failure);
    }

    API_AttributeIndex penTableInEffect = APIInvalidAttributeIndex;
    if (ACAPI_Attribute_GetCurrPenSet (&penTableInEffect) != NoError || penTableInEffect != penTable.header.index) {
        return CreateFailedExecutionResult (APIERR_GENERAL, "Archicad did not put the Pen Table into effect.");
    }
    return CreateSuccessfulExecutionResult ();
#else
    (void) parameters;
    return CreateFailedExecutionResult (APIERR_NOTSUPPORTED, "Available from Archicad 27.");
#endif
}
