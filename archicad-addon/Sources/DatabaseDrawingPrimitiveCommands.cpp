#include "DatabaseDrawingPrimitiveCommands.hpp"
#include "DrawingPrimitiveCommands.hpp"
#include "MigrationHelper.hpp"

namespace {

// The databases whose elements this command reads, with their name in #/WindowType.
struct DrawingDatabaseType {
    API_WindowTypeID   typeID;
    const char*        name;
    API_NavigatorItemTypeID projectMapItemType;
};

const DrawingDatabaseType DrawingDatabaseTypes[] = {
    { APIWind_SectionID,           "Section",           API_SectionNavItem },
    { APIWind_ElevationID,         "Elevation",         API_ElevationNavItem },
    { APIWind_InteriorElevationID, "InteriorElevation", API_InteriorElevationNavItem },
    { APIWind_DetailID,            "Details",           API_DetailDrawingNavItem },
    { APIWind_WorksheetID,         "Worksheet",         API_WorksheetDrawingNavItem },
    { APIWind_DocumentFrom3DID,    "3DDocument",        API_DocumentFrom3DNavItem },
    { APIWind_LayoutID,            "Layout",            API_LayoutNavItem },
    { APIWind_MasterLayoutID,      "MasterLayout",      API_MasterLayoutNavItem }
};

const DrawingDatabaseType* FindDrawingDatabaseType (API_WindowTypeID typeID)
{
    for (const DrawingDatabaseType& type : DrawingDatabaseTypes) {
        if (type.typeID == typeID) {
            return &type;
        }
    }
    return nullptr;
}

GSErrCode GetDatabaseUnIdsOfType (API_WindowTypeID typeID, GS::Array<API_DatabaseUnId>& databaseUnIds)
{
    switch (typeID) {
        case APIWind_SectionID:           return ACAPI_Database_GetSectionDatabases (nullptr, &databaseUnIds);
        case APIWind_ElevationID:         return ACAPI_Database_GetElevationDatabases (nullptr, &databaseUnIds);
        case APIWind_InteriorElevationID: return ACAPI_Database_GetInteriorElevationDatabases (nullptr, &databaseUnIds);
        case APIWind_DetailID:            return ACAPI_Database_GetDetailDatabases (nullptr, &databaseUnIds);
        case APIWind_WorksheetID:         return ACAPI_Database_GetWorksheetDatabases (nullptr, &databaseUnIds);
        case APIWind_DocumentFrom3DID:    return ACAPI_Database_GetDocumentFrom3DDatabases (nullptr, &databaseUnIds);
        case APIWind_LayoutID:            return ACAPI_Database_GetLayoutDatabases (nullptr, &databaseUnIds);
        case APIWind_MasterLayoutID:      return ACAPI_Database_GetMasterLayoutDatabases (nullptr, &databaseUnIds);
        default:                          return APIERR_BADDATABASE;
    }
}

// A databaseId names a database by its unique id only. Its type is the one whose database list
// contains that id; ACAPI_Window_GetDatabaseInfo then fills in the rest.
GSErrCode ResolveDatabaseOfId (const API_Guid& databaseGuid, API_DatabaseInfo& database)
{
    database = DatabaseIdResolver::Instance ().GetDatabaseWithId (databaseGuid);
    if (database.typeID == API_ZombieWindowID) {
        for (const DrawingDatabaseType& type : DrawingDatabaseTypes) {
            GS::Array<API_DatabaseUnId> databaseUnIds;
            if (GetDatabaseUnIdsOfType (type.typeID, databaseUnIds) != NoError) {
                continue;
            }
            for (const API_DatabaseUnId& databaseUnId : databaseUnIds) {
                if (databaseUnId.elemSetId == databaseGuid) {
                    database.typeID = type.typeID;
                    break;
                }
            }
            if (database.typeID != API_ZombieWindowID) {
                break;
            }
        }
        if (database.typeID == API_ZombieWindowID) {
            return APIERR_BADDATABASE;
        }
    }
    return ACAPI_Window_GetDatabaseInfo (&database);
}

#ifdef ServerMainVers_2700
// The Project Map item of a database, which opens its window like a double click in the Navigator.
GS::Optional<API_Guid> FindProjectMapItemOfDatabase (const API_DatabaseInfo& database)
{
    const DrawingDatabaseType* type = FindDrawingDatabaseType (database.typeID);
    if (type == nullptr) {
        return {};
    }

    API_NavigatorItem filterItem = {};
    filterItem.itemType        = type->projectMapItemType;
    filterItem.mapId           = (database.typeID == APIWind_LayoutID || database.typeID == APIWind_MasterLayoutID) ? API_LayoutMap : API_ProjectMap;
    filterItem.db.databaseUnId = database.databaseUnId;

    GS::Array<API_NavigatorItem> results;
    if (ACAPI_Navigator_SearchNavigatorItem (&filterItem, &results) != NoError) {
        return {};
    }
    for (const API_NavigatorItem& result : results) {
        if (result.itemType == filterItem.itemType && result.db.databaseUnId == database.databaseUnId) {
            return result.guid;
        }
    }
    return {};
}
#endif

// A database whose window was not opened since the project was opened is not built yet, so
// ACAPI_Database_ChangeCurrentDatabase fails with APIERR_BADDATABASE (see ACAPI_Window_ChangeWindow
// in ACAPI_Automate.h). Opening its window builds it.
GSErrCode OpenWindowOfDatabase (const API_DatabaseInfo& database)
{
    API_WindowInfo windowInfo = database;
    GSErrCode err = ACAPI_Window_ChangeWindow (&windowInfo);
#ifdef ServerMainVers_2700
    if (err != NoError) {
        // ChangeWindow refuses some never opened worksheets and details, while opening their
        // Project Map item works (as ChangeWindow does for #1602).
        const GS::Optional<API_Guid> projectMapItem = FindProjectMapItemOfDatabase (database);
        if (projectMapItem.HasValue ()) {
            const GS::UniString guidStr = APIGuidToString (projectMapItem.Get ());
            err = ACAPI_View_GoToView (guidStr.ToCStr ().Get ());
        }
    }
#endif
    return err;
}

bool IsSameDatabase (const API_DatabaseInfo& db1, const API_DatabaseInfo& db2)
{
    return db1.typeID == db2.typeID && db1.databaseUnId == db2.databaseUnId && db1.index == db2.index;
}

// Which elements of the database to return; an empty list keeps every element.
struct ElementFilter {
    GS::HashSet<API_ElemTypeID> types;
    GS::HashSet<API_Guid>       elements;

