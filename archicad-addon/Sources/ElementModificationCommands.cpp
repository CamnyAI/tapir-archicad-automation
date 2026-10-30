#include "ElementModificationCommands.hpp"
#include "ElementCreationCommands.hpp"
#include "MigrationHelper.hpp"

#include <cmath>

namespace {

constexpr const char* ModifyIdSchema = R"(
                        "id": {
                            "type": "string",
                            "description": "The new Element ID (the ID field of the Settings dialog, shown in schedules and labels; the same string GetDetailsOfElements returns as id)."
                        },)";

constexpr const char* ModifyFloorIndexSchema = R"(
                        "floorIndex": {
                            "type": "integer",
                            "description": "Moves the element to this story (story index)."
                        },)";

constexpr const char* ModifyLayerIndexSchema = R"(
                        "layerIndex": {
                            "type": "integer",
                            "description": "Moves the element to this layer (layer attribute index). The layer must exist and must not be locked."
                        },)";

// The line-family settings Line, Arc, Circle and Spline share with their Create commands.
constexpr const char* ModifyLineFamilySettingsSchema = R"(
                        "linePenIndex": {
                            "type": "integer",
                            "minimum": 1,
                            "maximum": 255,
                            "description": "Pen index of the line. Clears a color override pen."
                        },
                        "lineTypeId": {
                            "$ref": "#/AttributeId",
                            "description": "Line type attribute of the line."
                        },
                        "roomSeparator": {
                            "type": "boolean",
                            "description": "Is this a zone boundary line?"
                        })";

GS::Optional<GS::UniString> GetLineTypeIndex (const GS::ObjectState& attributeId, API_AttributeIndex& index)
{
    API_Attribute attr = {};
    attr.header.typeID = API_LinetypeID;
    attr.header.guid = GetGuidFromObjectState (attributeId);
    if (ACAPI_Attribute_Get (&attr) != NoError) {
        return GS::UniString ("lineTypeId does not refer to an existing line type.");
    }
    index = attr.header.index;
    return {};
}

struct LineFamilyChanges {
    bool pen = false;
    bool lineType = false;
    bool roomSeparator = false;
};

GS::Optional<GS::UniString> ApplyLineFamilySettings (const GS::ObjectState& item,
                                                     API_ExtendedPenType& linePen,
                                                     API_AttributeIndex& ltypeInd,
                                                     bool& roomSeparator,
                                                     LineFamilyChanges& changes)
{
    Int32 penIndex = 0;
    if (item.Get ("linePenIndex", penIndex)) {
        linePen.penIndex = (short) penIndex;
        linePen.colorOverridePenIndex = 0;
        changes.pen = true;
    }
    const GS::ObjectState* lineTypeId = item.Get ("lineTypeId");
    if (lineTypeId != nullptr) {
        auto error = GetLineTypeIndex (*lineTypeId, ltypeInd);
        if (error.HasValue ()) {
            return error;
        }
        changes.lineType = true;
    }
    if (item.Get ("roomSeparator", roomSeparator)) {
        changes.roomSeparator = true;
    }
    return {};
}

bool HasCoordinate (const GS::ObjectState& item, const char* fieldName, API_Coord& coordinate)
{
    const GS::ObjectState* os = item.Get (fieldName);
    if (os == nullptr) {
        return false;
    }
    coordinate = Get2DCoordinateFromObjectState (*os);
    return true;
}

// The arc fields of Arcs and Circles. Arcs also have their begin and end angles.
GS::Optional<GS::UniString> ApplyArcGeometry (const GS::ObjectState& item, API_ArcType& arc, API_Element& mask, bool withAngles)
{
    if (HasCoordinate (item, "origin", arc.origC)) {
        ACAPI_ELEMENT_MASK_SET (mask, API_ArcType, origC);
    }
    if (item.Get ("radius", arc.r)) {
        if (arc.r <= 0.0) {
            return GS::UniString ("radius must be positive.");
        }
        ACAPI_ELEMENT_MASK_SET (mask, API_ArcType, r);
    }
    if (item.Get ("angle", arc.angle)) {
        ACAPI_ELEMENT_MASK_SET (mask, API_ArcType, angle);
    }
    if (item.Get ("ratio", arc.ratio)) {
        if (arc.ratio <= 0.0) {
            return GS::UniString ("ratio must be positive.");
        }
        ACAPI_ELEMENT_MASK_SET (mask, API_ArcType, ratio);
    }
    if (item.Get ("reflected", arc.reflected)) {
        ACAPI_ELEMENT_MASK_SET (mask, API_ArcType, reflected);
    }
    if (withAngles) {
        if (item.Get ("begAngle", arc.begAng)) {
            ACAPI_ELEMENT_MASK_SET (mask, API_ArcType, begAng);
        }
        if (item.Get ("endAngle", arc.endAng)) {
            ACAPI_ELEMENT_MASK_SET (mask, API_ArcType, endAng);
        }
    }

    LineFamilyChanges changes;
    auto error = ApplyLineFamilySettings (item, arc.linePen, arc.ltypeInd, arc.roomSeparator, changes);
    if (error.HasValue ()) {
        return error;
    }
    if (changes.pen) ACAPI_ELEMENT_MASK_SET (mask, API_ArcType, linePen);
    if (changes.lineType) ACAPI_ELEMENT_MASK_SET (mask, API_ArcType, ltypeInd);
    if (changes.roomSeparator) ACAPI_ELEMENT_MASK_SET (mask, API_ArcType, roomSeparator);
    return {};
}

constexpr const char* ModifyArcGeometrySchema = R"(
                        "origin": {
                            "$ref": "#/Coordinate2D",
                            "description": "The center point."
                        },
                        "radius": {
                            "type": "number",
                            "exclusiveMinimum": 0,
                            "description": "The radius (of the 'a' axis of an ellipse)."
                        },
                        "angle": {
                            "type": "number",
                            "description": "The angle of the 'a' axis of an ellipse in radians; 0 for a circular one."
                        },
                        "ratio": {
                            "type": "number",
                            "exclusiveMinimum": 0,
                            "description": "a/b of an ellipse; 1 for a circular one."
                        },
                        "reflected": {
                            "type": "boolean",
                            "description": "Is the ellipse reflected onto its smaller axis?"
                        },)";