    bool Accepts (API_ElemTypeID type, const API_Guid& guid, API_ElemTypeID ownerType, const API_Guid& ownerGuid) const
    {
        if (!types.IsEmpty () && !types.Contains (type) && !(ownerType != API_ZombieElemID && types.Contains (ownerType))) {
            return false;
        }
        if (!elements.IsEmpty () && !elements.Contains (guid) && !(ownerGuid != APINULLGuid && elements.Contains (ownerGuid))) {
            return false;
        }
        return true;
    }
};

// Every element of the current database, in drawing order first, followed by any element the
// drawing order did not hand back.
GS::Array<API_Guid> GetElementsOfCurrentDatabase ()
{
    GS::Array<API_Guid> result;
    GS::HashSet<API_Guid> found;

    // The walk must be started with DrawOrderInit: in a database that was just built (a section
    // whose window was opened a moment ago) DrawOrderGetNext without it crashes Archicad 30 in
    // DrawOrderIterator::GoToFirstElementNotLessThan.
    if (ACAPI_DrawOrder_DrawOrderInit () == NoError) {
        API_Elem_Head elemHead = {};
        while (ACAPI_DrawOrder_DrawOrderGetNext (APIFilt_None, &elemHead) == NoError) {
            if (elemHead.guid == APINULLGuid || found.Contains (elemHead.guid)) {
                break;
            }
            found.Add (elemHead.guid);
            result.Push (elemHead.guid);
        }
        ACAPI_DrawOrder_DrawOrderTerm ();
    }

    GS::Array<API_Guid> elemList;
    if (ACAPI_Element_GetElemList (API_ZombieElemID, &elemList) == NoError) {
        for (const API_Guid& guid : elemList) {
            if (!found.Contains (guid)) {
                found.Add (guid);
                result.Push (guid);
            }
        }
    }
    return result;
}

GS::ObjectState CreateDatabaseObjectState (const API_DatabaseInfo& database)
{
    const DrawingDatabaseType* type = FindDrawingDatabaseType (database.typeID);
    GS::ObjectState os = CreateDatabaseIdObjectState (DatabaseIdResolver::Instance ().GetIdOfDatabase (database));
    os.Add ("windowType", GS::UniString (type != nullptr ? type->name : "Unknown"));
    os.Add ("name", GS::UniString (database.name));
    os.Add ("referenceId", GS::UniString (database.ref));
    os.Add ("title", GS::UniString (database.title));
    if (database.linkedElement != APINULLGuid) {
        os.Add ("linkedElementId", CreateGuidObjectState (database.linkedElement));
    }
    if (database.typeID == APIWind_LayoutID && database.masterLayoutUnId.elemSetId != APINULLGuid) {
        os.Add ("masterLayoutDatabaseId", CreateGuidObjectState (database.masterLayoutUnId.elemSetId));
    }
    return os;
}

// Reads the current database: its scale, origin and extent, and the primitives of its elements.
void ReadCurrentDatabase (const API_DatabaseInfo& database, const ElementFilter& filter, GS::ObjectState& response)
{
    double scale = 0.0;
    if (ACAPI_Drawing_GetDrawingScale (&scale) == NoError) {
        response.Add ("drawingScale", scale);
    }

    API_Coord3D localOrigin = {};
    if (ACAPI_Database_GetLocOrigo (&localOrigin) == NoError) {
        response.Add ("localOrigin", Create2DCoordinateObjectState (API_Coord { localOrigin.x, localOrigin.y }));
    }

    API_DatabaseUnId databaseUnId = database.databaseUnId;
    API_Box extent = {};
    if (ACAPI_Element_GetExtent (&extent, &databaseUnId) == NoError && extent.xMin <= extent.xMax && extent.yMin <= extent.yMax) {
        response.Add ("extent", GS::ObjectState (
            "xMin", extent.xMin,
            "yMin", extent.yMin,
            "xMax", extent.xMax,
            "yMax", extent.yMax));
    }

    const auto& elements = response.AddList<GS::ObjectState> ("elements");
    for (const API_Guid& guid : GetElementsOfCurrentDatabase ()) {
        API_Element element = {};
        element.header.guid = guid;
        if (ACAPI_Element_GetHeader (&element.header) != NoError) {
            continue;
        }
        const API_ElemTypeID type = GetElemTypeId (element.header);

        API_Guid       ownerGuid = APINULLGuid;
        API_ElemTypeID ownerType = API_ZombieElemID;
        if (type == API_SectElemID && ACAPI_Element_Get (&element) == NoError) {
            ownerGuid = element.sectElem.parentGuid;
#ifdef ServerMainVers_2600
            ownerType = GetElemTypeId (element.sectElem.parentType);
#else
            API_Elem_Head ownerHead = {};
            ownerHead.guid = ownerGuid;
            if (ACAPI_Element_GetHeader (&ownerHead) == NoError) {
                ownerType = GetElemTypeId (ownerHead);
            }
#endif
        }

        if (!filter.Accepts (type, guid, ownerType, ownerGuid)) {
            continue;
        }

        GS::ObjectState item = CreateElementIdObjectState (guid);
        item.Add ("elementType", GetElementTypeNonLocalizedName (type));
        if (ownerGuid != APINULLGuid) {
            item.Add ("ownerElementId", CreateGuidObjectState (ownerGuid));
            if (ownerType != API_ZombieElemID) {
                item.Add ("ownerElementType", GetElementTypeNonLocalizedName (ownerType));
            }
        }

        GS::Array<GS::ObjectState> primitives;
        const GSErrCode err = GetDrawingPrimitivesOfElement (element.header, primitives);
        if (err != NoError) {
            GS::ObjectState error;
            error.Add ("code", err);
            error.Add ("message", "Failed to get the drawing primitives of the element");
            item.Add ("error", error);
        } else {
            const auto& primitiveList = item.AddList<GS::ObjectState> ("primitives");
            for (const GS::ObjectState& primitive : primitives) {
                primitiveList (primitive);
            }
        }
        elements (item);
    }
}

} // namespace