// The input schema of a Modify command: an array of items, each with elementId, the optional
// id, story and layer, and the type specific properties (JSON object members without braces,
// without a trailing comma). Written in the subset of C++ tools/generate_addon_docs.py reads.
GS::UniString BuildModifyElementsSchema (const char* arrayFieldName, const char* description, bool hasId, bool hasFloorIndex, const GS::UniString& itemProperties)
{
    GS::UniString schema = GS::UniString::Printf (R"({
        "type": "object",
        "properties": {
            "%s": {
                "type": "array",
                "description": "%s Only the given fields are changed. The elements keep their GUIDs.",
                "items": {
                    "type": "object",
                    "properties": {
                        "elementId": {
                            "$ref": "#/ElementId"
                        },)", arrayFieldName, description);
    if (hasId) {
        schema += ModifyIdSchema;
    }
    if (hasFloorIndex) {
        schema += ModifyFloorIndexSchema;
    }
    schema += ModifyLayerIndexSchema;
    schema += itemProperties;
    schema += GS::UniString::Printf (R"(
                    },
                    "additionalProperties": false,
                    "required": [
                        "elementId"
                    ]
                }
            }
        },
        "additionalProperties": false,
        "required": [
            "%s"
        ]
    })", arrayFieldName);
    return schema;
}

bool IsMaskEmpty (const API_Element& mask)
{
    const char* bytes = reinterpret_cast<const char*> (&mask);
    for (size_t ii = 0; ii < sizeof (API_Element); ++ii) {
        if (bytes[ii] != 0) {
            return false;
        }
    }
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// ModifyElementsCommandBase
// ---------------------------------------------------------------------------

ModifyElementsCommandBase::ModifyElementsCommandBase (const GS::String& commandNameIn, const GS::String& arrayFieldNameIn, const GS::UniString& elemTypeNameIn) :
    CommandBase (CommonSchema::Used),
    commandName (commandNameIn),
    arrayFieldName (arrayFieldNameIn),
    elemTypeName (elemTypeNameIn)
{
}

GS::String ModifyElementsCommandBase::GetName () const
{
    return commandName;
}

GS::Optional<GS::UniString> ModifyElementsCommandBase::GetRawResponseSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "executionResults": {
                "$ref": "#/ExecutionResults"
            }
        },
        "additionalProperties": false,
        "required": [
            "executionResults"
        ]
    })";
}

GS::ObjectState ModifyElementsCommandBase::Execute (const GS::ObjectState& parameters, GS::ProcessControl& /*processControl*/) const
{
    GS::Array<GS::ObjectState> items;
    parameters.Get (arrayFieldName, items);

    GS::ObjectState response;
    const auto& executionResults = response.AddList<GS::ObjectState> ("executionResults");
    const Stories stories = GetStories ();

    ACAPI_CallUndoableCommand ("Modify " + elemTypeName, [&] () -> GSErrCode {
        for (const GS::ObjectState& item : items) {
            const GS::ObjectState* elementId = item.Get ("elementId");
            if (elementId == nullptr) {
                executionResults (CreateFailedExecutionResult (APIERR_BADPARS, "elementId is missing."));
                continue;
            }

            API_Element element = {};
            element.header.guid = GetGuidFromObjectState (*elementId);
            GSErrCode err = ACAPI_Element_Get (&element);
            if (err != NoError) {
                executionResults (CreateFailedExecutionResult (err, "Failed to find the element."));
                continue;
            }
            if (!IsExpectedType (element)) {
                executionResults (CreateFailedExecutionResult (APIERR_BADID, "The element is not a " + elemTypeName + "."));
                continue;
            }

            API_Element mask = {};
            ACAPI_ELEMENT_MASK_CLEAR (mask);

            Int32 floorIndex = 0;
            const bool hasFloor = GetFloorFieldName () != nullptr && item.Get (GetFloorFieldName (), floorIndex);
            if (hasFloor) {
                if (stories.find ((short) floorIndex) == stories.end ()) {
                    executionResults (CreateFailedExecutionResult (APIERR_BADINDEX, GS::UniString (GetFloorFieldName ()) + " does not refer to an existing story."));
                    continue;
                }
                element.header.floorInd = (short) floorIndex;
                ACAPI_ELEMENT_MASK_SET (mask, API_Elem_Head, floorInd);
            }

            Int32 layerIndex = 0;
            if (item.Get ("layerIndex", layerIndex)) {
                const GS::Optional<GS::ObjectState> layerError = ApplyLayerIndexFromParameters (item, element);
                if (layerError.HasValue ()) {
                    executionResults (CreateFailedExecutionResult (*layerError));
                    continue;
                }
                ACAPI_ELEMENT_MASK_SET (mask, API_Elem_Head, layer);
            }

            API_ElementMemo memo = {};
            const GS::OnExit memoGuard ([&memo] () { ACAPI_DisposeElemMemoHdls (&memo); });
            UInt64 memoMask = 0;
            const GS::Optional<GS::UniString> applyError = ApplyTypeSpecificParameters (item, element, mask, memo, memoMask);
            if (applyError.HasValue ()) {
                executionResults (CreateFailedExecutionResult (APIERR_BADPARS, *applyError));
                continue;
            }

            GS::UniString id;
            const bool hasId = HasElementId () && item.Get ("id", id);
            const bool hasChange = memoMask != 0 || !IsMaskEmpty (mask);
            if (!hasId && !hasChange && !HasAfterChangeFields (item)) {
                executionResults (CreateFailedExecutionResult (APIERR_BADPARS, "No fields to modify were given."));
                continue;
            }

            // The Element ID is written first, as SetDetailsOfElements does, so a refused id
            // leaves the element as it was.
            if (hasId) {
                err = ChangeElementId (element.header.guid, id);
                if (err != NoError) {
                    executionResults (CreateFailedExecutionResult (err, "Failed to set the id of the element; nothing was changed."));
                    continue;
                }
            }

            if (hasChange) {
                const API_Guid guid = element.header.guid;
                err = ACAPI_Element_Change (&element, &mask, memoMask != 0 ? &memo : nullptr, memoMask, true);
                if (err != NoError) {
                    GS::UniString message = hasId ? "Failed to modify the element; only its id was set." : "Failed to modify the element.";
                    // A refused change can take the element with it (seen in Archicad 30 with a
                    // dimension memo Archicad did not accept), so the answer says so.
                    API_Elem_Head after = {};
                    after.guid = guid;
                    if (ACAPI_Element_GetHeader (&after) != NoError) {
                        message += " The element no longer exists after the refused change.";
                    }
                    executionResults (CreateFailedExecutionResult (err, message));
                    continue;
                }
            }

            const GS::Optional<GS::UniString> afterError = AfterChange (item, element);
            if (afterError.HasValue ()) {
                executionResults (CreateFailedExecutionResult (APIERR_GENERAL, "The element was changed, but " + *afterError));
                continue;
            }

            // Archicad can accept a story change and keep the element where it was, so the
            // story is read back.
            if (hasFloor) {
                API_Elem_Head changed = {};
                changed.guid = element.header.guid;
                if (ACAPI_Element_GetHeader (&changed) == NoError && changed.floorInd != (short) floorIndex) {
                    executionResults (CreateFailedExecutionResult (APIERR_REFUSEDPAR, "Archicad kept the element on its story; the other fields were changed."));
                    continue;
                }
            }
            executionResults (CreateSuccessfulExecutionResult ());
        }
        return NoError;
    });

    return response;
}