GetDrawingPrimitivesOfDatabaseCommand::GetDrawingPrimitivesOfDatabaseCommand () :
    CommandBase (CommonSchema::Used)
{
}

GS::String GetDrawingPrimitivesOfDatabaseCommand::GetName () const
{
    return "GetDrawingPrimitivesOfDatabase";
}

GS::Optional<GS::UniString> GetDrawingPrimitivesOfDatabaseCommand::GetInputParametersSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "databaseId": {
                "$ref": "#/DatabaseId",
                "description": "The database to read: a section, elevation, interior elevation, detail, worksheet, 3D document, layout or master layout, as GetDatabases, GetContainingDatabases or GetDatabaseIdFromNavigatorItemId return it. Give either databaseId or navigatorItemId."
            },
            "navigatorItemId": {
                "$ref": "#/NavigatorItemId",
                "description": "A Project Map, View Map or Layout Book item whose database is read. Only its database is used: the saved settings of a view (layer combination, model view options, zoom) are not applied."
            },
            "elementTypes": {
                "type": "array",
                "description": "Optional. Only elements of these types are returned. A section element (SectElem) also matches the type of the model element it was cut from, so 'Wall' returns the walls drawn in a section.",
                "items": {
                    "$ref": "#/ElementType"
                },
                "minItems": 1
            },
            "elements": {
                "$ref": "#/Elements",
                "description": "Optional. Only these elements are returned. A section element also matches the id of the model element it was cut from."
            },
            "openWindowIfNeeded": {
                "type": "boolean",
                "description": "Optional, true by default. A database whose window was not opened since the project was opened cannot be read in the background. With true its window is opened, read and closed again (Archicad 27 or later), and the window that was in front before comes back to the front; windowOpened in the response says so. With false the command fails for such a database instead."
            },
            "rebuild": {
                "type": "boolean",
                "description": "Optional, false by default. A section, elevation or interior elevation read in the background shows the model as it was when the database was last built, for example when its window was last shown, so later changes of the model are missing. With true the database is rebuilt in the background first (ACAPI_Database_RebuildCurrentDatabase, Archicad 27 or later), as Archicad does when the window comes to the front."
            }
        },
        "additionalProperties": false,
        "required": []
    })";
}

GS::Optional<GS::UniString> GetDrawingPrimitivesOfDatabaseCommand::GetRawResponseSchema () const
{
    return GS::UniString (R"({
        "type": "object",
        "properties": {
            "database": {
                "type": "object",
                "description": "The database that was read.",
                "properties": {
                    "databaseId": {
                        "$ref": "#/DatabaseId"
                    },
                    "windowType": {
                        "$ref": "#/WindowType"
                    },
                    "name": {
                        "type": "string"
                    },
                    "referenceId": {
                        "type": "string"
                    },
                    "title": {
                        "type": "string",
                        "description": "The title of the window, generally the reference id and the name."
                    },
                    "linkedElementId": {
                        "$ref": "#/ElementId",
                        "description": "The element the database belongs to, for example the Section, Elevation, Interior Elevation, Detail or Worksheet marker it was made from. Missing for independent databases."
                    },
                    "masterLayoutDatabaseId": {
                        "$ref": "#/DatabaseId",
                        "description": "Layouts only: the master layout of the layout."
                    }
                },
                "additionalProperties": false,
                "required": [
                    "databaseId",
                    "windowType",
                    "name",
                    "referenceId",
                    "title"
                ]
            },
            "drawingScale": {
                "type": "number",
                "description": "The drawing scale of the database as its denominator, 100.0 for 1:100. The coordinates of the primitives are in meters at the size of the model in every database except layouts and master layouts, whose scale is 1.0 and whose coordinates are in meters on the sheet (drawings placed on a layout come back there at their placed size). In sections, elevations and interior elevations y is the elevation (z) and x runs along the marker's line in the direction from its start to its end, measured from the point of the line nearest to the project origin: x is the dot product of a floor plan point with the unit vector from start to end. The marker is linkedElementId; GetDetailsOfElements returns its start and end."
            },
            "localOrigin": {
                "$ref": "#/Coordinate2D",
                "description": "The user origin of the database, in the coordinates of the primitives. Missing for databases that have none."
            },
            "extent": {
                "$ref": "#/BoundingBox2D",
                "description": "The bounding box of every element of the database, hidden ones included, as ACAPI_Element_GetExtent returns it."
            },
            "windowOpened": {
                "type": "boolean",
                "description": "True when the database could not be read before its window was opened: the command opened its window, read it, closed it again (Archicad 27 or later) and brought back the window that was in front."
            },
            "elements": {
                "type": "array",
                "description": "The elements of the database in drawing order, each with its primitives in the format of GetDrawingPrimitivesOfElements.",
                "items": {
                    "type": "object",
                    "properties": {
                        "elementId": {
                            "$ref": "#/ElementId"
                        },
                        "elementType": {
                            "$ref": "#/ElementType"
                        },
                        "ownerElementId": {
                            "$ref": "#/ElementId",
                            "description": "Section elements (SectElem) only: the model element the section element was cut from."
                        },
                        "ownerElementType": {
                            "$ref": "#/ElementType",
                            "description": "Section elements (SectElem) only: the type of the model element the section element was cut from."
                        },
                        "primitives": {
                            "type": "array",
                            "description": "The primitives in the order Archicad draws them. The Ctrl_* items mark the beginning and the end of sub-parts (hatch borders, hatch lines, texts, ...).",
                            "items": )") + DrawingPrimitiveItemSchema () + R"(
                        },
                        "error": {
                            "$ref": "#/Error",
                            "description": "Why the primitives of this element could not be read."
                        }
                    },
                    "additionalProperties": false,
                    "required": [
                        "elementId",
                        "elementType"
                    ]
                }
            }
        },
        "additionalProperties": false,
        "required": [
            "database",
            "windowOpened",
            "elements"
        ]
    })";
}