// ---------------------------------------------------------------------------
// ModifyLines
// ---------------------------------------------------------------------------

ModifyLinesCommand::ModifyLinesCommand () :
    ModifyElementsCommandBase ("ModifyLines", "linesWithDetails", "Line")
{
}

bool ModifyLinesCommand::IsExpectedType (const API_Element& element) const
{
    return GetElemTypeId (element.header) == API_LineID;
}

GS::Optional<GS::UniString> ModifyLinesCommand::GetInputParametersSchema () const
{
    return BuildModifyElementsSchema ("linesWithDetails",
        "The Line elements to modify, each with the fields to change, the ones CreateLineElements takes.",
        false, false, GS::UniString (R"(
                        "begCoordinate": {
                            "$ref": "#/Coordinate2D",
                            "description": "The new start point."
                        },
                        "endCoordinate": {
                            "$ref": "#/Coordinate2D",
                            "description": "The new end point."
                        },)") + ModifyLineFamilySettingsSchema);
}

GS::Optional<GS::UniString> ModifyLinesCommand::ApplyTypeSpecificParameters (const GS::ObjectState& item, API_Element& element, API_Element& mask, API_ElementMemo& /*memo*/, UInt64& /*memoMask*/) const
{
    const bool hasBeg = HasCoordinate (item, "begCoordinate", element.line.begC);
    const bool hasEnd = HasCoordinate (item, "endCoordinate", element.line.endC);
    if (hasBeg) {
        ACAPI_ELEMENT_MASK_SET (mask, API_LineType, begC);
    }
    if (hasEnd) {
        ACAPI_ELEMENT_MASK_SET (mask, API_LineType, endC);
    }
    if ((hasBeg || hasEnd) && std::hypot (element.line.endC.x - element.line.begC.x, element.line.endC.y - element.line.begC.y) < 1e-9) {
        return GS::UniString ("The start and end points of the line would be the same.");
    }

    LineFamilyChanges changes;
    auto error = ApplyLineFamilySettings (item, element.line.linePen, element.line.ltypeInd, element.line.roomSeparator, changes);
    if (error.HasValue ()) {
        return error;
    }
    if (changes.pen) ACAPI_ELEMENT_MASK_SET (mask, API_LineType, linePen);
    if (changes.lineType) ACAPI_ELEMENT_MASK_SET (mask, API_LineType, ltypeInd);
    if (changes.roomSeparator) ACAPI_ELEMENT_MASK_SET (mask, API_LineType, roomSeparator);
    return {};
}

// ---------------------------------------------------------------------------
// ModifyArcs
// ---------------------------------------------------------------------------

ModifyArcsCommand::ModifyArcsCommand () :
    ModifyElementsCommandBase ("ModifyArcs", "arcsWithDetails", "Arc")
{
}

bool ModifyArcsCommand::IsExpectedType (const API_Element& element) const
{
    // Archicad keeps the circles CreateCircles makes as arcs that span the whole circle, so the
    // command takes both types.
    const API_ElemTypeID typeID = GetElemTypeId (element.header);
    return typeID == API_ArcID || typeID == API_CircleID;
}

GS::Optional<GS::UniString> ModifyArcsCommand::GetInputParametersSchema () const
{
    return BuildModifyElementsSchema ("arcsWithDetails",
        "The Arc and Circle elements to modify, each with the fields to change: the ones CreateArcs takes, and the ellipse fields GetDetailsOfElements returns. Begin and end angles given to a whole circle make it an arc.",
        false, false, GS::UniString (ModifyArcGeometrySchema) + R"(
                        "begAngle": {
                            "type": "number",
                            "description": "Beginning angle of the arc in radians."
                        },
                        "endAngle": {
                            "type": "number",
                            "description": "End angle of the arc in radians."
                        },)" + ModifyLineFamilySettingsSchema);
}

GS::Optional<GS::UniString> ModifyArcsCommand::ApplyTypeSpecificParameters (const GS::ObjectState& item, API_Element& element, API_Element& mask, API_ElementMemo& /*memo*/, UInt64& /*memoMask*/) const
{
    return ApplyArcGeometry (item, element.arc, mask, true);
}

// ---------------------------------------------------------------------------
// ModifyCircles
// ---------------------------------------------------------------------------

ModifyCirclesCommand::ModifyCirclesCommand () :
    ModifyElementsCommandBase ("ModifyCircles", "circlesWithDetails", "Circle")
{
}

bool ModifyCirclesCommand::IsExpectedType (const API_Element& element) const
{
    // Circle elements, and the whole-circle arcs CreateCircles makes (GetDetailsOfElements reports
    // them as Arcs). Their begin and end angles are the same; the 'whole' flag is not reliable here
    // (tested in Archicad 30: it stayed true for such an arc after ModifyArcs gave it end angles).
    const API_ElemTypeID typeID = GetElemTypeId (element.header);
    if (typeID == API_CircleID) {
        return true;
    }
    if (typeID != API_ArcID) {
        return false;
    }
    const double span = std::fabs (element.arc.endAng - element.arc.begAng);
    return span < 1e-9 || std::fabs (span - 2.0 * 3.14159265358979323846) < 1e-9;
}

GS::Optional<GS::UniString> ModifyCirclesCommand::GetInputParametersSchema () const
{
    return BuildModifyElementsSchema ("circlesWithDetails",
        "The Circle elements to modify, each with the fields to change: the ones CreateCircles takes, and the ellipse fields GetDetailsOfElements returns. Archicad keeps a circle made by CreateCircles as an arc spanning the whole circle, which GetDetailsOfElements reports as an Arc; both are accepted, an arc with different begin and end angles is not.",
        false, false, GS::UniString (ModifyArcGeometrySchema) + ModifyLineFamilySettingsSchema);
}

GS::Optional<GS::UniString> ModifyCirclesCommand::ApplyTypeSpecificParameters (const GS::ObjectState& item, API_Element& element, API_Element& mask, API_ElementMemo& /*memo*/, UInt64& /*memoMask*/) const
{
    return ApplyArcGeometry (item, element.circle, mask, false);
}

// ---------------------------------------------------------------------------
// ModifyPolylines
// ---------------------------------------------------------------------------

ModifyPolylinesCommand::ModifyPolylinesCommand () :
    ModifyElementsCommandBase ("ModifyPolylines", "polylinesWithDetails", "Polyline")
{
}

bool ModifyPolylinesCommand::IsExpectedType (const API_Element& element) const
{
    return GetElemTypeId (element.header) == API_PolyLineID;
}

GS::Optional<GS::UniString> ModifyPolylinesCommand::GetInputParametersSchema () const
{
    return BuildModifyElementsSchema ("polylinesWithDetails",
        "The Polyline elements to modify, each with the fields to change, the ones CreatePolylines takes. The line type can also be given as lineTypeId, as GetDetailsOfElements returns it.",
        false, false, GS::UniString (R"(
                        "coordinates": {
                            "type": "array",
                            "description": "The new points of the polyline. A polyline whose last point is its first is closed. Replaces the whole geometry: arcs not given again become straight segments.",
                            "items": {
                                "$ref": "#/Coordinate2D"
                            },
                            "minItems": 2
                        },
                        "arcs": {
                            "type": "array",
                            "description": "The curved segments of the new geometry, with indices into coordinates. Only with coordinates.",
                            "items": {
                                "$ref": "#/PolyArc"
                            }
                        },
                        "lineTypeIndex": {
                            "type": "integer",
                            "minimum": 1,
                            "description": "Line type attribute index of the polyline, as CreatePolylines takes it. Do not combine with lineTypeId."
                        },
                        "penWeightMm": {
                            "type": "number",
                            "description": "Pen weight override in mm, as CreatePolylines takes it."
                        },)") + ModifyLineFamilySettingsSchema);
}

GS::Optional<GS::UniString> ModifyPolylinesCommand::ApplyTypeSpecificParameters (const GS::ObjectState& item, API_Element& element, API_Element& mask, API_ElementMemo& memo, UInt64& memoMask) const
{
    GS::Array<GS::ObjectState> coordinates;
    GS::Array<GS::ObjectState> arcs;
    const bool hasCoordinates = item.Get ("coordinates", coordinates);
    const bool hasArcs = item.Get ("arcs", arcs);
    if (hasArcs && !hasCoordinates) {
        return GS::UniString ("arcs can only be given together with coordinates.");
    }
    if (hasCoordinates) {
        const Int32 nCoords = (Int32) coordinates.GetSize ();
        if (nCoords < 2) {
            return GS::UniString ("A polyline needs at least 2 coordinates.");
        }
        for (const GS::ObjectState& arc : arcs) {
            Int32 begIndex = -1;
            Int32 endIndex = -1;
            arc.Get ("begIndex", begIndex);
            arc.Get ("endIndex", endIndex);
            if (begIndex < 0 || begIndex >= nCoords || endIndex < 0 || endIndex >= nCoords || begIndex == endIndex) {
                return GS::UniString ("An arc refers to a point that is not in coordinates.");
            }
        }
        const GS::Array<API_PolyArc> polyArcs = GetPolyArcs (arcs, 1);

        // The memo layout SetDetailsOfElements writes for a polyline: a dummy point at index 0,
        // one subpolygon, and vertexIDs[0] holding the largest vertex ID, which Archicad needs to
        // accept the polygon.
        memo.coords = reinterpret_cast<API_Coord**> (BMAllocateHandle ((nCoords + 1) * sizeof (API_Coord), ALLOCATE_CLEAR, 0));
        memo.pends = reinterpret_cast<Int32**> (BMAllocateHandle (2 * sizeof (Int32), ALLOCATE_CLEAR, 0));
        memo.parcs = reinterpret_cast<API_PolyArc**> (BMAllocateHandle (polyArcs.GetSize () * sizeof (API_PolyArc), ALLOCATE_CLEAR, 0));
        memo.vertexIDs = reinterpret_cast<UInt32**> (BMAllocateHandle ((nCoords + 1) * sizeof (UInt32), ALLOCATE_CLEAR, 0));
        if (memo.coords == nullptr || memo.pends == nullptr || memo.parcs == nullptr || memo.vertexIDs == nullptr) {
            return GS::UniString ("Not enough memory for the polyline.");
        }
        (*memo.coords)[0] = { -1.0, 0.0 };
        (*memo.vertexIDs)[0] = (UInt32) nCoords;
        for (Int32 ii = 0; ii < nCoords; ++ii) {
            (*memo.coords)[ii + 1] = Get2DCoordinateFromObjectState (coordinates[ii]);
            (*memo.vertexIDs)[ii + 1] = (UInt32) (ii + 1);
        }
        (*memo.pends)[0] = 0;
        (*memo.pends)[1] = nCoords;
        Int32 iArc = 0;
        for (const API_PolyArc& polyArc : polyArcs) {
            (*memo.parcs)[iArc++] = polyArc;
        }
        element.polyLine.poly.nCoords = nCoords;
        element.polyLine.poly.nSubPolys = 1;
        element.polyLine.poly.nArcs = (Int32) polyArcs.GetSize ();
        ACAPI_ELEMENT_MASK_SET (mask, API_PolyLineType, poly);
        memoMask = APIMemoMask_Polygon;
    }

    Int32 lineTypeIndex = 0;
    if (item.Get ("lineTypeIndex", lineTypeIndex)) {
        if (item.Get ("lineTypeId") != nullptr) {
            return GS::UniString ("Give the line type either as lineTypeIndex or as lineTypeId.");
        }
        API_Attribute attr = {};
        attr.header.typeID = API_LinetypeID;
        attr.header.index = ACAPI_CreateAttributeIndex (lineTypeIndex);
        if (ACAPI_Attribute_Get (&attr) != NoError) {
            return GS::UniString ("lineTypeIndex does not refer to an existing line type.");
        }
        element.polyLine.ltypeInd = attr.header.index;
        ACAPI_ELEMENT_MASK_SET (mask, API_PolyLineType, ltypeInd);
    }

    if (item.Get ("penWeightMm", element.polyLine.penWeight)) {
        ACAPI_ELEMENT_MASK_SET (mask, API_PolyLineType, penWeight);
    }

    LineFamilyChanges changes;
    auto error = ApplyLineFamilySettings (item, element.polyLine.linePen, element.polyLine.ltypeInd, element.polyLine.roomSeparator, changes);
    if (error.HasValue ()) {
        return error;
    }
    if (changes.pen) ACAPI_ELEMENT_MASK_SET (mask, API_PolyLineType, linePen);
    if (changes.lineType) ACAPI_ELEMENT_MASK_SET (mask, API_PolyLineType, ltypeInd);
    if (changes.roomSeparator) ACAPI_ELEMENT_MASK_SET (mask, API_PolyLineType, roomSeparator);
    return {};
}

// ---------------------------------------------------------------------------
// ModifySplines
// ---------------------------------------------------------------------------

ModifySplinesCommand::ModifySplinesCommand () :
    ModifyElementsCommandBase ("ModifySplines", "splinesWithDetails", "Spline")
{
}

bool ModifySplinesCommand::IsExpectedType (const API_Element& element) const
{
    return GetElemTypeId (element.header) == API_SplineID;
}

GS::Optional<GS::UniString> ModifySplinesCommand::GetInputParametersSchema () const
{
    return BuildModifyElementsSchema ("splinesWithDetails",
        "The Spline elements to modify, each with the fields to change: the ones CreateSplines takes, and the Bezier directions of the points.",
        false, false, GS::UniString (R"(
                        "coordinates": {
                            "type": "array",
                            "description": "The new points of the spline. Do not repeat the first point at the end even for a closed spline. Without bezierDirections Archicad smooths the curve through them.",
                            "items": {
                                "$ref": "#/Coordinate2D"
                            },
                            "minItems": 2
                        },
                        "bezierDirections": {
                            "type": "array",
                            "description": "The handles of the points, one per point of coordinates (or of the current points when coordinates is not given). Turns automatic smoothing off.",
                            "items": {
                                "type": "object",
                                "properties": {
                                    "lenPrev": {
                                        "type": "number",
                                        "minimum": 0,
                                        "description": "Length of the handle towards the previous point."
                                    },
                                    "lenNext": {
                                        "type": "number",
                                        "minimum": 0,
                                        "description": "Length of the handle towards the next point."
                                    },
                                    "dirAng": {
                                        "type": "number",
                                        "description": "Direction of the handles (the tangent) in radians."
                                    }
                                },
                                "additionalProperties": false,
                                "required": [
                                    "lenPrev",
                                    "lenNext",
                                    "dirAng"
                                ]
                            },
                            "minItems": 2
                        },
                        "closed": {
                            "type": "boolean",
                            "description": "Is this a closed curve?"
                        },)") + ModifyLineFamilySettingsSchema);
}

GS::Optional<GS::UniString> ModifySplinesCommand::ApplyTypeSpecificParameters (const GS::ObjectState& item, API_Element& element, API_Element& mask, API_ElementMemo& memo, UInt64& memoMask) const
{
    GS::Array<GS::ObjectState> coordinates;
    GS::Array<GS::ObjectState> directions;
    const bool hasCoordinates = item.Get ("coordinates", coordinates);
    const bool hasDirections = item.Get ("bezierDirections", directions);
    const bool hasClosed = item.Get ("closed", element.spline.closed);

    if (hasCoordinates || hasDirections || hasClosed) {
        // The geometry is always written whole: the points, and the handles or automatic smoothing.
        API_ElementMemo current = {};
        const GS::OnExit currentGuard ([&current] () { ACAPI_DisposeElemMemoHdls (&current); });
        const GSErrCode err = ACAPI_Element_GetMemo (element.header.guid, &current, APIMemoMask_Polygon);
        if (err != NoError || current.coords == nullptr) {
            return GS::UniString ("Failed to read the current points of the spline.");
        }
        const Int32 nCurrent = (Int32) (BMGetHandleSize (reinterpret_cast<GSHandle> (current.coords)) / sizeof (API_Coord));
        const Int32 nPoints = hasCoordinates ? (Int32) coordinates.GetSize () : nCurrent;
        if (nPoints < 2) {
            return GS::UniString ("A spline needs at least 2 points.");
        }
        if (hasDirections && (Int32) directions.GetSize () != nPoints) {
            return GS::UniString ("bezierDirections needs one direction for each point of the spline.");
        }

        memo.coords = reinterpret_cast<API_Coord**> (BMAllocateHandle (nPoints * sizeof (API_Coord), ALLOCATE_CLEAR, 0));
        if (memo.coords == nullptr) {
            return GS::UniString ("Not enough memory for the spline.");
        }
        for (Int32 ii = 0; ii < nPoints; ++ii) {
            (*memo.coords)[ii] = hasCoordinates ? Get2DCoordinateFromObjectState (coordinates[ii]) : (*current.coords)[ii];
        }

        // The handles: the given ones, or the current ones when only closed changes; new points
        // without handles are smoothed by Archicad.
        const bool keepCurrentDirections = !hasCoordinates && !hasDirections && !element.spline.autoSmooth && current.bezierDirs != nullptr;
        if (hasDirections || keepCurrentDirections) {
            memo.bezierDirs = reinterpret_cast<API_SplineDir**> (BMAllocateHandle (nPoints * sizeof (API_SplineDir), ALLOCATE_CLEAR, 0));
            if (memo.bezierDirs == nullptr) {
                return GS::UniString ("Not enough memory for the spline.");
            }
            for (Int32 ii = 0; ii < nPoints; ++ii) {
                if (hasDirections) {
                    API_SplineDir dir = {};
                    directions[ii].Get ("lenPrev", dir.lenPrev);
                    directions[ii].Get ("lenNext", dir.lenNext);
                    directions[ii].Get ("dirAng", dir.dirAng);
                    (*memo.bezierDirs)[ii] = dir;
                } else {
                    (*memo.bezierDirs)[ii] = (*current.bezierDirs)[ii];
                }
            }
            element.spline.autoSmooth = false;
        } else {
            element.spline.autoSmooth = true;
        }
        ACAPI_ELEMENT_MASK_SET (mask, API_SplineType, autoSmooth);
        ACAPI_ELEMENT_MASK_SET (mask, API_SplineType, closed);
        memoMask = APIMemoMask_Polygon;
    }

    LineFamilyChanges changes;
    auto error = ApplyLineFamilySettings (item, element.spline.linePen, element.spline.ltypeInd, element.spline.roomSeparator, changes);
    if (error.HasValue ()) {
        return error;
    }
    if (changes.pen) ACAPI_ELEMENT_MASK_SET (mask, API_SplineType, linePen);
    if (changes.lineType) ACAPI_ELEMENT_MASK_SET (mask, API_SplineType, ltypeInd);
    if (changes.roomSeparator) ACAPI_ELEMENT_MASK_SET (mask, API_SplineType, roomSeparator);
    return {};
}

// ---------------------------------------------------------------------------
// ModifyDimensions
// ---------------------------------------------------------------------------

namespace {

struct NamedValue {
    const char* name;
    Int32 value;
};

constexpr NamedValue DimensionAppearances[] = {
    { "Linear", APIApp_Normal }, { "Cumulative", APIApp_Cumu }, { "Baseline", APIApp_CumuSv }, { "Elevation", APIApp_Elev }
};
constexpr NamedValue DimensionTextPositions[] = {
    { "Above", APIPos_Above }, { "In", APIPos_In }, { "Below", APIPos_Below }
};
constexpr NamedValue DimensionWitnessForms[] = {
    { "None", APIWtn_None }, { "Small", APIWtn_Small }, { "Large", APIWtn_Large }, { "Fix", APIWtn_Fix }
};
constexpr NamedValue DimensionMarkerTypes[] = {
    { "CrossLine", APIMark_CrossLine }, { "EmptyCircle", APIMark_EmptyCirc }, { "SlashLine", APIMark_SlashLine },
    { "OpenArrow30", APIMark_OpenArrow30 }, { "ClosedArrow30", APIMark_ClosArrow30 }, { "FullArrow30", APIMark_FullArrow30 },
    { "SlashLine45", APIMark_SlashLine45 }, { "CrossCircle", APIMark_CrossCirc }, { "OpenArrow90", APIMark_OpenArrow90 },
    { "ClosedArrow90", APIMark_ClosArrow90 }, { "FullArrow90", APIMark_FullArrow90 }, { "FullCircle", APIMark_FullCirc },
    { "PepitaCircle", APIMark_PepitaCirc }, { "BandArrow", APIMark_BandArrow }, { "OpenArrow60", APIMark_OpenArrow60 },
    { "ClosedArrow60", APIMark_ClosArrow60 }, { "FullArrow60", APIMark_FullArrow60 }, { "SlashLine75", APIMark_SlashLine75 }
};

template <size_t N>
bool GetNamedValue (const GS::ObjectState& item, const char* fieldName, const NamedValue (&values)[N], Int32& value)
{
    GS::UniString name;
    if (!item.Get (fieldName, name)) {
        return false;
    }
    for (const NamedValue& namedValue : values) {
        if (name == namedValue.name) {
            value = namedValue.value;
            return true;
        }
    }
    return false;
}


// The text settings of a dimension note. Returns whether any was given.
GS::Optional<GS::UniString> ApplyDimensionTextSettings (const GS::ObjectState& item, API_NoteType& note, bool& changed)
{
    if (item.Get ("textHeight", note.noteSize)) {
        changed = true;
    }
    Int32 textPen = 0;
    if (item.Get ("textPen", textPen)) {
        note.notePen = (short) textPen;
        changed = true;
    }
    GS::UniString fontName;
    if (item.Get ("textFont", fontName)) {
        const GS::Optional<short> fontIndex = GetFontIndexFromName (fontName);
        if (!fontIndex.HasValue ()) {
            return GS::UniString ("textFont does not name an available font.");
        }
        note.noteFont = *fontIndex;
        changed = true;
    }
    if (item.Get ("textFrame", note.frame)) {
        changed = true;
    }
    return {};
}

template <size_t N>
GS::UniString GetName (const NamedValue (&values)[N], Int32 value)
{
    for (const NamedValue& namedValue : values) {
        if (namedValue.value == value) {
            return namedValue.name;
        }
    }
    return "Unknown";
}

} // namespace

void AddDimensionNoteSettings (GS::ObjectState& os, const API_NoteType& note)
{
    os.Add ("textHeight", note.noteSize);
    os.Add ("textPen", (Int32) note.notePen);
    const GS::Optional<GS::UniString> fontName = GetFontNameFromIndex (note.noteFont);
    if (fontName.HasValue ()) {
        os.Add ("textFont", *fontName);
    }
    os.Add ("textFrame", note.frame);
}

GS::ObjectState CreateDimensionSettingsObjectState (const API_Element& element)
{
    const API_DimensionType& dim = element.dimension;
    GS::ObjectState os;
    os.Add ("appearance", GetName (DimensionAppearances, dim.dimAppear));
    os.Add ("linePen", (Int32) dim.linPen);
    os.Add ("markerType", GetName (DimensionMarkerTypes, dim.markerData.markerType));
    os.Add ("markerPen", (Int32) dim.markerData.markerPen);
    os.Add ("markerSize", dim.markerData.markerSize);
    os.Add ("witnessForm", GetName (DimensionWitnessForms, dim.defWitnessForm));
    os.Add ("witnessVal", dim.defWitnessVal);
    os.Add ("textPosition", GetName (DimensionTextPositions, dim.textPos));
    AddDimensionNoteSettings (os, dim.defNote);
    return os;
}

ModifyDimensionsCommand::ModifyDimensionsCommand () :
    ModifyElementsCommandBase ("ModifyDimensions", "dimensionsWithDetails", "Dimension")
{
}

bool ModifyDimensionsCommand::IsExpectedType (const API_Element& element) const
{
    return GetElemTypeId (element.header) == API_DimensionID;
}

GS::Optional<GS::UniString> ModifyDimensionsCommand::GetInputParametersSchema () const
{
    return BuildModifyElementsSchema ("dimensionsWithDetails",
        "The linear Dimension elements to modify, each with the fields to change: the dimension line and the text settings CreateAssociativeDimensions takes, and the appearance and text settings of the Dimension Settings, in the format GetDimensionData returns them in settings. Text and witness line settings apply to every witness point. The witness points themselves cannot be replaced.",
        false, false, GS::UniString (R"(
                        "referencePoint": {
                            "$ref": "#/Coordinate2D",
                            "description": "A point of the dimension line (the dimensionLinePosition GetDimensionData returns)."
                        },
                        "direction": {
                            "$ref": "#/Coordinate2D",
                            "description": "Direction of the dimension line. Must not be zero."
                        },
                        "appearance": {
                            "type": "string",
                            "enum": ["Linear", "Cumulative", "Baseline", "Elevation"],
                            "description": "Dimension type, as in the Dimension Settings. A dimension is not switched to or from Elevation: Archicad keeps the type then (tested in Archicad 30), so the item fails; Elevation is accepted for an Elevation dimension, so the settings GetDimensionData returns can be written back."
                        },
                        "linePen": {
                            "type": "integer",
                            "minimum": 1,
                            "maximum": 255,
                            "description": "Pen of the dimension and witness lines."
                        },
                        "markerType": {
                            "type": "string",
                            "enum": ["CrossLine", "EmptyCircle", "SlashLine", "OpenArrow30", "ClosedArrow30", "FullArrow30", "SlashLine45", "CrossCircle", "OpenArrow90", "ClosedArrow90", "FullArrow90", "FullCircle", "PepitaCircle", "BandArrow", "OpenArrow60", "ClosedArrow60", "FullArrow60", "SlashLine75"],
                            "description": "The marker drawn at the witness points."
                        },
                        "markerPen": {
                            "type": "integer",
                            "minimum": 1,
                            "maximum": 255,
                            "description": "Pen of the markers."
                        },
                        "markerSize": {
                            "type": "number",
                            "exclusiveMinimum": 0,
                            "description": "Size of the markers in millimeters (paper size)."
                        },
                        "witnessForm": {
                            "type": "string",
                            "enum": ["None", "Small", "Large", "Fix"],
                            "description": "Witness line form, as GetDimensionData returns it."
                        },
                        "witnessVal": {
                            "type": "number",
                            "minimum": 0,
                            "description": "Gap (Fix) or length (Large) of the witness lines, as GetDimensionData returns it."
                        },
                        "textPosition": {
                            "type": "string",
                            "enum": ["Above", "In", "Below"],
                            "description": "Position of the texts relative to the dimension line."
                        },
                        "textHeight": {
                            "type": "number",
                            "exclusiveMinimum": 0,
                            "description": "Height of the dimension texts in millimeters (paper size)."
                        },
                        "textPen": {
                            "type": "integer",
                            "minimum": 1,
                            "maximum": 255,
                            "description": "Pen of the dimension texts."
                        },
                        "textFont": {
                            "type": "string",
                            "description": "Font name of the dimension texts."
                        },
                        "textFrame": {
                            "type": "boolean",
                            "description": "Whether the dimension texts have a frame."
                        })"));
}

GS::Optional<GS::UniString> ModifyDimensionsCommand::ApplyTypeSpecificParameters (const GS::ObjectState& item, API_Element& element, API_Element& mask, API_ElementMemo& memo, UInt64& memoMask) const
{
    API_DimensionType& dim = element.dimension;

    if (HasCoordinate (item, "referencePoint", dim.refC)) {
        ACAPI_ELEMENT_MASK_SET (mask, API_DimensionType, refC);
    }
    const GS::ObjectState* directionOS = item.Get ("direction");
    if (directionOS != nullptr) {
        const API_Coord direction = Get2DCoordinateFromObjectState (*directionOS);
        if (std::hypot (direction.x, direction.y) < 1e-9) {
            return GS::UniString ("direction must not be zero.");
        }
        dim.direction.x = direction.x;
        dim.direction.y = direction.y;
        ACAPI_ELEMENT_MASK_SET (mask, API_DimensionType, direction);
    }

    Int32 value = 0;
    if (GetNamedValue (item, "appearance", DimensionAppearances, value)) {
        if ((value == APIApp_Elev) != (dim.dimAppear == APIApp_Elev)) {
            return GS::UniString ("A dimension is not switched to or from the Elevation type.");
        }
        dim.dimAppear = (API_AppearID) value;
        ACAPI_ELEMENT_MASK_SET (mask, API_DimensionType, dimAppear);
    }
    if (item.Get ("linePen", value)) {
        dim.linPen = (short) value;
        ACAPI_ELEMENT_MASK_SET (mask, API_DimensionType, linPen);
    }
    if (GetNamedValue (item, "markerType", DimensionMarkerTypes, value)) {
        dim.markerData.markerType = (API_MarkerID) value;
        ACAPI_ELEMENT_MASK_SET (mask, API_DimensionType, markerData.markerType);
    }
    if (item.Get ("markerPen", value)) {
        dim.markerData.markerPen = (short) value;
        ACAPI_ELEMENT_MASK_SET (mask, API_DimensionType, markerData.markerPen);
    }
    if (item.Get ("markerSize", dim.markerData.markerSize)) {
        ACAPI_ELEMENT_MASK_SET (mask, API_DimensionType, markerData.markerSize);
    }
    if (GetNamedValue (item, "textPosition", DimensionTextPositions, value)) {
        dim.textPos = (API_TextPosID) value;
        ACAPI_ELEMENT_MASK_SET (mask, API_DimensionType, textPos);
    }

    bool witnessChanged = false;
    if (GetNamedValue (item, "witnessForm", DimensionWitnessForms, value)) {
        dim.defWitnessForm = (API_WitnessID) value;
        ACAPI_ELEMENT_MASK_SET (mask, API_DimensionType, defWitnessForm);
        witnessChanged = true;
    }
    if (item.Get ("witnessVal", dim.defWitnessVal)) {
        ACAPI_ELEMENT_MASK_SET (mask, API_DimensionType, defWitnessVal);
        witnessChanged = true;
    }

    bool textChanged = false;
    auto textError = ApplyDimensionTextSettings (item, dim.defNote, textChanged);
    if (textError.HasValue ()) {
        return textError;
    }
    if (textChanged) {
        ACAPI_ELEMENT_MASK_SET (mask, API_DimensionType, defNote);
    }

    if (!textChanged && !witnessChanged) {
        return {};
    }

    // The witness points carry their own text and witness line settings, which the Dimension
    // Settings change together, so they are written with the memo, as the DevKit's Element_Test
    // does. Only their settings change, never their number or their points: a new set of witness
    // points made ACAPI_Element_Change fail with APIERR_BADPARS after it had deleted the
    // dimension (tested in Archicad 30).
    GSErrCode err = ACAPI_Element_GetMemo (element.header.guid, &memo);
    if (err != NoError || memo.dimElems == nullptr) {
        return GS::UniString ("Failed to read the witness points of the dimension.");
    }
    for (Int32 ii = 0; ii < dim.nDimElem; ++ii) {
        API_DimElem& dimElem = (*memo.dimElems)[ii];
        bool noteChanged = false;
        ApplyDimensionTextSettings (item, dimElem.note, noteChanged);
        if (witnessChanged) {
            dimElem.witnessForm = dim.defWitnessForm;
            dimElem.witnessVal = dim.defWitnessVal;
        }
    }
    memoMask = APIMemoMask_All;
    return {};
}

// ---------------------------------------------------------------------------
// ModifyRailings
// ---------------------------------------------------------------------------

ModifyRailingsCommand::ModifyRailingsCommand () :
    ModifyElementsCommandBase ("ModifyRailings", "railingsWithDetails", "Railing")
{
}

bool ModifyRailingsCommand::IsExpectedType (const API_Element& element) const
{
    return GetElemTypeId (element.header) == API_RailingID;
}

GS::Optional<GS::UniString> ModifyRailingsCommand::GetInputParametersSchema () const
{
    return BuildModifyElementsSchema ("railingsWithDetails",
        "The Railing elements to modify, each with the fields to change: the ones CreateRailings takes except the reference line, and the segment heights GetDetailsOfElements returns.",
        true, true, R"(
                        "bottomOffset": {
                            "type": "number",
                            "description": "Base height of the railing relative to its home story."
                        },
                        "height": {
                            "type": "number",
                            "exclusiveMinimum": 0,
                            "description": "Height of every segment of the railing, and of the segments added later (the default segment)."
                        },
                        "segmentHeights": {
                            "type": "array",
                            "description": "The height of each segment, in the order GetDetailsOfElements returns segmentHeights. Applied after height.",
                            "items": {
                                "type": "number",
                                "exclusiveMinimum": 0
                            }
                        })");
}

GS::Optional<GS::UniString> ModifyRailingsCommand::ApplyTypeSpecificParameters (const GS::ObjectState& item, API_Element& element, API_Element& mask, API_ElementMemo& /*memo*/, UInt64& /*memoMask*/) const
{
    if (item.Get ("bottomOffset", element.railing.bottomOffset)) {
        ACAPI_ELEMENT_MASK_SET (mask, API_RailingType, bottomOffset);
    }
    if (item.Get ("height", element.railing.defSegment.height)) {
        ACAPI_ELEMENT_MASK_SET (mask, API_RailingType, defSegment.height);
    }

    return {};
}

bool ModifyRailingsCommand::HasAfterChangeFields (const GS::ObjectState& item) const
{
    GS::Array<double> segmentHeights;
    return item.Get ("segmentHeights", segmentHeights);
}

GS::Optional<GS::UniString> ModifyRailingsCommand::AfterChange (const GS::ObjectState& item, const API_Element& element) const
{
    double height = 0.0;
    const bool hasHeight = item.Get ("height", height);
    GS::Array<double> segmentHeights;
    const bool hasSegmentHeights = item.Get ("segmentHeights", segmentHeights);
    if (!hasHeight && !hasSegmentHeights) {
        return {};
    }

    // The segments are subelements: each is changed as an element of its own, as the DevKit's
    // Element_Test changes a railing node.
    // The railing as it is after the change: a new reference line has new segments.
    API_Element railing = {};
    railing.header.guid = element.header.guid;
    API_ElementMemo memo = {};
    const GS::OnExit guard ([&memo] () { ACAPI_DisposeElemMemoHdls (&memo); });
    GSErrCode err = ACAPI_Element_Get (&railing);
    if (err == NoError) {
        err = ACAPI_Element_GetMemo (railing.header.guid, &memo, APIMemoMask_RailingSegment);
    }
    if (err != NoError || memo.railingSegments == nullptr) {
        return GS::UniString ("its segments could not be read.");
    }
    const GSSize nSegments = (GSSize) railing.railing.nSegments;
    if (hasSegmentHeights && (GSSize) segmentHeights.GetSize () != nSegments) {
        return GS::UniString::Printf ("segmentHeights has %u heights for %d segments.", segmentHeights.GetSize (), (int) nSegments);
    }
    for (GSSize ii = 0; ii < nSegments; ++ii) {
        API_Element segment = {};
        segment.header.guid = memo.railingSegments[ii].head.guid;
        err = ACAPI_Element_Get (&segment);
        if (err != NoError) {
            return GS::UniString ("a segment could not be read.");
        }
        segment.railingSegment.height = hasSegmentHeights ? segmentHeights[(UIndex) ii] : height;
        API_Element segmentMask = {};
        ACAPI_ELEMENT_MASK_CLEAR (segmentMask);
        ACAPI_ELEMENT_MASK_SET (segmentMask, API_RailingSegmentType, height);
        err = ACAPI_Element_Change (&segment, &segmentMask, nullptr, 0, true);
        if (err != NoError) {
            return GS::UniString ("Archicad refused the height of a segment.");
        }
    }
    return {};
}

// ---------------------------------------------------------------------------
// ModifyStairs
// ---------------------------------------------------------------------------

ModifyStairsCommand::ModifyStairsCommand () :
    ModifyElementsCommandBase ("ModifyStairs", "stairsWithDetails", "Stair")
{
}

bool ModifyStairsCommand::IsExpectedType (const API_Element& element) const
{
    return GetElemTypeId (element.header) == API_StairID;
}

GS::Optional<GS::UniString> ModifyStairsCommand::GetInputParametersSchema () const
{
    return BuildModifyElementsSchema ("stairsWithDetails",
        "The Stair elements to modify, each with the fields to change: the ones CreateStairs takes except the baseline, the story, treadDepth and finishVisible, which Archicad does not change through ACAPI_Element_Change (tested in Archicad 30). Archicad applies the stair rules of the project to them as in the Stair Settings (a new number of risers also changes the length of the flight), so read the result back with GetDetailsOfElements.",
        true, false, R"(
                        "zCoordinate": {
                            "type": "number",
                            "description": "The new absolute elevation of the stair base. The stair stays on its home story: Archicad does not move a stair to another story with ACAPI_Element_Change (tested in Archicad 30), so the elevation becomes the base offset from that story."
                        },
                        "totalHeight": {
                            "type": "number",
                            "exclusiveMinimum": 0,
                            "description": "Total height of the stair."
                        },
                        "flightWidth": {
                            "type": "number",
                            "exclusiveMinimum": 0,
                            "description": "Width of the stair flight."
                        },
                        "stepNum": {
                            "type": "integer",
                            "minimum": 1,
                            "description": "Number of risers (steps)."
                        },
                        "riserHeight": {
                            "type": "number",
                            "exclusiveMinimum": 0,
                            "description": "Height of each riser. Archicad keeps the riser height at the total height divided by the number of risers, so without stepNum it sets the number of risers that comes closest to this height, as the Stair Settings do."
                        })");
}

GS::Optional<GS::UniString> ModifyStairsCommand::ApplyTypeSpecificParameters (const GS::ObjectState& item, API_Element& element, API_Element& mask, API_ElementMemo& /*memo*/, UInt64& /*memoMask*/) const
{
    API_StairType& stair = element.stair;

    double zCoordinate = 0.0;
    if (item.Get ("zCoordinate", zCoordinate)) {
        stair.basePlane.basePoint.z = zCoordinate - GetZPos (element.header.floorInd, 0.0, GetStories ());
        ACAPI_ELEMENT_MASK_SET (mask, API_StairType, basePlane);
    }
    if (item.Get ("totalHeight", stair.totalHeight)) {
        ACAPI_ELEMENT_MASK_SET (mask, API_StairType, totalHeight);
    }
    if (item.Get ("flightWidth", stair.flightWidth)) {
        ACAPI_ELEMENT_MASK_SET (mask, API_StairType, flightWidth);
    }
    Int32 stepNum = 0;
    if (item.Get ("stepNum", stepNum)) {
        stair.stepNum = (UInt32) stepNum;
        ACAPI_ELEMENT_MASK_SET (mask, API_StairType, stepNum);
    }
    double riserHeight = 0.0;
    if (item.Get ("riserHeight", riserHeight)) {
        // Archicad derives the riser height from the total height and the number of risers and
        // ignores a riser height of its own (tested in Archicad 30), so a riser height alone
        // gives the number of risers.
        stair.riserHeight = riserHeight;
        ACAPI_ELEMENT_MASK_SET (mask, API_StairType, riserHeight);
        if (!item.Get ("stepNum", stepNum)) {
            const double risers = std::round (stair.totalHeight / riserHeight);
            stair.stepNum = (UInt32) (risers < 1.0 ? 1.0 : risers);
            ACAPI_ELEMENT_MASK_SET (mask, API_StairType, stepNum);
        }
    }
    return {};
}