GS::ObjectState GetDrawingPrimitivesOfDatabaseCommand::Execute (const GS::ObjectState& parameters, GS::ProcessControl& /*processControl*/) const
{
    const GS::ObjectState* databaseId      = parameters.Get ("databaseId");
    const GS::ObjectState* navigatorItemId = parameters.Get ("navigatorItemId");
    if ((databaseId == nullptr) == (navigatorItemId == nullptr)) {
        return CreateErrorResponse (APIERR_BADPARS, "Give either databaseId or navigatorItemId.");
    }

    API_DatabaseInfo target = {};
    GSErrCode err = NoError;
    if (databaseId != nullptr) {
        err = ResolveDatabaseOfId (GetGuidFromObjectState (*databaseId), target);
        if (err != NoError) {
            return CreateErrorResponse (err, "Failed to find the database of databaseId.");
        }
    } else {
        API_Guid navGuid = GetGuidFromObjectState (*navigatorItemId);
        API_NavigatorItem navigatorItem = {};
        err = ACAPI_Navigator_GetNavigatorItem (&navGuid, &navigatorItem);
        if (err != NoError) {
            return CreateErrorResponse (err, "Failed to find the navigator item of navigatorItemId.");
        }
        target = navigatorItem.db;
        err = ACAPI_Window_GetDatabaseInfo (&target);
        if (err != NoError) {
            return CreateErrorResponse (err, "Failed to find the database of the navigator item.");
        }
    }
    if (FindDrawingDatabaseType (target.typeID) == nullptr) {
        return CreateErrorResponse (APIERR_BADDATABASE, "The database must be a section, elevation, interior elevation, detail, worksheet, 3D document, layout or master layout. Floor plan elements are read with GetDrawingPrimitivesOfElements.");
    }

    ElementFilter filter;
    GS::Array<GS::UniString> elementTypes;
    if (parameters.Get ("elementTypes", elementTypes)) {
        for (const GS::UniString& elementType : elementTypes) {
            const API_ElemTypeID typeID = GetElementTypeFromNonLocalizedName (elementType);
            if (typeID == API_ZombieElemID) {
                return CreateErrorResponse (APIERR_BADPARS, "Unknown element type: " + elementType);
            }
            filter.types.Add (typeID);
        }
    }
    GS::Array<GS::ObjectState> elements;
    if (parameters.Get ("elements", elements)) {
        for (const GS::ObjectState& element : elements) {
            filter.elements.Add (GetGuidFromElementsArrayItem (element));
        }
    }
    bool openWindowIfNeeded = true;
    parameters.Get ("openWindowIfNeeded", openWindowIfNeeded);
    bool rebuild = false;
    parameters.Get ("rebuild", rebuild);
#ifndef ServerMainVers_2700
    if (rebuild) {
        return CreateErrorResponse (APIERR_NOTSUPPORTED, "rebuild requires Archicad 27 or later.");
    }
#endif

    API_DatabaseInfo startingDatabase = {};
    err = ACAPI_Database_GetCurrentDatabase (&startingDatabase);
    if (err != NoError) {
        return CreateErrorResponse (err, "Failed to get the current database.");
    }
    API_WindowInfo startingWindow = {};
    const bool hasStartingWindow = (ACAPI_Window_GetCurrentWindow (&startingWindow) == NoError);

    bool windowOpened = false;
    err = ACAPI_Database_ChangeCurrentDatabase (&target);
    if (err != NoError) {
        if (!openWindowIfNeeded) {
            return CreateErrorResponse (err, "The database cannot be read before its window is opened. Pass openWindowIfNeeded true, or open it once with ChangeWindow.");
        }
        const GSErrCode openErr = OpenWindowOfDatabase (target);
        if (openErr != NoError) {
            ACAPI_Database_ChangeCurrentDatabase (&startingDatabase);
            return CreateErrorResponse (openErr, "The database cannot be read in the background, and opening its window failed.");
        }
        windowOpened = true;
        err = ACAPI_Database_ChangeCurrentDatabase (&target);
    }
    GSErrCode rebuildErr = NoError;
#ifdef ServerMainVers_2700
    if (err == NoError && rebuild) {
        rebuildErr = ACAPI_Database_RebuildCurrentDatabase ();
    }
#endif

    GS::ObjectState response;
    if (err == NoError && rebuildErr == NoError) {
        response.Add ("database", CreateDatabaseObjectState (target));
        response.Add ("windowOpened", windowOpened);
        ReadCurrentDatabase (target, filter, response);
    }

    // Bring back what was current: the front window first, as ChangeWindow also changes the
    // current database, then close the window this command opened, then the database.
    if (windowOpened) {
        if (hasStartingWindow) {
            ACAPI_Window_ChangeWindow (&startingWindow);
        }
#ifdef ServerMainVers_2700
        if (!hasStartingWindow || !IsSameDatabase (startingWindow, target)) {
            API_WindowInfo openedWindow = target;
            ACAPI_Window_CloseWindow (&openedWindow);
        }
#endif
    }
    ACAPI_Database_ChangeCurrentDatabase (&startingDatabase);

    if (err != NoError) {
        return CreateErrorResponse (err, "Failed to switch to the database.");
    }
    if (rebuildErr != NoError) {
        return CreateErrorResponse (rebuildErr, "Failed to rebuild the database.");
    }
    return response;
}
