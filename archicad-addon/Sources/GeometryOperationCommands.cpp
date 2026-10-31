#include "GeometryOperationCommands.hpp"

#include "MigrationHelper.hpp"
#include "OnExit.hpp"
#ifdef ServerMainVers_2900
#include "Polygon2D.hpp"
#endif

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

// The Archicad API has no function for Split, Intersect, Adjust, Fillet, Chamfer or Offset
// Edge: ACAPI_Element_Edit only knows the transformations of API_EditCmdID (drag, rotate,
// mirror, elevate, stretch, resize), and ACAPI_Element_Trim_* only trims construction elements
// with roofs and shells. So these commands compute the new shapes themselves: an element keeps
// its guid and gets the first piece with ACAPI_Element_Change, and every other piece is a new
// element made from the original's settings (ACAPI_Element_Get + ACAPI_Element_GetMemo +
// ACAPI_Element_Create), which then gets the original's classifications, category values,
// user-defined property values and Element ID. Each command runs in one undoable step.

namespace {

constexpr double LengthEps = 1e-6;
constexpr double AngleEps = 1e-9;
constexpr double Pi = 3.14159265358979323846;

// ---------------------------------------------------------------------------------------------
// Plane geometry
// ---------------------------------------------------------------------------------------------

inline API_Coord MakeCoord (double x, double y)
{
    API_Coord c;
    c.x = x;
    c.y = y;
    return c;
}

inline API_Coord Plus (const API_Coord& a, const API_Coord& b)  { return MakeCoord (a.x + b.x, a.y + b.y); }
inline API_Coord Minus (const API_Coord& a, const API_Coord& b) { return MakeCoord (a.x - b.x, a.y - b.y); }
inline API_Coord Times (const API_Coord& a, double s)           { return MakeCoord (a.x * s, a.y * s); }
inline double    Dot (const API_Coord& a, const API_Coord& b)   { return a.x * b.x + a.y * b.y; }
inline double    Cross (const API_Coord& a, const API_Coord& b) { return a.x * b.y - a.y * b.x; }
inline double    Length (const API_Coord& a)                    { return std::sqrt (Dot (a, a)); }
inline double    Distance (const API_Coord& a, const API_Coord& b) { return Length (Minus (a, b)); }
inline double    Lerp (double a, double b, double t)             { return a + (b - a) * t; }

inline API_Coord Normalized (const API_Coord& a)
{
    const double len = Length (a);
    return len > 0.0 ? Times (a, 1.0 / len) : a;
}

inline API_Coord Rotated (const API_Coord& v, double angle)
{
    const double c = std::cos (angle);
    const double s = std::sin (angle);
    return MakeCoord (v.x * c - v.y * s, v.x * s + v.y * c);
}

// The intersection of the infinite lines p1 + t * d1 and p2 + s * d2.
bool IntersectLines (const API_Coord& p1, const API_Coord& d1, const API_Coord& p2, const API_Coord& d2, API_Coord& result)
{
    const double den = Cross (d1, d2);
    if (std::abs (den) <= 1e-12 * Length (d1) * Length (d2)) {
        return false;
    }
    const double t = Cross (Minus (p2, p1), d2) / den;
    result = Plus (p1, Times (d1, t));
    return true;
}

double DistanceFromLine (const API_Coord& point, const API_Coord& linePoint, const API_Coord& lineDir)
{
    return std::abs (Cross (Minus (point, linePoint), lineDir)) / Length (lineDir);
}

// A straight segment (angle == 0) or a circular arc from beg to end. The angle is the signed
// central angle of the arc: positive when the arc runs counterclockwise from beg to end, which
// puts it on the right-hand side of the chord, the convention of API_PolyArc::arcAngle.
struct Curve {
    API_Coord beg;
    API_Coord end;
    double    angle  = 0.0;
    API_Coord center = { 0.0, 0.0 };
    double    radius = 0.0;
};

inline bool IsArc (const Curve& curve)
{
    return std::abs (curve.angle) > AngleEps;
}

Curve MakeCurve (const API_Coord& beg, const API_Coord& end, double angle)
{
    Curve curve;
    curve.beg = beg;
    curve.end = end;
    curve.angle = angle;
    if (IsArc (curve)) {
        const API_Coord chord = Minus (end, beg);
        const double chordLength = Length (chord);
        const API_Coord mid = Times (Plus (beg, end), 0.5);
        const API_Coord leftNormal = MakeCoord (-chord.y / chordLength, chord.x / chordLength);
        curve.center = Plus (mid, Times (leftNormal, (chordLength / 2.0) / std::tan (angle / 2.0)));
        curve.radius = (chordLength / 2.0) / std::abs (std::sin (angle / 2.0));
    }
    return curve;
}

Curve MakeArcFromCenter (const API_Coord& center, double radius, double begAngle, double sweep)
{
    Curve curve;
    curve.center = center;
    curve.radius = radius;
    curve.angle = sweep;
    curve.beg = Plus (center, MakeCoord (radius * std::cos (begAngle), radius * std::sin (begAngle)));
    curve.end = Plus (center, MakeCoord (radius * std::cos (begAngle + sweep), radius * std::sin (begAngle + sweep)));
    return curve;
}

double CurveLength (const Curve& curve)
{
    return IsArc (curve) ? curve.radius * std::abs (curve.angle) : Distance (curve.beg, curve.end);
}

API_Coord PointAt (const Curve& curve, double fraction)
{
    if (IsArc (curve)) {
        return Plus (curve.center, Rotated (Minus (curve.beg, curve.center), curve.angle * fraction));
    }
    return MakeCoord (Lerp (curve.beg.x, curve.end.x, fraction), Lerp (curve.beg.y, curve.end.y, fraction));
}

Curve SubCurve (const Curve& curve, double from, double to)
{
    Curve sub = curve;
    sub.beg = PointAt (curve, from);
    sub.end = PointAt (curve, to);
    sub.angle = curve.angle * (to - from);
    return sub;
}

Curve Reversed (const Curve& curve)
{
    Curve reversed = curve;
    reversed.beg = curve.end;
    reversed.end = curve.beg;
    reversed.angle = -curve.angle;
    return reversed;
}

// The fractions (0 < f < 1, along the length) where the curve crosses the infinite line through
// linePoint in lineDir, in increasing order. Touching the line and meeting it at an end point
// are not crossings.
std::vector<double> LineCrossings (const Curve& curve, const API_Coord& linePoint, const API_Coord& lineDir)
{
    std::vector<double> fractions;
    const double length = CurveLength (curve);
    if (length <= LengthEps) {
        return fractions;
    }
    const API_Coord dir = Normalized (lineDir);
    auto addIfInside = [&] (double f) {
        if (f * length > LengthEps && (1.0 - f) * length > LengthEps) {
            fractions.push_back (f);
        }
    };

    if (!IsArc (curve)) {
        const API_Coord segment = Minus (curve.end, curve.beg);
        const double den = Cross (segment, dir);
        if (std::abs (den) > 1e-12 * length) {
            addIfInside (Cross (Minus (linePoint, curve.beg), dir) / den);
        }
    } else {
        const API_Coord w = Minus (linePoint, curve.center);
        const double b = Dot (w, dir);
        const double c = Dot (w, w) - curve.radius * curve.radius;
        const double disc = b * b - c;
        if (disc > LengthEps * LengthEps) {
            const double root = std::sqrt (disc);
            const API_Coord v0 = Minus (curve.beg, curve.center);
            for (double s : { -b - root, -b + root }) {
                const API_Coord v = Minus (Plus (linePoint, Times (dir, s)), curve.center);
                double phi = std::atan2 (Cross (v0, v), Dot (v0, v));
                if (curve.angle > 0.0 && phi < 0.0) {
                    phi += 2.0 * Pi;
                } else if (curve.angle < 0.0 && phi > 0.0) {
                    phi -= 2.0 * Pi;
                }
                const double f = phi / curve.angle;
                if (f < 1.0) {
                    addIfInside (f);
                }
            }
        }
    }

    std::sort (fractions.begin (), fractions.end ());
    return fractions;
}

// ---------------------------------------------------------------------------------------------
// Elements as curves
// ---------------------------------------------------------------------------------------------

inline void SetElemType (API_Elem_Head& header, API_ElemTypeID typeID)
{
#ifdef ServerMainVers_2600
    header.type = typeID;
#else
    header.typeID = typeID;
#endif
}

// API_Element is a union, and "= {}" only clears its first member, the header: the other bytes
// keep what was in the memory before. ACAPI_Element_Get writes through pointers in the type
// specific part (the area text of a hatch, API_NoteType::contentUStr, crashed Archicad 30 when
// it was not null), so an element to read is cleared whole first.
inline void ClearElement (API_Element& element, const API_Guid& guid = APINULLGuid)
{
    BNZeroMemory (&element, sizeof (API_Element));
    element.header.guid = guid;
}

GS::UniString ElemTypeName (const API_Elem_Head& header)
{
    return GetElementTypeNonLocalizedName (GetElemTypeId (header));
}

// The reference line of a wall or a beam, a line, or a circular arc as a curve. Returns the
// reason when the element has no such curve.
GS::Optional<GS::UniString> GetCurveOfElement (const API_Element& element, Curve& curve)
{
    switch (GetElemTypeId (element.header)) {
        case API_WallID:
            if (element.wall.type == APIWtyp_Poly) {
                return GS::UniString ("Polygonal walls have no reference line to work with.");
            }
            curve = MakeCurve (element.wall.begC, element.wall.endC, element.wall.angle);
            return GS::NoValue;
        case API_BeamID:
            if (element.beam.beamShape == API_VerticallyCurvedBeam) {
                return GS::UniString ("Vertically curved beams are not supported.");
            }
            curve = MakeCurve (element.beam.begC, element.beam.endC, element.beam.beamShape == API_HorizontallyCurvedBeam ? element.beam.curveAngle : 0.0);
            return GS::NoValue;
        case API_LineID:
            curve = MakeCurve (element.line.begC, element.line.endC, 0.0);
            return GS::NoValue;
        case API_ArcID: {
            if (std::abs (element.arc.ratio - 1.0) > 1e-9) {
                return GS::UniString ("Elliptical arcs are not supported, only circular ones.");
            }
            double sweep = element.arc.endAng - element.arc.begAng;
            while (sweep <= AngleEps) {
                sweep += 2.0 * Pi;
            }
            while (sweep > 2.0 * Pi + AngleEps) {
                sweep -= 2.0 * Pi;
            }
            curve = MakeArcFromCenter (element.arc.origC, element.arc.r, element.arc.begAng, sweep);
            return GS::NoValue;
        }
        default:
            return GS::UniString::Printf ("Elements of type %T are not supported, only walls, beams, lines and arcs.", ElemTypeName (element.header).ToPrintf ());
    }
}

// Writes the curve into the element (and its mask). A slanted beam keeps its slope: its level,
// the height at the beginning point, follows a beginning point that moves along the beam.
void SetCurveOfElement (API_Element& element, API_Element& mask, const Curve& curve)
{
    switch (GetElemTypeId (element.header)) {
        case API_WallID:
            element.wall.begC = curve.beg;
            element.wall.endC = curve.end;
            element.wall.angle = IsArc (curve) ? curve.angle : 0.0;
            ACAPI_ELEMENT_MASK_SET (mask, API_WallType, begC);
            ACAPI_ELEMENT_MASK_SET (mask, API_WallType, endC);
            ACAPI_ELEMENT_MASK_SET (mask, API_WallType, angle);
            break;
        case API_BeamID: {
            if (element.beam.isSlanted && !IsArc (curve)) {
                const API_Coord axis = Minus (element.beam.endC, element.beam.begC);
                const double axisLength = Length (axis);
                if (axisLength > LengthEps) {
                    const double shift = Dot (Minus (curve.beg, element.beam.begC), axis) / axisLength;
                    element.beam.level += shift * std::tan (element.beam.slantAngle);
                    ACAPI_ELEMENT_MASK_SET (mask, API_BeamType, level);
                }
            }
            element.beam.begC = curve.beg;
            element.beam.endC = curve.end;
            element.beam.curveAngle = IsArc (curve) ? curve.angle : 0.0;
            element.beam.beamShape = IsArc (curve) ? API_HorizontallyCurvedBeam : API_StraightBeam;
            ACAPI_ELEMENT_MASK_SET (mask, API_BeamType, begC);
            ACAPI_ELEMENT_MASK_SET (mask, API_BeamType, endC);
            ACAPI_ELEMENT_MASK_SET (mask, API_BeamType, curveAngle);
            ACAPI_ELEMENT_MASK_SET (mask, API_BeamType, beamShape);
        } break;
        case API_LineID:
            element.line.begC = curve.beg;
            element.line.endC = curve.end;
            ACAPI_ELEMENT_MASK_SET (mask, API_LineType, begC);
            ACAPI_ELEMENT_MASK_SET (mask, API_LineType, endC);
            break;
        case API_ArcID: {
            // An arc element always runs counterclockwise from begAng to endAng.
            const Curve ccw = curve.angle > 0.0 ? curve : Reversed (curve);
            element.arc.origC = ccw.center;
            element.arc.r = ccw.radius;
            element.arc.begAng = std::atan2 (ccw.beg.y - ccw.center.y, ccw.beg.x - ccw.center.x);
            element.arc.endAng = element.arc.begAng + ccw.angle;
            ACAPI_ELEMENT_MASK_SET (mask, API_ArcType, origC);
            ACAPI_ELEMENT_MASK_SET (mask, API_ArcType, r);
            ACAPI_ELEMENT_MASK_SET (mask, API_ArcType, begAng);
            ACAPI_ELEMENT_MASK_SET (mask, API_ArcType, endAng);
        } break;
        default:
            break;
    }
}

// The arrow heads of lines, arcs and polylines stay on the outer ends of the pieces only.
// The two arrows have their own settings from Archicad 29; before that the element keeps them.
void HideInnerArrows (API_Element& element, API_Element& mask, bool keepStart, bool keepEnd)
{
#ifdef ServerMainVers_2900
    // Archicad only takes a change of the arrow data when all its fields are masked (see
    // ApplyLabelLeaderLineSettableDetails), so the unchanged ones are masked with their own values.
#define TAPIR_HIDE_ARROW(TYPE, MEMBER, ARROW) \
    element.MEMBER.ARROW.arrowVisibility = false; \
    ACAPI_ELEMENT_MASK_SET (mask, TYPE, ARROW.arrowType); \
    ACAPI_ELEMENT_MASK_SET (mask, TYPE, ARROW.arrowVisibility); \
    ACAPI_ELEMENT_MASK_SET (mask, TYPE, ARROW.arrowPen); \
    ACAPI_ELEMENT_MASK_SET (mask, TYPE, ARROW.arrowSize);
    switch (GetElemTypeId (element.header)) {
        case API_LineID:
            if (!keepStart) { TAPIR_HIDE_ARROW (API_LineType, line, startArrowData) }
            if (!keepEnd)   { TAPIR_HIDE_ARROW (API_LineType, line, endArrowData) }
            break;
        case API_ArcID:
            if (!keepStart) { TAPIR_HIDE_ARROW (API_ArcType, arc, startArrowData) }
            if (!keepEnd)   { TAPIR_HIDE_ARROW (API_ArcType, arc, endArrowData) }
            break;
        case API_PolyLineID:
            if (!keepStart) { TAPIR_HIDE_ARROW (API_PolyLineType, polyLine, startArrowData) }
            if (!keepEnd)   { TAPIR_HIDE_ARROW (API_PolyLineType, polyLine, endArrowData) }
            break;
        default:
            break;
    }
#undef TAPIR_HIDE_ARROW
#else
    UNUSED_PARAMETER (element);
    UNUSED_PARAMETER (mask);
    UNUSED_PARAMETER (keepStart);
    UNUSED_PARAMETER (keepEnd);
#endif
}

// ---------------------------------------------------------------------------------------------
// Checks before an element is changed
// ---------------------------------------------------------------------------------------------

// ACAPI_Element_Edit skips an element it may not edit without an error, and the changes here
// should not start on an element that cannot take all of them, so the reasons are checked first.
GS::Optional<GS::ObjectState> CheckEditable (const API_Guid& guid, const char* operation)
{
    API_Elem_Head header = {};
    if (!LoadElementHeaderByGuid (guid, header)) {
        return CreateErrorResponse (APIERR_BADID, "Element does not exist.");
    }

    API_Attribute layer = {};
    layer.header.typeID = API_LayerID;
    layer.header.index = header.layer;
    if (ACAPI_Attribute_Get (&layer) == NoError) {
        if ((layer.header.flags & APILay_Locked) != 0) {
            return CreateErrorResponse (APIERR_LOCKEDLAY, GS::UniString::Printf ("The element was not %s, because its layer is locked.", operation));
        }
        if ((layer.header.flags & APILay_Hidden) != 0) {
            return CreateErrorResponse (APIERR_HIDDENLAY, GS::UniString::Printf ("The element was not %s, because its layer is hidden.", operation));
        }
    }

    if (!ACAPI_Element_Filter (guid, APIFilt_IsEditable)) {
        return CreateErrorResponse (APIERR_GENERAL, GS::UniString::Printf ("The element was not %s, because it is not editable.", operation));
    }

    // New pieces are created in the current database, so the element must be there. Floor plan
    // elements are handled by FloorPlanDatabaseScope.
    API_DatabaseInfo currentDb = {};
    API_DatabaseInfo elemDb = {};
    if (ACAPI_Database_GetCurrentDatabase (&currentDb) == NoError &&
        ACAPI_Database_GetContainingDatabase (&guid, &elemDb) == NoError &&
        ACAPI_Window_GetDatabaseInfo (&elemDb) == NoError &&
        elemDb.typeID != APIWind_FloorPlanID)
    {
        const DatabaseIdResolver& resolver = DatabaseIdResolver::Instance ();
        if (resolver.GetIdOfDatabase (elemDb) != resolver.GetIdOfDatabase (currentDb)) {
            return CreateErrorResponse (APIERR_BADDATABASE,
                GS::UniString::Printf ("The element was not %s, because it is in another database (%T) than the current one. Activate its window first (see GetContainingDatabases and ChangeWindow).",
                    operation, GS::UniString (elemDb.name).ToPrintf ()));
        }
    }

    return GS::NoValue;
}

// Makes the floor plan the current database for the lifetime of the object when one of the
// elements lives on the floor plan and another database (a section, the 3D window ...) is
// current, so new pieces land next to their originals. Puts the previous database back.
class FloorPlanDatabaseScope {
public:
    explicit FloorPlanDatabaseScope (const GS::Array<API_Guid>& guids)
    {
        for (const API_Guid& guid : guids) {
            API_DatabaseInfo elemDb = {};
            if (ACAPI_Database_GetContainingDatabase (&guid, &elemDb) == NoError &&
                ACAPI_Window_GetDatabaseInfo (&elemDb) == NoError &&
                elemDb.typeID == APIWind_FloorPlanID)
            {
                SwitchCurrentDatabaseToFloorPlan (previousDatabase, switched);
                return;
            }
        }
    }

    ~FloorPlanDatabaseScope ()
    {
        if (switched) {
            ACAPI_Database_ChangeCurrentDatabase (&previousDatabase);
        }
    }

    FloorPlanDatabaseScope (const FloorPlanDatabaseScope&) = delete;
    FloorPlanDatabaseScope& operator= (const FloorPlanDatabaseScope&) = delete;

private:
    API_DatabaseInfo previousDatabase = {};
    bool switched = false;
};

// ---------------------------------------------------------------------------------------------
// Copies of elements
// ---------------------------------------------------------------------------------------------

// Everything a new piece takes over from the original: its settings and geometry (element and
// memo) and what the API keeps apart from them. Read before the original is changed.
struct ElementData {
    API_Guid                                guid = APINULLGuid;
    API_Element                             element = {};
    API_ElementMemo                         memo = {};
    GS::Array<API_Property>                 properties;
    GS::Array<GS::Pair<API_Guid, API_Guid>> classifications;
    GS::Array<API_ElemCategoryValue>        categoryValues;
    GS::Optional<GS::UniString>             id;

    ElementData () = default;
    ~ElementData () { ACAPI_DisposeElemMemoHdls (&memo); }
    ElementData (const ElementData&) = delete;
    ElementData& operator= (const ElementData&) = delete;
};

GSErrCode ReadElementData (const API_Guid& guid, ElementData& data)
{
    data.guid = guid;
    ClearElement (data.element, guid);
    GSErrCode err = ACAPI_Element_Get (&data.element);
    if (err != NoError) {
        return err;
    }

    if (ACAPI_Element_GetMemo (guid, &data.memo, APIMemoMask_All) == NoError && data.memo.elemInfoString != nullptr) {
        data.id = *data.memo.elemInfoString;
    }

    ACAPI_Element_GetClassificationItems (guid, data.classifications);

    GS::Array<API_ElemCategory> categories;
    if (TAPIR_GetElementCategories (categories) == NoError) {
        for (const API_ElemCategory& category : categories) {
            API_ElemCategoryValue value = {};
            if (TAPIR_Element_GetCategoryValue (guid, category, value) == NoError) {
                data.categoryValues.Push (value);
            }
        }
    }

    GS::Array<API_PropertyDefinition> definitions;
    if (ACAPI_Element_GetPropertyDefinitions (guid, API_PropertyDefinitionFilter_UserDefined, definitions) == NoError &&
        ACAPI_Element_GetPropertyValues (guid, definitions, data.properties) == NoError)
    {
        for (UIndex i = data.properties.GetSize (); i >= 1; --i) {
            const API_Property& property = data.properties[i - 1];
            if (property.isDefault || !property.definition.canValueBeEditable || property.status != API_Property_HasValue) {
                data.properties.Delete (i - 1);
            }
        }
    } else {
        data.properties.Clear ();
    }

    return NoError;
}

// Gives the target the classifications, category values, property values and Element ID of the
// element the data was read from. A value the target cannot take (an item not available for its
// type, a property not available for it) is left out, the rest is copied.
void CopyElementDataTo (const ElementData& data, const API_Guid& target)
{
    for (const GS::Pair<API_Guid, API_Guid>& systemAndItem : data.classifications) {
        ACAPI_Element_AddClassificationItem (target, systemAndItem.second);
    }
    GS::Array<GS::Pair<API_Guid, API_Guid>> targetClassifications;
    if (ACAPI_Element_GetClassificationItems (target, targetClassifications) == NoError) {
        for (const GS::Pair<API_Guid, API_Guid>& targetItem : targetClassifications) {
            bool systemOfSource = false;
            for (const GS::Pair<API_Guid, API_Guid>& sourceItem : data.classifications) {
                systemOfSource = systemOfSource || sourceItem.first == targetItem.first;
            }
            if (!systemOfSource) {
                ACAPI_Element_RemoveClassificationItem (target, targetItem.second);
            }
        }
    }

    for (const API_ElemCategoryValue& value : data.categoryValues) {
        TAPIR_Element_SetCategoryValue (target, value);
    }

    for (const API_Property& property : data.properties) {
        ACAPI_Element_SetProperty (target, property);
    }

    if (data.id.HasValue ()) {
        ChangeElementId (target, *data.id);
    }
}

// Creates a new element from `element` (the original's settings with the new geometry) and the
// memo, and copies the original's other data onto it.
GSErrCode CreateCopy (const ElementData& data, API_Element& element, API_ElementMemo* memo, API_Guid& newGuid)
{
    element.header.guid = APINULLGuid;
    const GSErrCode err = ACAPI_Element_Create (&element, memo);
    if (err != NoError) {
        return err;
    }
    newGuid = element.header.guid;
    CopyElementDataTo (data, newGuid);
    return NoError;
}

GS::ObjectState CreateElementsWrapper (const std::vector<API_Guid>& guids)
{
    GS::ObjectState result;
    const auto& list = result.AddList<GS::ObjectState> ("elements");
    for (const API_Guid& guid : guids) {
        list (CreateElementIdObjectState (guid));
    }
    return result;
}

template <typename T>
USize PtrItemCount (const T* ptr)
{
    return ptr == nullptr ? 0 : (USize) (BMGetPtrSize (reinterpret_cast<GSConstPtr> (ptr)) / sizeof (T));
}

template <typename T>
USize HandleItemCount (T** handle)
{
    return handle == nullptr ? 0 : (USize) (BMGetHandleSize (reinterpret_cast<GSConstHandle> (handle)) / sizeof (T));
}

// ---------------------------------------------------------------------------------------------
// Split
// ---------------------------------------------------------------------------------------------

// The index of the piece (between bounds[k] and bounds[k + 1], fractions of the length) a
// distance from the beginning falls into.
size_t PieceOfDistance (const std::vector<double>& bounds, double length, double distance)
{
    for (size_t k = bounds.size () - 1; k-- > 0;) {
        if (distance >= bounds[k] * length - LengthEps) {
            return k;
        }
    }
    return 0;
}

// The fields of a piece besides its curve: the thickness of a trapezoid wall along its length,
// and the arrows of a line or an arc.
void ApplyPieceDetails (API_Element& element, API_Element& mask, const API_Element& original, double from, double to, bool isFirst, bool isLast)
{
    switch (GetElemTypeId (element.header)) {
        case API_WallID:
            if (original.wall.type == APIWtyp_Trapez) {
                element.wall.thickness = Lerp (original.wall.thickness, original.wall.thickness1, from);
                element.wall.thickness1 = Lerp (original.wall.thickness, original.wall.thickness1, to);
                ACAPI_ELEMENT_MASK_SET (mask, API_WallType, thickness);
                ACAPI_ELEMENT_MASK_SET (mask, API_WallType, thickness1);
            }
            break;
        case API_LineID:
        case API_ArcID:
            HideInnerArrows (element, mask, isFirst, isLast);
            break;
        default:
            break;
    }
}

// The holes of a beam piece (their position counts from the piece's own beginning) and its end
// cuts: the outer ends keep the original's cuts, the new ends get the plain cut of the original.
void SetBeamPieceMemo (API_ElementMemo& memo, const std::vector<API_Beam_Hole>& holes, const API_AssemblySegmentCutData& begCut,
                       const API_AssemblySegmentCutData& endCut, const API_AssemblySegmentCutData& innerCut,
                       double from, double to, double length, bool isFirst, bool isLast)
{
    std::vector<API_Beam_Hole> pieceHoles;
    for (const API_Beam_Hole& hole : holes) {
        if (hole.centerx >= from * length - LengthEps && (isLast || hole.centerx < to * length - LengthEps)) {
            API_Beam_Hole pieceHole = hole;
            pieceHole.centerx -= from * length;
            pieceHoles.push_back (pieceHole);
        }
    }
    if (memo.beamHoles != nullptr) {
        BMKillHandle (reinterpret_cast<GSHandle*> (&memo.beamHoles));
    }
    memo.beamHoles = reinterpret_cast<API_Beam_Hole**> (BMAllocateHandle ((GSSize) (pieceHoles.size () * sizeof (API_Beam_Hole)), ALLOCATE_CLEAR, 0));
    for (size_t i = 0; i < pieceHoles.size (); ++i) {
        (*memo.beamHoles)[i] = pieceHoles[i];
    }

    const USize nCuts = PtrItemCount (memo.assemblySegmentCuts);
    if (nCuts >= 2) {
        memo.assemblySegmentCuts[0] = isFirst ? begCut : innerCut;
        memo.assemblySegmentCuts[nCuts - 1] = isLast ? endCut : innerCut;
    }
}

GS::ObjectState SplitCurveElement (ElementData& data, const API_Coord& linePoint, const API_Coord& lineDir)
{
    const API_ElemTypeID typeID = GetElemTypeId (data.element.header);
    Curve curve;
    const GS::Optional<GS::UniString> curveError = GetCurveOfElement (data.element, curve);
    if (curveError.HasValue ()) {
        return CreateErrorResponse (APIERR_BADELEMENTTYPE, *curveError);
    }

    const std::vector<double> crossings = LineCrossings (curve, linePoint, lineDir);
    if (crossings.empty ()) {
        return CreateErrorResponse (APIERR_BADPARS, "The split line does not cross the element.");
    }
    std::vector<double> bounds = { 0.0 };
    bounds.insert (bounds.end (), crossings.begin (), crossings.end ());
    bounds.push_back (1.0);
    const size_t nPieces = bounds.size () - 1;
    const double length = CurveLength (curve);

    // Beams: the holes and the end cuts are divided between the pieces.
    std::vector<API_Beam_Hole> holes;
    API_AssemblySegmentCutData begCut = {};
    API_AssemblySegmentCutData endCut = {};
    API_AssemblySegmentCutData innerCut = {};
    if (typeID == API_BeamID) {
        if (data.element.beam.nSegments != 1 || PtrItemCount (data.memo.beamSegments) != 1) {
            return CreateErrorResponse (APIERR_BADELEMENTTYPE, "Only beams of one segment can be split.");
        }
        const USize nCuts = PtrItemCount (data.memo.assemblySegmentCuts);
        if (nCuts < 2) {
            return CreateErrorResponse (APIERR_GENERAL, "The end cuts of the beam could not be read.");
        }
        begCut = data.memo.assemblySegmentCuts[0];
        endCut = data.memo.assemblySegmentCuts[nCuts - 1];
        innerCut = begCut.cutType != APIAssemblySegmentCut_Custom ? begCut : endCut;
        if (innerCut.cutType == APIAssemblySegmentCut_Custom) {
            innerCut.cutType = APIAssemblySegmentCut_Vertical;
            innerCut.customAngle = 0.0;
        }
        const USize nHoles = HandleItemCount (data.memo.beamHoles);
        for (USize i = 0; i < nHoles; ++i) {
            holes.push_back ((*data.memo.beamHoles)[i]);
        }
    }

    // Walls: the windows and doors beyond the first piece move to the piece they are on. They are
    // read before anything changes, deleted, and created again in the new piece.
    std::vector<std::unique_ptr<ElementData>> movingOpenings;
    std::vector<size_t> movingOpeningPieces;
    if (typeID == API_WallID) {
        GS::Array<API_Guid> openingGuids;
        for (USize i = 0; i < PtrItemCount (data.memo.wallWindows); ++i) {
            openingGuids.Push (data.memo.wallWindows[i]);
        }
        for (USize i = 0; i < PtrItemCount (data.memo.wallDoors); ++i) {
            openingGuids.Push (data.memo.wallDoors[i]);
        }
        for (const API_Guid& openingGuid : openingGuids) {
            auto opening = std::make_unique<ElementData> ();
            if (ReadElementData (openingGuid, *opening) != NoError) {
                return CreateErrorResponse (APIERR_GENERAL, "A window or door of the wall could not be read, so the wall was not split.");
            }
            const size_t piece = PieceOfDistance (bounds, length, opening->element.window.objLoc);
            if (piece > 0) {
                movingOpenings.push_back (std::move (opening));
                movingOpeningPieces.push_back (piece);
            }
        }
    }

    GSErrCode err = NoError;
    if (!movingOpenings.empty ()) {
        GS::Array<API_Guid> toDelete;
        for (const auto& opening : movingOpenings) {
            toDelete.Push (opening->guid);
        }
        err = ACAPI_Element_Delete (toDelete);
        if (err != NoError) {
            return CreateErrorResponse (err, "The windows and doors on the new pieces could not be moved, so the wall was not split.");
        }
    }

    std::vector<API_Guid> pieceGuids;
    for (size_t k = 0; k < nPieces; ++k) {
        const bool isFirst = k == 0;
        const bool isLast = k + 1 == nPieces;
        API_Element pieceElement = data.element;
        API_Element mask;
        ACAPI_ELEMENT_MASK_CLEAR (mask);
        SetCurveOfElement (pieceElement, mask, SubCurve (curve, bounds[k], bounds[k + 1]));
        ApplyPieceDetails (pieceElement, mask, data.element, bounds[k], bounds[k + 1], isFirst, isLast);

        API_ElementMemo* memo = nullptr;
        UInt64 memoMask = 0;
        if (typeID == API_BeamID) {
            SetBeamPieceMemo (data.memo, holes, begCut, endCut, innerCut, bounds[k], bounds[k + 1], length, isFirst, isLast);
            memo = &data.memo;
            memoMask = APIMemoMask_BeamHole | APIMemoMask_AssemblySegmentCut;
        }

        if (isFirst) {
            err = ACAPI_Element_Change (&pieceElement, &mask, memo, memoMask, true);
            if (err != NoError) {
                return CreateErrorResponse (err, "Failed to change the element to its first piece.");
            }
            pieceGuids.push_back (data.guid);
            if (typeID == API_BeamID) {
                // The segments of the new beams are new elements too.
                for (USize i = 0; i < PtrItemCount (data.memo.beamSegments); ++i) {
                    data.memo.beamSegments[i].head.guid = APINULLGuid;
                }
            }
            continue;
        }

        API_Guid newGuid = APINULLGuid;
        err = CreateCopy (data, pieceElement, memo, newGuid);
        if (err != NoError) {
            return CreateErrorResponse (err, GS::UniString::Printf ("The element was changed to its first piece, but piece %u could not be created.", (unsigned) (k + 1)));
        }
        pieceGuids.push_back (newGuid);
    }

    for (size_t i = 0; i < movingOpenings.size (); ++i) {
        ElementData& opening = *movingOpenings[i];
        API_Element openingElement = opening.element;
        openingElement.window.owner = pieceGuids[movingOpeningPieces[i]];
        openingElement.window.objLoc -= bounds[movingOpeningPieces[i]] * length;
        API_Guid newGuid = APINULLGuid;
        err = CreateCopy (opening, openingElement, &opening.memo, newGuid);
        if (err != NoError) {
            return CreateErrorResponse (err, "The wall was split, but a window or door could not be placed into its new piece.");
        }
    }

    return CreateElementsWrapper (pieceGuids);
}

// Polylines: the pieces are paths of straight and curved segments.
struct PolylineGeometry {
    std::vector<Curve> segments;
    bool               closed = false;
};

PolylineGeometry GetPolylineGeometry (const API_ElementMemo& memo)
{
    PolylineGeometry geometry;
    const USize nCoords = HandleItemCount (memo.coords);
    if (nCoords < 3) {
        return geometry;
    }
    const Int32 last = (Int32) nCoords - 1;
    for (Int32 i = 1; i < last; ++i) {
        double angle = 0.0;
        for (USize a = 0; a < HandleItemCount (memo.parcs); ++a) {
            const API_PolyArc& arc = (*memo.parcs)[a];
            if (arc.begIndex == i && arc.endIndex == i + 1) {
                angle = arc.arcAngle;
            }
        }
        geometry.segments.push_back (MakeCurve ((*memo.coords)[i], (*memo.coords)[i + 1], angle));
    }
    geometry.closed = last > 2 && IsSame2DCoordinate ((*memo.coords)[1], (*memo.coords)[last]);
    return geometry;
}

// The part of the path between two positions; a position is a segment index plus the fraction
// along that segment. For a closed polyline `to` may run past the end and wrap around.
std::vector<Curve> ExtractPath (const std::vector<Curve>& segments, double from, double to)
{
    std::vector<Curve> path;
    const size_t n = segments.size ();
    size_t i = (size_t) std::floor (from);
    while (true) {
        const double segFrom = std::max (from - (double) i, 0.0);
        const double segTo = std::min (to - (double) i, 1.0);
        if (segTo - segFrom > 1e-12) {
            path.push_back (SubCurve (segments[i % n], segFrom, segTo));
        }
        if ((double) (i + 1) >= to - 1e-12) {
            break;
        }
        ++i;
    }
    return path;
}

// A polyline memo for a path, in the layout SetDetailsOfElements writes polylines with.
void BuildPolylineMemo (const std::vector<Curve>& path, API_ElementMemo& memo, API_Polygon& poly)
{
    const Int32 nCoords = (Int32) path.size () + 1;
    Int32 nArcs = 0;
    for (const Curve& segment : path) {
        nArcs += IsArc (segment) ? 1 : 0;
    }
    memo.coords    = reinterpret_cast<API_Coord**>   (BMAllocateHandle ((nCoords + 1) * sizeof (API_Coord), ALLOCATE_CLEAR, 0));
    memo.pends     = reinterpret_cast<Int32**>       (BMAllocateHandle (2 * sizeof (Int32), ALLOCATE_CLEAR, 0));
    memo.parcs     = reinterpret_cast<API_PolyArc**> (BMAllocateHandle (nArcs * sizeof (API_PolyArc), ALLOCATE_CLEAR, 0));
    memo.vertexIDs = reinterpret_cast<UInt32**>      (BMAllocateHandle ((nCoords + 1) * sizeof (UInt32), ALLOCATE_CLEAR, 0));

    (*memo.coords)[0] = MakeCoord (-1.0, 0.0);
    (*memo.vertexIDs)[0] = (UInt32) nCoords;
    (*memo.coords)[1] = path.front ().beg;
    (*memo.vertexIDs)[1] = 1;
    Int32 iArc = 0;
    for (size_t j = 0; j < path.size (); ++j) {
        const Int32 index = (Int32) j + 2;
        (*memo.coords)[index] = path[j].end;
        (*memo.vertexIDs)[index] = (UInt32) index;
        if (IsArc (path[j])) {
            API_PolyArc arc = {};
            arc.begIndex = index - 1;
            arc.endIndex = index;
            arc.arcAngle = path[j].angle;
            (*memo.parcs)[iArc++] = arc;
        }
    }
    (*memo.pends)[0] = 0;
    (*memo.pends)[1] = nCoords;

    poly.nCoords = nCoords;
    poly.nSubPolys = 1;
    poly.nArcs = nArcs;
}

GS::ObjectState SplitPolyline (ElementData& data, const API_Coord& linePoint, const API_Coord& lineDir)
{
    const PolylineGeometry geometry = GetPolylineGeometry (data.memo);
    const std::vector<Curve>& segments = geometry.segments;
    if (segments.empty ()) {
        return CreateErrorResponse (APIERR_GENERAL, "The polyline's points could not be read.");
    }
    const size_t n = segments.size ();

    std::vector<double> positions;
    for (size_t j = 0; j < n; ++j) {
        for (double f : LineCrossings (segments[j], linePoint, lineDir)) {
            positions.push_back ((double) j + f);
        }
        // A vertex on the line is a split point too (an open polyline is not split at its ends).
        if ((geometry.closed || j > 0) && DistanceFromLine (segments[j].beg, linePoint, lineDir) <= LengthEps) {
            positions.push_back ((double) j);
        }
    }
    std::sort (positions.begin (), positions.end ());
    positions.erase (std::unique (positions.begin (), positions.end (), [] (double a, double b) { return std::abs (a - b) < 1e-9; }), positions.end ());
    if (positions.empty ()) {
        return CreateErrorResponse (APIERR_BADPARS, "The split line does not cross the element.");
    }

    std::vector<std::vector<Curve>> pieces;
    if (geometry.closed) {
        for (size_t k = 0; k < positions.size (); ++k) {
            const double to = k + 1 < positions.size () ? positions[k + 1] : positions[0] + (double) n;
            pieces.push_back (ExtractPath (segments, positions[k], to));
        }
    } else {
        std::vector<double> bounds = { 0.0 };
        bounds.insert (bounds.end (), positions.begin (), positions.end ());
        bounds.push_back ((double) n);
        for (size_t k = 0; k + 1 < bounds.size (); ++k) {
            pieces.push_back (ExtractPath (segments, bounds[k], bounds[k + 1]));
        }
    }

    std::vector<API_Guid> pieceGuids;
    for (size_t k = 0; k < pieces.size (); ++k) {
        if (pieces[k].empty ()) {
            continue;
        }
        API_Element pieceElement = data.element;
        API_Element mask;
        ACAPI_ELEMENT_MASK_CLEAR (mask);
        API_ElementMemo pieceMemo = {};
        const GS::OnExit memoGuard ([&pieceMemo] () { ACAPI_DisposeElemMemoHdls (&pieceMemo); });
        BuildPolylineMemo (pieces[k], pieceMemo, pieceElement.polyLine.poly);
        ACAPI_ELEMENT_MASK_SET (mask, API_PolyLineType, poly);
        HideInnerArrows (pieceElement, mask, k == 0 && !geometry.closed, k + 1 == pieces.size () && !geometry.closed);

        GSErrCode err = NoError;
        if (pieceGuids.empty ()) {
            err = ACAPI_Element_Change (&pieceElement, &mask, &pieceMemo, APIMemoMask_Polygon, true);
            if (err != NoError) {
                return CreateErrorResponse (err, "Failed to change the polyline to its first piece.");
            }
            pieceGuids.push_back (data.guid);
            continue;
        }
        API_Guid newGuid = APINULLGuid;
        err = CreateCopy (data, pieceElement, &pieceMemo, newGuid);
        if (err != NoError) {
            return CreateErrorResponse (err, GS::UniString::Printf ("The polyline was changed to its first piece, but piece %u could not be created.", (unsigned) (k + 1)));
        }
        pieceGuids.push_back (newGuid);
    }

    return CreateElementsWrapper (pieceGuids);
}

#ifdef ServerMainVers_2900
// Slabs and hatches: the polygon (with its holes and curved edges) is cut with the two half
// planes of the split line by Geometry::Polygon2D, which may give more than one piece per side.

// The memo layout of API polygons: coords[0] unused, every contour closed by a copy of its first
// vertex, pends[0] == 0, arcs numbered by their vertices.
constexpr GS::UInt32 ApiPolygonLayout = Geometry::PolyCreateFlags_DummyCoordAtIdxZero |
                                        Geometry::PolyCreateFlags_1stCoordDuplAtContourEnd |
                                        Geometry::PolyCreateFlags_DummyContourEndAtIdxZero;

// A polygon in the shape CreateSlabs and CreateHatches take it: the outline and its curved edges
// (numbered from the outline's first vertex) and the holes, each as polygonOutline and polygonArcs.
struct PolygonPiece {
    GS::Array<GS::ObjectState> outline;
    GS::Array<GS::ObjectState> arcs;
    GS::Array<GS::ObjectState> holes;
};

PolygonPiece PolygonPieceFromApiLayout (const GS::Array<Point2D>& coords, const GS::Array<PolyArcRec>& arcs, const GS::Array<UIndex>& ends)
{
    PolygonPiece piece;
    for (UIndex c = 0; c + 1 < ends.GetSize (); ++c) {
        const UIndex first = ends[c] + 1;
        const UIndex closing = ends[c + 1];
        GS::Array<GS::ObjectState> contourCoords;
        GS::Array<GS::ObjectState> contourArcs;
        for (UIndex i = first; i < closing; ++i) {
            contourCoords.Push (Create2DCoordinateObjectState (MakeCoord (coords[i].x, coords[i].y)));
        }
        for (const PolyArcRec& arc : arcs) {
            if (arc.begIndex >= first && arc.begIndex < closing) {
                contourArcs.Push (GS::ObjectState ("begIndex", (Int32) (arc.begIndex - first), "endIndex", (Int32) (arc.endIndex - first), "arcAngle", arc.arcAngle));
            }
        }
        if (c == 0) {
            piece.outline = contourCoords;
            piece.arcs = contourArcs;
        } else {
            piece.holes.Push (GS::ObjectState ("polygonOutline", contourCoords, "polygonArcs", contourArcs));
        }
    }
    return piece;
}

std::vector<PolygonPiece> CutPolygonByLine (const API_ElementMemo& memo, const API_Coord& linePoint, const API_Coord& lineDir)
{
    GS::Array<Point2D> coords;
    for (USize i = 0; i < HandleItemCount (memo.coords); ++i) {
        coords.Push (Point2D ((*memo.coords)[i].x, (*memo.coords)[i].y));
    }
    GS::Array<PolyArcRec> arcs;
    for (USize i = 0; i < HandleItemCount (memo.parcs); ++i) {
        const API_PolyArc& arc = (*memo.parcs)[i];
        arcs.Push (PolyArcRec ((UIndex) arc.begIndex, (UIndex) arc.endIndex, arc.arcAngle));
    }
    GS::Array<UIndex> ends;
    for (USize i = 0; i < HandleItemCount (memo.pends); ++i) {
        ends.Push ((UIndex) (*memo.pends)[i]);
    }

    std::vector<PolygonPiece> pieces;
    const Geometry::MultiPolygon2D source = Geometry::Polygon2D::Create (coords, arcs, ends, ApiPolygonLayout);
    const API_Coord normal = MakeCoord (-lineDir.y, lineDir.x);
    for (double side : { 1.0, -1.0 }) {
        // ca * x + cb * y > cc is outside the half plane.
        const HalfPlane2DData halfPlane (side * normal.x, side * normal.y, side * Dot (normal, linePoint));
        for (const Geometry::Polygon2D& polygon : source) {
            const Geometry::MultiPolygon2D parts = polygon.Cut (halfPlane);
            for (const Geometry::Polygon2D& part : parts) {
                GS::Array<Point2D> partCoords;
                GS::Array<PolyArcRec> partArcs;
                GS::Array<UIndex> partEnds;
                part.CopyPolygonData (partCoords, partArcs, partEnds, GS::Array<Geometry::Polygon2D::ConstContourIterator> (), ApiPolygonLayout);
                pieces.push_back (PolygonPieceFromApiLayout (partCoords, partArcs, partEnds));
            }
        }
    }
    return pieces;
}

// The memo of a new slab or hatch piece. The memo builder of CreateSlabs fills in the edge data
// along the new polygon; it gets an empty memo, so it allocates every handle at the size of the
// piece (it reuses handles it is given when the element's own counts match the new ones).
bool BuildPolygonPieceMemo (API_Element& element, API_ElementMemo& memo, PolygonPiece& piece)
{
    if (GetElemTypeId (element.header) == API_SlabID) {
        return !BuildSlabMemoFromGeometry (element, memo, piece.outline, piece.arcs, piece.holes).HasValue ();
    }
    return BuildHatchPolygonMemo (piece.outline, piece.arcs, piece.holes, element.hatch.poly, memo);
}

// The original slab gets its new polygon the way ModifySlabs writes one (ApplySlabPolygonChange
// and ACAPI_Element_ChangeMemo): a polygon rebuilt from scratch is refused (APIERR_BADPOLY) once
// the number of vertices changes. A hatch takes its rebuilt polygon with ACAPI_Element_Change.
GSErrCode ChangeToPolygonPiece (const ElementData& data, PolygonPiece& piece)
{
    API_ElementMemo memo = {};
    const GS::OnExit memoGuard ([&memo] () { ACAPI_DisposeElemMemoHdls (&memo); });
    if (GetElemTypeId (data.element.header) == API_SlabID) {
        ACAPI_Element_GetMemo (data.guid, &memo);
        if (ApplySlabPolygonChange (memo, data.element.slab.sideMat, piece.outline, piece.arcs, piece.holes).HasValue ()) {
            return APIERR_BADPOLY;
        }
        API_Guid guid = data.guid;
        return ACAPI_Element_ChangeMemo (guid, APIMemoMask_Polygon | APIMemoMask_EdgeTrims | APIMemoMask_SideMaterials, &memo);
    }
    API_Element element = data.element;
    API_Element mask;
    ACAPI_ELEMENT_MASK_CLEAR (mask);
    if (!BuildHatchPolygonMemo (piece.outline, piece.arcs, piece.holes, element.hatch.poly, memo)) {
        return APIERR_MEMFULL;
    }
    ACAPI_ELEMENT_MASK_SET (mask, API_HatchType, poly);
    return ACAPI_Element_Change (&element, &mask, &memo, APIMemoMask_Polygon, true);
}

GS::ObjectState SplitPolygonElement (ElementData& data, const API_Coord& linePoint, const API_Coord& lineDir)
{
    std::vector<PolygonPiece> pieces = CutPolygonByLine (data.memo, linePoint, lineDir);
    if (pieces.size () < 2) {
        return CreateErrorResponse (APIERR_BADPARS, "The split line does not cross the element.");
    }

    // ApplySlabPolygonChange adds outline vertices by splitting an edge, which keeps the outline
    // valid, but removes them one by one, which can leave an intermediate outline that crosses
    // itself: Archicad then refuses it, and with a concave outline it corrupted the heap (Archicad 30
    // crashed in the next allocation). So the original slab keeps the first piece with at least as
    // many outline vertices as it has, and when there is none, every piece becomes a new slab and
    // the original is deleted at the end; the same happens when Archicad refuses the new polygon.
    const bool isSlab = GetElemTypeId (data.element.header) == API_SlabID;
    const Int32 originalOutlineCount = HandleItemCount (data.memo.pends) >= 2 ? (*data.memo.pends)[1] - 1 : 0;
    bool canKeepOriginal = !isSlab;
    for (size_t k = 0; isSlab && k < pieces.size (); ++k) {
        if ((Int32) pieces[k].outline.GetSize () >= originalOutlineCount) {
            std::rotate (pieces.begin (), pieces.begin () + k, pieces.begin () + k + 1);
            canKeepOriginal = true;
            break;
        }
    }

    const bool originalChanged = canKeepOriginal && ChangeToPolygonPiece (data, pieces[0]) == NoError;
    std::vector<API_Guid> pieceGuids;
    if (originalChanged) {
        pieceGuids.push_back (data.guid);
    }
    for (size_t k = originalChanged ? 1 : 0; k < pieces.size (); ++k) {
        API_Element pieceElement = data.element;
        API_ElementMemo pieceMemo = {};
        const GS::OnExit memoGuard ([&pieceMemo] () { ACAPI_DisposeElemMemoHdls (&pieceMemo); });
        if (!BuildPolygonPieceMemo (pieceElement, pieceMemo, pieces[k])) {
            return CreateErrorResponse (APIERR_GENERAL, GS::UniString::Printf ("The polygon of piece %u could not be made.", (unsigned) (k + 1)));
        }
        API_Guid newGuid = APINULLGuid;
        const GSErrCode err = CreateCopy (data, pieceElement, &pieceMemo, newGuid);
        if (err != NoError) {
            return CreateErrorResponse (err, GS::UniString::Printf ("Piece %u of the element could not be created.", (unsigned) (k + 1)));
        }
        pieceGuids.push_back (newGuid);
    }
    if (!originalChanged) {
        const GSErrCode err = ACAPI_Element_Delete ({ data.guid });
        if (err != NoError) {
            return CreateErrorResponse (err, "The pieces were created as new elements, but the original could not be deleted.");
        }
    }

    return CreateElementsWrapper (pieceGuids);
}
#endif

GS::ObjectState SplitElement (const API_Guid& guid, const API_Coord& linePoint, const API_Coord& lineDir)
{
    const GS::Optional<GS::ObjectState> notEditable = CheckEditable (guid, "split");
    if (notEditable.HasValue ()) {
        return *notEditable;
    }

    ElementData data;
    const GSErrCode err = ReadElementData (guid, data);
    if (err != NoError) {
        return CreateErrorResponse (err, "Failed to read the element.");
    }

    switch (GetElemTypeId (data.element.header)) {
        case API_WallID:
        case API_BeamID:
        case API_LineID:
        case API_ArcID:
            return SplitCurveElement (data, linePoint, lineDir);
        case API_PolyLineID:
            return SplitPolyline (data, linePoint, lineDir);
        case API_SlabID:
        case API_HatchID:
#ifdef ServerMainVers_2900
            return SplitPolygonElement (data, linePoint, lineDir);
#else
            return CreateErrorResponse (APIERR_NOTSUPPORTED, "Slabs and hatches can be split from Archicad 29.");
#endif
        default:
            return CreateErrorResponse (APIERR_BADELEMENTTYPE,
                GS::UniString::Printf ("Elements of type %T cannot be split, only walls, beams, lines, arcs, polylines, slabs and hatches.", ElemTypeName (data.element.header).ToPrintf ()));
    }
}

GS::Array<API_Guid> GetElementGuids (const GS::Array<GS::ObjectState>& elements)
{
    GS::Array<API_Guid> guids;
    for (const GS::ObjectState& item : elements) {
        guids.Push (GetGuidFromElementsArrayItem (item));
    }
    return guids;
}

} // namespace

SplitElementsCommand::SplitElementsCommand () :
    CommandBase (CommonSchema::Used)
{
}

GS::String SplitElementsCommand::GetName () const
{
    return "SplitElements";
}

GS::Optional<GS::UniString> SplitElementsCommand::GetInputParametersSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "elements": {
                "$ref": "#/Elements",
                "description": "The elements to split: walls (straight, trapezoid or curved, not polygonal), beams of one segment (straight, slanted or horizontally curved), lines, circular arcs, polylines, and from Archicad 29 slabs and hatches."
            },
            "splitLine": {
                "type": "object",
                "description": "The split line, infinite in both directions, through two points on the floor plan.",
                "properties": {
                    "begCoordinate": {
                        "$ref": "#/Coordinate2D",
                        "description": "A point of the split line."
                    },
                    "endCoordinate": {
                        "$ref": "#/Coordinate2D",
                        "description": "Another point of the split line. Must differ from begCoordinate."
                    }
                },
                "additionalProperties": false,
                "required": ["begCoordinate", "endCoordinate"]
            }
        },
        "additionalProperties": false,
        "required": ["elements", "splitLine"]
    })";
}

GS::Optional<GS::UniString> SplitElementsCommand::GetRawResponseSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "piecesOfElements": {
                "type": "array",
                "description": "The pieces of each element in the order of the input; the first one is the original element itself. The pieces of a wall, beam, line, arc or polyline are in order along it from its beginning. Of a slab or hatch, the pieces on the right of the split line (looking from begCoordinate to endCoordinate) come before those on its left, except that a slab keeps the first piece with at least as many outline vertices as it had; when no piece has that many (vertices cannot be taken away from an existing slab safely), or Archicad refuses the new polygon, all pieces are new slabs and the original is deleted. An element the split line does not cross, or that cannot be split, gets an error.",
                "items": {
                    "$ref": "#/ElementsWrapperOrError"
                }
            }
        },
        "additionalProperties": false,
        "required": ["piecesOfElements"]
    })";
}

GS::ObjectState SplitElementsCommand::Execute (const GS::ObjectState& parameters, GS::ProcessControl& /*processControl*/) const
{
    GS::Array<GS::ObjectState> elements;
    parameters.Get ("elements", elements);
    const GS::ObjectState* splitLine = parameters.Get ("splitLine");
    if (splitLine == nullptr || splitLine->Get ("begCoordinate") == nullptr || splitLine->Get ("endCoordinate") == nullptr) {
        return CreateErrorResponse (APIERR_BADPARS, "splitLine needs begCoordinate and endCoordinate.");
    }
    const API_Coord lineBeg = Get2DCoordinateFromObjectState (*splitLine->Get ("begCoordinate"));
    const API_Coord lineEnd = Get2DCoordinateFromObjectState (*splitLine->Get ("endCoordinate"));
    if (Distance (lineBeg, lineEnd) <= LengthEps) {
        return CreateErrorResponse (APIERR_BADPARS, "The two points of splitLine are the same point, so they do not define a line.");
    }
    const API_Coord lineDir = Normalized (Minus (lineEnd, lineBeg));

    GS::ObjectState response;
    const auto& piecesOfElements = response.AddList<GS::ObjectState> ("piecesOfElements");

    const FloorPlanDatabaseScope databaseScope (GetElementGuids (elements));
    ACAPI_CallUndoableCommand ("Split Elements", [&] () -> GSErrCode {
        for (const GS::ObjectState& item : elements) {
            piecesOfElements (SplitElement (GetGuidFromElementsArrayItem (item), lineBeg, lineDir));
        }
        return NoError;
    });

    return response;
}

// ---------------------------------------------------------------------------------------------
// Intersect and Adjust
// ---------------------------------------------------------------------------------------------

namespace {

// Reads a straight wall, beam or line for Intersect, Adjust, Fillet and Chamfer.
GS::Optional<GS::ObjectState> ReadStraightElement (const API_Guid& guid, const char* operation, API_Element& element, Curve& curve)
{
    const GS::Optional<GS::ObjectState> notEditable = CheckEditable (guid, operation);
    if (notEditable.HasValue ()) {
        return notEditable;
    }
    ClearElement (element, guid);
    const GSErrCode err = ACAPI_Element_Get (&element);
    if (err != NoError) {
        return CreateErrorResponse (err, "Failed to read the element.");
    }
    const GS::Optional<GS::UniString> curveError = GetCurveOfElement (element, curve);
    if (curveError.HasValue ()) {
        return CreateErrorResponse (APIERR_BADELEMENTTYPE, *curveError);
    }
    if (IsArc (curve)) {
        return CreateErrorResponse (APIERR_BADELEMENTTYPE, GS::UniString::Printf ("Only straight elements can be %s; this one is curved.", operation));
    }
    if (CurveLength (curve) <= LengthEps) {
        return CreateErrorResponse (APIERR_BADPARS, "The element has no length.");
    }
    return GS::NoValue;
}

inline API_Coord DirectionOf (const Curve& curve)
{
    return Minus (curve.end, curve.beg);
}

// Moves the end of the curve that is nearer to the point onto the point.
GS::Optional<GS::UniString> MoveNearerEnd (Curve& curve, const API_Coord& point)
{
    if (Distance (curve.beg, point) <= Distance (curve.end, point)) {
        curve.beg = point;
    } else {
        curve.end = point;
    }
    if (CurveLength (curve) <= LengthEps) {
        return GS::UniString ("The element would have no length left.");
    }
    return GS::NoValue;
}

GSErrCode ChangeCurve (const API_Element& original, const Curve& curve)
{
    API_Element element = original;
    API_Element mask;
    ACAPI_ELEMENT_MASK_CLEAR (mask);
    SetCurveOfElement (element, mask, curve);
    return ACAPI_Element_Change (&element, &mask, nullptr, 0, true);
}

GS::ObjectState IntersectPair (const API_Guid& guid1, const API_Guid& guid2)
{
    if (guid1 == guid2) {
        return CreateFailedExecutionResult (APIERR_BADPARS, "The two elements of the pair are the same element.");
    }
    API_Element element1, element2;
    Curve curve1, curve2;
    GS::Optional<GS::ObjectState> error = ReadStraightElement (guid1, "intersected", element1, curve1);
    if (!error.HasValue ()) {
        error = ReadStraightElement (guid2, "intersected", element2, curve2);
    }
    if (error.HasValue ()) {
        return CreateFailedExecutionResult (*error);
    }

    API_Coord corner;
    if (!IntersectLines (curve1.beg, DirectionOf (curve1), curve2.beg, DirectionOf (curve2), corner)) {
        return CreateFailedExecutionResult (APIERR_BADPARS, "The elements are parallel, so they have no intersection.");
    }
    GS::Optional<GS::UniString> curveError = MoveNearerEnd (curve1, corner);
    if (!curveError.HasValue ()) {
        curveError = MoveNearerEnd (curve2, corner);
    }
    if (curveError.HasValue ()) {
        return CreateFailedExecutionResult (APIERR_BADPARS, *curveError);
    }

    GSErrCode err = ChangeCurve (element1, curve1);
    if (err == NoError) {
        err = ChangeCurve (element2, curve2);
    }
    if (err != NoError) {
        return CreateFailedExecutionResult (err, "Failed to change the elements.");
    }
    return CreateSuccessfulExecutionResult ();
}

} // namespace

IntersectElementsCommand::IntersectElementsCommand () :
    CommandBase (CommonSchema::Used)
{
}

GS::String IntersectElementsCommand::GetName () const
{
    return "IntersectElements";
}

GS::Optional<GS::UniString> IntersectElementsCommand::GetInputParametersSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "elementPairs": {
                "type": "array",
                "description": "The pairs of straight linear elements (walls, beams, lines; of any of these types) to extend or trim to the intersection of their lines. In each element the end nearer to the intersection moves onto it.",
                "items": {
                    "type": "object",
                    "properties": {
                        "elementId1": {
                            "$ref": "#/ElementId"
                        },
                        "elementId2": {
                            "$ref": "#/ElementId"
                        }
                    },
                    "additionalProperties": false,
                    "required": ["elementId1", "elementId2"]
                }
            }
        },
        "additionalProperties": false,
        "required": ["elementPairs"]
    })";
}

GS::Optional<GS::UniString> IntersectElementsCommand::GetRawResponseSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "executionResults": {
                "$ref": "#/ExecutionResults"
            }
        },
        "additionalProperties": false,
        "required": ["executionResults"]
    })";
}

GS::ObjectState IntersectElementsCommand::Execute (const GS::ObjectState& parameters, GS::ProcessControl& /*processControl*/) const
{
    GS::Array<GS::ObjectState> elementPairs;
    parameters.Get ("elementPairs", elementPairs);

    GS::ObjectState response;
    const auto& executionResults = response.AddList<GS::ObjectState> ("executionResults");

    ACAPI_CallUndoableCommand ("Intersect Elements", [&] () -> GSErrCode {
        for (const GS::ObjectState& pair : elementPairs) {
            const GS::ObjectState* id1 = pair.Get ("elementId1");
            const GS::ObjectState* id2 = pair.Get ("elementId2");
            if (id1 == nullptr || id2 == nullptr) {
                executionResults (CreateFailedExecutionResult (APIERR_BADPARS, "elementId1 or elementId2 is missing."));
                continue;
            }
            executionResults (IntersectPair (GetGuidFromObjectState (*id1), GetGuidFromObjectState (*id2)));
        }
        return NoError;
    });

    return response;
}

AdjustElementsCommand::AdjustElementsCommand () :
    CommandBase (CommonSchema::Used)
{
}

GS::String AdjustElementsCommand::GetName () const
{
    return "AdjustElements";
}

GS::Optional<GS::UniString> AdjustElementsCommand::GetInputParametersSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "elements": {
                "$ref": "#/Elements",
                "description": "The straight walls, beams and lines to extend or trim to the target. In each element the end nearer to the target line moves onto it."
            },
            "targetLine": {
                "type": "object",
                "description": "The target: the infinite line through two points. Give either targetLine or targetElementId.",
                "properties": {
                    "begCoordinate": {
                        "$ref": "#/Coordinate2D"
                    },
                    "endCoordinate": {
                        "$ref": "#/Coordinate2D"
                    }
                },
                "additionalProperties": false,
                "required": ["begCoordinate", "endCoordinate"]
            },
            "targetElementId": {
                "$ref": "#/ElementId",
                "description": "The target: a straight wall, beam or line, whose reference line (for a wall or a beam) or line is extended infinitely. Give either targetLine or targetElementId."
            }
        },
        "additionalProperties": false,
        "required": ["elements"]
    })";
}

GS::Optional<GS::UniString> AdjustElementsCommand::GetRawResponseSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "executionResults": {
                "$ref": "#/ExecutionResults"
            }
        },
        "additionalProperties": false,
        "required": ["executionResults"]
    })";
}

GS::ObjectState AdjustElementsCommand::Execute (const GS::ObjectState& parameters, GS::ProcessControl& /*processControl*/) const
{
    GS::Array<GS::ObjectState> elements;
    parameters.Get ("elements", elements);

    const GS::ObjectState* targetLine = parameters.Get ("targetLine");
    const GS::ObjectState* targetElementId = parameters.Get ("targetElementId");
    if ((targetLine == nullptr) == (targetElementId == nullptr)) {
        return CreateErrorResponse (APIERR_BADPARS, "Give exactly one of targetLine and targetElementId.");
    }

    API_Coord targetPoint;
    API_Coord targetDir;
    API_Guid targetGuid = APINULLGuid;
    if (targetLine != nullptr) {
        if (targetLine->Get ("begCoordinate") == nullptr || targetLine->Get ("endCoordinate") == nullptr) {
            return CreateErrorResponse (APIERR_BADPARS, "targetLine needs begCoordinate and endCoordinate.");
        }
        targetPoint = Get2DCoordinateFromObjectState (*targetLine->Get ("begCoordinate"));
        targetDir = Minus (Get2DCoordinateFromObjectState (*targetLine->Get ("endCoordinate")), targetPoint);
        if (Length (targetDir) <= LengthEps) {
            return CreateErrorResponse (APIERR_BADPARS, "The two points of targetLine are the same point, so they do not define a line.");
        }
    } else {
        targetGuid = GetGuidFromObjectState (*targetElementId);
        API_Element target;
        ClearElement (target, targetGuid);
        if (ACAPI_Element_Get (&target) != NoError) {
            return CreateErrorResponse (APIERR_BADID, "The target element does not exist.");
        }
        Curve targetCurve;
        const GS::Optional<GS::UniString> curveError = GetCurveOfElement (target, targetCurve);
        if (curveError.HasValue ()) {
            return CreateErrorResponse (APIERR_BADELEMENTTYPE, "The target element cannot be used: " + *curveError);
        }
        if (IsArc (targetCurve) || CurveLength (targetCurve) <= LengthEps) {
            return CreateErrorResponse (APIERR_BADELEMENTTYPE, "The target element must be straight.");
        }
        targetPoint = targetCurve.beg;
        targetDir = DirectionOf (targetCurve);
    }

    GS::ObjectState response;
    const auto& executionResults = response.AddList<GS::ObjectState> ("executionResults");

    ACAPI_CallUndoableCommand ("Adjust Elements", [&] () -> GSErrCode {
        for (const GS::ObjectState& item : elements) {
            const API_Guid guid = GetGuidFromElementsArrayItem (item);
            if (guid == targetGuid) {
                executionResults (CreateFailedExecutionResult (APIERR_BADPARS, "The element is the target itself."));
                continue;
            }
            API_Element element;
            Curve curve;
            const GS::Optional<GS::ObjectState> error = ReadStraightElement (guid, "adjusted", element, curve);
            if (error.HasValue ()) {
                executionResults (CreateFailedExecutionResult (*error));
                continue;
            }
            API_Coord point;
            if (!IntersectLines (curve.beg, DirectionOf (curve), targetPoint, targetDir, point)) {
                executionResults (CreateFailedExecutionResult (APIERR_BADPARS, "The element is parallel to the target, so it cannot reach it."));
                continue;
            }
            const GS::Optional<GS::UniString> curveError = MoveNearerEnd (curve, point);
            if (curveError.HasValue ()) {
                executionResults (CreateFailedExecutionResult (APIERR_BADPARS, *curveError));
                continue;
            }
            const GSErrCode err = ChangeCurve (element, curve);
            executionResults (err == NoError ? CreateSuccessfulExecutionResult () : CreateFailedExecutionResult (err, "Failed to change the element."));
        }
        return NoError;
    });

    return response;
}

// ---------------------------------------------------------------------------------------------
// Fillet and Chamfer
// ---------------------------------------------------------------------------------------------

namespace {

// Where two straight elements meet: the intersection of their lines, and for each element the
// unit direction from the corner towards its far end (the end that stays) and the distance to it.
struct Corner {
    API_Coord point;
    API_Coord dir1;
    API_Coord dir2;
    double    reach1 = 0.0;
    double    reach2 = 0.0;
    bool      nearIsEnd1 = false;
    bool      nearIsEnd2 = false;
    double    angle = 0.0;  // between dir1 and dir2, 0 < angle < pi
};

GS::Optional<GS::UniString> ComputeCorner (const Curve& curve1, const Curve& curve2, Corner& corner)
{
    if (!IntersectLines (curve1.beg, DirectionOf (curve1), curve2.beg, DirectionOf (curve2), corner.point)) {
        return GS::UniString ("The elements are parallel, so they have no corner.");
    }
    auto side = [&] (const Curve& curve, API_Coord& dir, double& reach, bool& nearIsEnd) {
        nearIsEnd = Distance (curve.end, corner.point) <= Distance (curve.beg, corner.point);
        const API_Coord farEnd = nearIsEnd ? curve.beg : curve.end;
        reach = Distance (farEnd, corner.point);
        dir = Normalized (Minus (farEnd, corner.point));
    };
    side (curve1, corner.dir1, corner.reach1, corner.nearIsEnd1);
    side (curve2, corner.dir2, corner.reach2, corner.nearIsEnd2);
    if (corner.reach1 <= LengthEps || corner.reach2 <= LengthEps) {
        return GS::UniString ("An element lies entirely at the corner.");
    }
    corner.angle = std::acos (std::max (-1.0, std::min (1.0, Dot (corner.dir1, corner.dir2))));
    if (corner.angle < 1e-6 || Pi - corner.angle < 1e-6) {
        return GS::UniString ("The elements are on one line, so they have no corner.");
    }
    return GS::NoValue;
}

inline void MoveNearEnd (Curve& curve, bool nearIsEnd, const API_Coord& point)
{
    if (nearIsEnd) {
        curve.end = point;
    } else {
        curve.beg = point;
    }
}

// The new element between the two trimmed elements. It runs in the direction of the first
// element: from its trimmed end when that end is its end point, towards its beginning otherwise.
GS::ObjectState CreateConnectingElement (const ElementData& data1, const Corner& corner, const Curve& fromFirstToSecond)
{
    const Curve connection = corner.nearIsEnd1 ? fromFirstToSecond : Reversed (fromFirstToSecond);
    API_Element element = data1.element;
    API_Element mask;
    ACAPI_ELEMENT_MASK_CLEAR (mask);
    API_Guid newGuid = APINULLGuid;
    GSErrCode err = NoError;

    switch (GetElemTypeId (data1.element.header)) {
        case API_WallID: {
            SetCurveOfElement (element, mask, connection);
            if (data1.element.wall.type == APIWtyp_Trapez) {
                const double nearThickness = corner.nearIsEnd1 ? data1.element.wall.thickness1 : data1.element.wall.thickness;
                element.wall.type = APIWtyp_Normal;
                element.wall.thickness = nearThickness;
                element.wall.thickness1 = nearThickness;
            }
            err = CreateCopy (data1, element, nullptr, newGuid);
        } break;
        case API_LineID:
            if (!IsArc (connection)) {
                SetCurveOfElement (element, mask, connection);
                HideInnerArrows (element, mask, false, false);
                err = CreateCopy (data1, element, nullptr, newGuid);
            } else {
                // A line is rounded with an arc element that has the line's settings.
                API_Element arc;
                ClearElement (arc);
                SetElemType (arc.header, API_ArcID);
                err = ACAPI_Element_GetDefaults (&arc, nullptr);
                if (err != NoError) {
                    break;
                }
                arc.header.layer = data1.element.header.layer;
                arc.header.floorInd = data1.element.header.floorInd;
                arc.arc.linePen = data1.element.line.linePen;
                arc.arc.ltypeInd = data1.element.line.ltypeInd;
                arc.arc.roomSeparator = data1.element.line.roomSeparator;
                arc.arc.determination = data1.element.line.determination;
                arc.arc.penWeight = data1.element.line.penWeight;
                arc.arc.ratio = 1.0;
                arc.arc.angle = 0.0;
                SetCurveOfElement (arc, mask, connection);
                HideInnerArrows (arc, mask, false, false);
                err = CreateCopy (data1, arc, nullptr, newGuid);
            }
            break;
        default:
            err = APIERR_BADELEMENTTYPE;
            break;
    }

    if (err != NoError) {
        return CreateErrorResponse (err, "The elements were trimmed, but the element between them could not be created.");
    }
    return CreateElementIdObjectState (newGuid);
}

enum class CornerOperation { Fillet, Chamfer };

GS::ObjectState RoundOrCutCorner (const GS::ObjectState& pair, CornerOperation operation)
{
    const char* operationName = operation == CornerOperation::Fillet ? "filleted" : "chamfered";
    const GS::ObjectState* id1 = pair.Get ("elementId1");
    const GS::ObjectState* id2 = pair.Get ("elementId2");
    if (id1 == nullptr || id2 == nullptr) {
        return CreateErrorResponse (APIERR_BADPARS, "elementId1 or elementId2 is missing.");
    }
    const API_Guid guid1 = GetGuidFromObjectState (*id1);
    const API_Guid guid2 = GetGuidFromObjectState (*id2);
    if (guid1 == guid2) {
        return CreateErrorResponse (APIERR_BADPARS, "The two elements of the pair are the same element.");
    }

    API_Element element1, element2;
    Curve curve1, curve2;
    GS::Optional<GS::ObjectState> error = ReadStraightElement (guid1, operationName, element1, curve1);
    if (!error.HasValue ()) {
        error = ReadStraightElement (guid2, operationName, element2, curve2);
    }
    if (error.HasValue ()) {
        return *error;
    }
    const API_ElemTypeID type1 = GetElemTypeId (element1.header);
    if (type1 != GetElemTypeId (element2.header) || (type1 != API_WallID && type1 != API_LineID)) {
        return CreateErrorResponse (APIERR_BADELEMENTTYPE, "Both elements must be walls, or both must be lines.");
    }

    Corner corner;
    const GS::Optional<GS::UniString> cornerError = ComputeCorner (curve1, curve2, corner);
    if (cornerError.HasValue ()) {
        return CreateErrorResponse (APIERR_BADPARS, *cornerError);
    }

    API_Coord point1, point2;
    Curve between;
    if (operation == CornerOperation::Fillet) {
        double radius = 0.0;
        pair.Get ("radius", radius);
        if (radius <= LengthEps) {
            return CreateErrorResponse (APIERR_BADPARS, "The radius must be positive.");
        }
        const double tangentLength = radius / std::tan (corner.angle / 2.0);
        if (tangentLength >= corner.reach1 - LengthEps || tangentLength >= corner.reach2 - LengthEps) {
            return CreateErrorResponse (APIERR_BADPARS, "The radius is too large: the arc would not fit on the elements.");
        }
        point1 = Plus (corner.point, Times (corner.dir1, tangentLength));
        point2 = Plus (corner.point, Times (corner.dir2, tangentLength));
        const API_Coord center = Plus (corner.point, Times (Normalized (Plus (corner.dir1, corner.dir2)), radius / std::sin (corner.angle / 2.0)));
        const double sweep = Pi - corner.angle;
        between = MakeCurve (point1, point2, Cross (Minus (point1, center), Minus (point2, center)) > 0.0 ? sweep : -sweep);
    } else {
        double distance1 = 0.0;
        pair.Get ("distance1", distance1);
        double distance2 = distance1;
        pair.Get ("distance2", distance2);
        if (distance1 <= LengthEps || distance2 <= LengthEps) {
            return CreateErrorResponse (APIERR_BADPARS, "The chamfer distances must be positive.");
        }
        if (distance1 >= corner.reach1 - LengthEps || distance2 >= corner.reach2 - LengthEps) {
            return CreateErrorResponse (APIERR_BADPARS, "A chamfer distance is too large: the chamfer would not fit on the elements.");
        }
        point1 = Plus (corner.point, Times (corner.dir1, distance1));
        point2 = Plus (corner.point, Times (corner.dir2, distance2));
        between = MakeCurve (point1, point2, 0.0);
    }

    ElementData data1;
    GSErrCode err = ReadElementData (guid1, data1);
    if (err != NoError) {
        return CreateErrorResponse (err, "Failed to read the first element.");
    }

    MoveNearEnd (curve1, corner.nearIsEnd1, point1);
    MoveNearEnd (curve2, corner.nearIsEnd2, point2);
    err = ChangeCurve (element1, curve1);
    if (err == NoError) {
        err = ChangeCurve (element2, curve2);
    }
    if (err != NoError) {
        return CreateErrorResponse (err, "Failed to trim the elements.");
    }

    return CreateConnectingElement (data1, corner, between);
}

GS::ObjectState ExecuteCornerOperation (const GS::ObjectState& parameters, CornerOperation operation)
{
    GS::Array<GS::ObjectState> elementPairs;
    parameters.Get ("elementPairs", elementPairs);

    GS::ObjectState response;
    const auto& connectingElements = response.AddList<GS::ObjectState> ("connectingElements");

    GS::Array<API_Guid> guids;
    for (const GS::ObjectState& pair : elementPairs) {
        const GS::ObjectState* id1 = pair.Get ("elementId1");
        if (id1 != nullptr) {
            guids.Push (GetGuidFromObjectState (*id1));
        }
    }
    const FloorPlanDatabaseScope databaseScope (guids);
    ACAPI_CallUndoableCommand (operation == CornerOperation::Fillet ? "Fillet Elements" : "Chamfer Elements", [&] () -> GSErrCode {
        for (const GS::ObjectState& pair : elementPairs) {
            connectingElements (RoundOrCutCorner (pair, operation));
        }
        return NoError;
    });

    return response;
}

} // namespace

FilletElementsCommand::FilletElementsCommand () :
    CommandBase (CommonSchema::Used)
{
}

GS::String FilletElementsCommand::GetName () const
{
    return "FilletElements";
}

GS::Optional<GS::UniString> FilletElementsCommand::GetInputParametersSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "elementPairs": {
                "type": "array",
                "description": "The pairs of straight walls or straight lines whose corner is rounded. Both elements of a pair are walls, or both are lines; their lines need not meet, the corner is where they would.",
                "items": {
                    "type": "object",
                    "properties": {
                        "elementId1": {
                            "$ref": "#/ElementId"
                        },
                        "elementId2": {
                            "$ref": "#/ElementId"
                        },
                        "radius": {
                            "type": "number",
                            "description": "The radius of the rounding arc (of the walls' reference lines).",
                            "exclusiveMinimum": 0
                        }
                    },
                    "additionalProperties": false,
                    "required": ["elementId1", "elementId2", "radius"]
                }
            }
        },
        "additionalProperties": false,
        "required": ["elementPairs"]
    })";
}

GS::Optional<GS::UniString> FilletElementsCommand::GetRawResponseSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "connectingElements": {
                "type": "array",
                "description": "For each pair, in the order of the input: the new element between the two trimmed elements (a curved wall with the settings of the first wall, or an arc with the settings of the first line), or an error.",
                "items": {
                    "$ref": "#/ElementIdOrError"
                }
            }
        },
        "additionalProperties": false,
        "required": ["connectingElements"]
    })";
}

GS::ObjectState FilletElementsCommand::Execute (const GS::ObjectState& parameters, GS::ProcessControl& /*processControl*/) const
{
    return ExecuteCornerOperation (parameters, CornerOperation::Fillet);
}

ChamferElementsCommand::ChamferElementsCommand () :
    CommandBase (CommonSchema::Used)
{
}

GS::String ChamferElementsCommand::GetName () const
{
    return "ChamferElements";
}

GS::Optional<GS::UniString> ChamferElementsCommand::GetInputParametersSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "elementPairs": {
                "type": "array",
                "description": "The pairs of straight walls or straight lines whose corner is cut. Both elements of a pair are walls, or both are lines; their lines need not meet, the corner is where they would.",
                "items": {
                    "type": "object",
                    "properties": {
                        "elementId1": {
                            "$ref": "#/ElementId"
                        },
                        "elementId2": {
                            "$ref": "#/ElementId"
                        },
                        "distance1": {
                            "type": "number",
                            "description": "The distance of the chamfer from the corner along the first element.",
                            "exclusiveMinimum": 0
                        },
                        "distance2": {
                            "type": "number",
                            "description": "Optional. The distance of the chamfer from the corner along the second element. By default it is distance1.",
                            "exclusiveMinimum": 0
                        }
                    },
                    "additionalProperties": false,
                    "required": ["elementId1", "elementId2", "distance1"]
                }
            }
        },
        "additionalProperties": false,
        "required": ["elementPairs"]
    })";
}

GS::Optional<GS::UniString> ChamferElementsCommand::GetRawResponseSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "connectingElements": {
                "type": "array",
                "description": "For each pair, in the order of the input: the new element between the two trimmed elements (a wall with the settings of the first wall, or a line with the settings of the first line), or an error.",
                "items": {
                    "$ref": "#/ElementIdOrError"
                }
            }
        },
        "additionalProperties": false,
        "required": ["connectingElements"]
    })";
}

GS::ObjectState ChamferElementsCommand::Execute (const GS::ObjectState& parameters, GS::ProcessControl& /*processControl*/) const
{
    return ExecuteCornerOperation (parameters, CornerOperation::Chamfer);
}

// ---------------------------------------------------------------------------------------------
// Offset Edge
// ---------------------------------------------------------------------------------------------

namespace {

GS::ObjectState OffsetEdge (const GS::ObjectState& item)
{
    const GS::ObjectState* elementId = item.Get ("elementId");
    Int32 edgeIndex = -1;
    double offset = 0.0;
    if (elementId == nullptr || !item.Get ("edgeIndex", edgeIndex) || !item.Get ("offset", offset)) {
        return CreateFailedExecutionResult (APIERR_BADPARS, "elementId, edgeIndex and offset are required.");
    }
    Int32 contourIndex = 0;
    item.Get ("contourIndex", contourIndex);
    const API_Guid guid = GetGuidFromObjectState (*elementId);

    const GS::Optional<GS::ObjectState> notEditable = CheckEditable (guid, "changed");
    if (notEditable.HasValue ()) {
        return CreateFailedExecutionResult (*notEditable);
    }
    API_Element element;
    ClearElement (element, guid);
    GSErrCode err = ACAPI_Element_Get (&element);
    if (err != NoError) {
        return CreateFailedExecutionResult (err, "Failed to read the element.");
    }
    switch (GetElemTypeId (element.header)) {
        case API_SlabID:
        case API_HatchID:
            break;
        case API_ZoneID:
            if (!element.zone.manual) {
                return CreateFailedExecutionResult (APIERR_BADELEMENTTYPE, "The shape of an automatic zone follows its boundaries; only manual zones can be changed.");
            }
            break;
        case API_RoofID:
            if (element.roof.roofClass != API_PlaneRoofID && element.roof.roofClass != API_PolyRoofID) {
                return CreateFailedExecutionResult (APIERR_BADELEMENTTYPE, "Only single-plane and multi-plane roofs are supported.");
            }
            break;
        default:
            return CreateFailedExecutionResult (APIERR_BADELEMENTTYPE, "Only slabs, hatches, manual zones and roofs are supported.");
    }

    // Slabs and roofs keep edge data (trims, side materials) along the polygon. Their polygon is
    // written with ACAPI_Element_ChangeMemo together with that data, as ModifySlabs does it:
    // ACAPI_Element_Change refuses a slab polygon (APIERR_BADPARS), even with the same number of
    // vertices.
    // A multi-plane roof is shaped by its pivot polygon (the additional polygon of the memo),
    // which ModifyRoofs writes the same way; its outline follows from it.
    const API_ElemTypeID typeID = GetElemTypeId (element.header);
    const bool isPivotPolygon = typeID == API_RoofID && element.roof.roofClass == API_PolyRoofID;
    const bool hasEdgeData = !isPivotPolygon && (typeID == API_SlabID || typeID == API_RoofID);
    const UInt64 memoMask = isPivotPolygon ? APIMemoMask_AdditionalPolygon :
                            hasEdgeData    ? (APIMemoMask_Polygon | APIMemoMask_SideMaterials | APIMemoMask_EdgeTrims) : APIMemoMask_Polygon;
    API_ElementMemo memo = {};
    const GS::OnExit memoGuard ([&memo] () { ACAPI_DisposeElemMemoHdls (&memo); });
    err = ACAPI_Element_GetMemo (guid, &memo, memoMask);
    API_Coord** coords = isPivotPolygon ? memo.additionalPolyCoords : memo.coords;
    Int32** pends = isPivotPolygon ? memo.additionalPolyPends : memo.pends;
    API_PolyArc** parcs = isPivotPolygon ? memo.additionalPolyParcs : memo.parcs;
    if (err != NoError || coords == nullptr || pends == nullptr) {
        return CreateFailedExecutionResult (err != NoError ? err : APIERR_GENERAL, "Failed to read the polygon of the element.");
    }

    const Int32 nContours = (Int32) HandleItemCount (pends) - 1;
    if (contourIndex < 0 || contourIndex >= nContours) {
        return CreateFailedExecutionResult (APIERR_BADPARS, GS::UniString::Printf ("contourIndex is out of range: the polygon has %d contours (0 is the outline, the others are holes).", (int) nContours));
    }
    const Int32 first = (*pends)[contourIndex] + 1;
    const Int32 closing = (*pends)[contourIndex + 1];
    const Int32 nVertices = closing - first;
    if (nVertices < 3) {
        return CreateFailedExecutionResult (APIERR_GENERAL, "The contour has fewer than three vertices.");
    }
    if (edgeIndex < 0 || edgeIndex >= nVertices) {
        return CreateFailedExecutionResult (APIERR_BADPARS, GS::UniString::Printf ("edgeIndex is out of range: the contour has %d edges.", (int) nVertices));
    }

    // Edge k runs from vertex k to vertex k + 1 of the contour (the last one back to vertex 0).
    auto vertexIndex = [&] (Int32 k) { return first + ((k % nVertices) + nVertices) % nVertices; };
    auto isArcEdge = [&] (Int32 k) {
        const Int32 beg = vertexIndex (k);
        const Int32 end = beg + 1;
        for (USize a = 0; a < HandleItemCount (parcs); ++a) {
            const API_PolyArc& arc = (*parcs)[a];
            if (std::abs (arc.arcAngle) > AngleEps && ((arc.begIndex == beg && arc.endIndex == end) || (arc.begIndex == end && arc.endIndex == beg))) {
                return true;
            }
        }
        return false;
    };
    if (isArcEdge (edgeIndex) || isArcEdge (edgeIndex - 1) || isArcEdge (edgeIndex + 1)) {
        return CreateFailedExecutionResult (APIERR_BADPARS, "Only a straight edge between straight edges can be offset.");
    }

    double doubleArea = 0.0;
    for (Int32 k = 0; k < nVertices; ++k) {
        doubleArea += Cross ((*coords)[vertexIndex (k)], (*coords)[vertexIndex (k + 1)]);
    }
    const API_Coord prev = (*coords)[vertexIndex (edgeIndex - 1)];
    const API_Coord a = (*coords)[vertexIndex (edgeIndex)];
    const API_Coord b = (*coords)[vertexIndex (edgeIndex + 1)];
    const API_Coord next = (*coords)[vertexIndex (edgeIndex + 2)];
    const API_Coord edgeDir = Minus (b, a);
    if (Length (edgeDir) <= LengthEps) {
        return CreateFailedExecutionResult (APIERR_GENERAL, "The edge has no length.");
    }
    // Away from the inside of the contour; for a hole that is towards the element.
    const API_Coord rightNormal = Normalized (MakeCoord (edgeDir.y, -edgeDir.x));
    API_Coord outward = doubleArea > 0.0 ? rightNormal : Times (rightNormal, -1.0);
    if (contourIndex > 0) {
        outward = Times (outward, -1.0);
    }
    const API_Coord shifted = Plus (a, Times (outward, offset));

    // The neighbouring edges keep their directions: the edge's new end points are where its
    // shifted line meets their lines.
    API_Coord newA, newB;
    if (!IntersectLines (shifted, edgeDir, prev, Minus (a, prev), newA)) {
        newA = shifted;
    }
    if (!IntersectLines (shifted, edgeDir, b, Minus (next, b), newB)) {
        newB = Plus (b, Times (outward, offset));
    }

    auto setVertex = [&] (Int32 k, const API_Coord& c) {
        const Int32 index = vertexIndex (k);
        (*coords)[index] = c;
        if (index == first) {
            (*coords)[closing] = c;
        }
    };
    setVertex (edgeIndex, newA);
    setVertex (edgeIndex + 1, newB);

    if (hasEdgeData) {
        API_Guid changedGuid = guid;
        err = ACAPI_Element_ChangeMemo (changedGuid, memoMask, &memo);
    } else {
        API_Element mask;
        ACAPI_ELEMENT_MASK_CLEAR (mask);
        err = ACAPI_Element_Change (&element, &mask, &memo, memoMask, true);
    }
    if (err != NoError) {
        return CreateFailedExecutionResult (err, "Failed to change the polygon of the element.");
    }
    return CreateSuccessfulExecutionResult ();
}

} // namespace

OffsetEdgesCommand::OffsetEdgesCommand () :
    CommandBase (CommonSchema::Used)
{
}

GS::String OffsetEdgesCommand::GetName () const
{
    return "OffsetEdges";
}

GS::Optional<GS::UniString> OffsetEdgesCommand::GetInputParametersSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "edgesToOffset": {
                "type": "array",
                "description": "The edges to move parallel to themselves, one edge of one element per item. The neighbouring edges keep their directions and get longer or shorter. Several items may name the same element; they are applied one after the other.",
                "items": {
                    "type": "object",
                    "properties": {
                        "elementId": {
                            "$ref": "#/ElementId",
                            "description": "A slab, a hatch, a manual zone or a roof (of a multi-plane roof, the edges of its pivot polygon, as GetDetailsOfElements lists it in pivotPolygonOutline)."
                        },
                        "contourIndex": {
                            "type": "integer",
                            "description": "Optional. The contour of the edge: 0 (the default) is the outline, 1 and up are the holes in the order GetDetailsOfElements lists them.",
                            "minimum": 0
                        },
                        "edgeIndex": {
                            "type": "integer",
                            "description": "The edge of the contour: edge k runs from its vertex k to vertex k + 1 (the last edge back to vertex 0), with the vertices in the order GetDetailsOfElements lists them. The edge and its two neighbours must be straight.",
                            "minimum": 0
                        },
                        "offset": {
                            "type": "number",
                            "description": "The distance to move the edge. A positive value moves it away from the inside of the element, which makes the element larger; a negative one makes it smaller."
                        }
                    },
                    "additionalProperties": false,
                    "required": ["elementId", "edgeIndex", "offset"]
                }
            }
        },
        "additionalProperties": false,
        "required": ["edgesToOffset"]
    })";
}

GS::Optional<GS::UniString> OffsetEdgesCommand::GetRawResponseSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "executionResults": {
                "$ref": "#/ExecutionResults"
            }
        },
        "additionalProperties": false,
        "required": ["executionResults"]
    })";
}

GS::ObjectState OffsetEdgesCommand::Execute (const GS::ObjectState& parameters, GS::ProcessControl& /*processControl*/) const
{
    GS::Array<GS::ObjectState> edgesToOffset;
    parameters.Get ("edgesToOffset", edgesToOffset);

    GS::ObjectState response;
    const auto& executionResults = response.AddList<GS::ObjectState> ("executionResults");

    ACAPI_CallUndoableCommand ("Offset Edges", [&] () -> GSErrCode {
        for (const GS::ObjectState& item : edgesToOffset) {
            executionResults (OffsetEdge (item));
        }
        return NoError;
    });

    return response;
}

// ---------------------------------------------------------------------------------------------
// Resize and Multiply: transformations ACAPI_Element_Edit makes
// ---------------------------------------------------------------------------------------------

namespace {

inline API_Coord3D MakeCoord3D (double x, double y, double z)
{
    API_Coord3D c;
    c.x = x;
    c.y = y;
    c.z = z;
    return c;
}

// Runs one ACAPI_Element_Edit on one element. Returns the guid of the edited element: the new
// copy when withDelete is false.
GSErrCode EditElement (const API_Guid& guid, const API_EditPars& editPars, API_Guid& resultGuid)
{
    GS::Array<API_Neig> neigs = { API_Neig (guid) };
    const GSErrCode err = ACAPI_Element_Edit (&neigs, editPars);
    if (err != NoError) {
        return err;
    }
    resultGuid = neigs.IsEmpty () ? APINULLGuid : neigs[0].guid;
    if (resultGuid == APINULLGuid || (!editPars.withDelete && resultGuid == guid)) {
        return APIERR_GENERAL;
    }
    return NoError;
}

API_EditPars ResizePars (const API_Coord& origin, double ratio)
{
    // The ratio is the length of the second vector over the first one, as in the DevKit's
    // Notification_Manager example, which turns the ratio of a resize back into edit parameters.
    API_EditPars editPars = {};
    editPars.typeID = APIEdit_Resize;
    editPars.withDelete = true;
    editPars.origC = origin;
    editPars.begC = MakeCoord3D (origin.x, origin.y, 0.0);
    editPars.endC = MakeCoord3D (origin.x + 1.0, origin.y, 0.0);
    editPars.endC2 = MakeCoord3D (origin.x + ratio, origin.y, 0.0);
    return editPars;
}

API_EditPars RotatePars (const API_Coord& origin, double angle, bool copy)
{
    API_EditPars editPars = {};
    editPars.typeID = APIEdit_Rotate;
    editPars.withDelete = !copy;
    editPars.origC = origin;
    editPars.begC = MakeCoord3D (origin.x + 1.0, origin.y, 0.0);
    editPars.endC = MakeCoord3D (origin.x + std::cos (angle), origin.y + std::sin (angle), 0.0);
    return editPars;
}

API_EditPars DragPars (const API_Coord3D& vector, bool copy)
{
    API_EditPars editPars = {};
    editPars.typeID = APIEdit_Drag;
    editPars.withDelete = !copy;
    editPars.endC = vector;
    return editPars;
}

} // namespace

ResizeElementsCommand::ResizeElementsCommand () :
    CommandBase (CommonSchema::Used)
{
}

GS::String ResizeElementsCommand::GetName () const
{
    return "ResizeElements";
}

GS::Optional<GS::UniString> ResizeElementsCommand::GetInputParametersSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "elements": {
                "$ref": "#/Elements",
                "description": "The elements to resize, like the Resize command of Archicad does: their plan geometry is scaled about the origin."
            },
            "origin": {
                "$ref": "#/Coordinate2D",
                "description": "The fixed point of the resize on the floor plan."
            },
            "ratio": {
                "type": "number",
                "description": "The scale factor: 2 doubles the distances from the origin, 0.5 halves them.",
                "exclusiveMinimum": 0
            },
            "copy": {
                "type": "boolean",
                "description": "Optional. If true, resized copies are made and the originals are kept. By default it's false."
            }
        },
        "additionalProperties": false,
        "required": ["elements", "origin", "ratio"]
    })";
}

GS::Optional<GS::UniString> ResizeElementsCommand::GetRawResponseSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "resizedElements": {
                "type": "array",
                "description": "For each element, in the order of the input: the resized element (the element itself, or its copy when copy is true), or an error.",
                "items": {
                    "$ref": "#/ElementIdOrError"
                }
            }
        },
        "additionalProperties": false,
        "required": ["resizedElements"]
    })";
}

GS::ObjectState ResizeElementsCommand::Execute (const GS::ObjectState& parameters, GS::ProcessControl& /*processControl*/) const
{
    GS::Array<GS::ObjectState> elements;
    parameters.Get ("elements", elements);
    const GS::ObjectState* originOs = parameters.Get ("origin");
    double ratio = 0.0;
    parameters.Get ("ratio", ratio);
    bool copy = false;
    parameters.Get ("copy", copy);
    if (originOs == nullptr || ratio <= 0.0) {
        return CreateErrorResponse (APIERR_BADPARS, "origin and a positive ratio are required.");
    }
    // ACAPI_Element_Edit resizes the element itself even with withDelete false (tested in
    // Archicad 30), so a resized copy is a copy made by dragging there and back, then resized.
    const API_EditPars editPars = ResizePars (Get2DCoordinateFromObjectState (*originOs), ratio);

    GS::ObjectState response;
    const auto& resizedElements = response.AddList<GS::ObjectState> ("resizedElements");

    ACAPI_CallUndoableCommand ("Resize Elements", [&] () -> GSErrCode {
        for (const GS::ObjectState& item : elements) {
            const API_Guid guid = GetGuidFromElementsArrayItem (item);
            const GS::Optional<GS::ObjectState> notEditable = CheckEditable (guid, "resized");
            if (notEditable.HasValue ()) {
                resizedElements (*notEditable);
                continue;
            }
            API_Guid targetGuid = guid;
            GSErrCode err = NoError;
            if (copy) {
                err = EditElement (guid, DragPars (MakeCoord3D (1.0, 0.0, 0.0), true), targetGuid);
                if (err == NoError) {
                    err = EditElement (targetGuid, DragPars (MakeCoord3D (-1.0, 0.0, 0.0), false), targetGuid);
                }
                if (err != NoError) {
                    resizedElements (CreateErrorResponse (err, "Failed to copy the element."));
                    continue;
                }
            }
            API_Guid resultGuid = APINULLGuid;
            err = EditElement (targetGuid, editPars, resultGuid);
            resizedElements (err == NoError ? CreateElementIdObjectState (resultGuid) : CreateErrorResponse (err, "Failed to resize the element."));
        }
        return NoError;
    });

    return response;
}

MultiplyElementsCommand::MultiplyElementsCommand () :
    CommandBase (CommonSchema::Used)
{
}

GS::String MultiplyElementsCommand::GetName () const
{
    return "MultiplyElements";
}

GS::Optional<GS::UniString> MultiplyElementsCommand::GetInputParametersSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "elements": {
                "$ref": "#/Elements",
                "description": "The elements to multiply. Each element is copied on its own (a wall with its windows and doors, like Archicad copies it)."
            },
            "numberOfCopies": {
                "type": "integer",
                "description": "The number of copies of each element.",
                "minimum": 1
            },
            "moveVector": {
                "$ref": "#/Coordinate3D",
                "description": "Optional. Copy i is moved by i times this vector (with distribute, by i / numberOfCopies times it). A z value elevates the copies. Give moveVector, rotation or both."
            },
            "rotation": {
                "type": "object",
                "description": "Optional. Copy i is rotated about the origin by i times the angle (with distribute, by i / numberOfCopies times it), then moved by its moveVector step when there is one.",
                "properties": {
                    "origin": {
                        "$ref": "#/Coordinate2D",
                        "description": "The center of the rotation on the floor plan."
                    },
                    "angle": {
                        "type": "number",
                        "description": "The angle in radians; positive is counterclockwise."
                    }
                },
                "additionalProperties": false,
                "required": ["origin", "angle"]
            },
            "distribute": {
                "type": "boolean",
                "description": "Optional. If true, moveVector and the rotation angle are the total for the last copy and the copies are spread evenly up to it (the Spread method of Archicad's Multiply). By default it's false: they are the step between two copies (the Increment method)."
            }
        },
        "additionalProperties": false,
        "required": ["elements", "numberOfCopies"]
    })";
}

GS::Optional<GS::UniString> MultiplyElementsCommand::GetRawResponseSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "copiesOfElements": {
                "type": "array",
                "description": "For each element, in the order of the input: its copies, in order, or an error.",
                "items": {
                    "$ref": "#/ElementsWrapperOrError"
                }
            }
        },
        "additionalProperties": false,
        "required": ["copiesOfElements"]
    })";
}

GS::ObjectState MultiplyElementsCommand::Execute (const GS::ObjectState& parameters, GS::ProcessControl& /*processControl*/) const
{
    GS::Array<GS::ObjectState> elements;
    parameters.Get ("elements", elements);
    Int32 numberOfCopies = 0;
    parameters.Get ("numberOfCopies", numberOfCopies);
    const GS::ObjectState* moveVectorOs = parameters.Get ("moveVector");
    const GS::ObjectState* rotationOs = parameters.Get ("rotation");
    bool distribute = false;
    parameters.Get ("distribute", distribute);
    if (numberOfCopies < 1) {
        return CreateErrorResponse (APIERR_BADPARS, "numberOfCopies must be at least 1.");
    }
    if (moveVectorOs == nullptr && rotationOs == nullptr) {
        return CreateErrorResponse (APIERR_BADPARS, "Give moveVector, rotation or both.");
    }
    API_Coord3D moveVector = MakeCoord3D (0.0, 0.0, 0.0);
    if (moveVectorOs != nullptr) {
        moveVector = Get3DCoordinateFromObjectState (*moveVectorOs);
    }
    API_Coord rotationOrigin = MakeCoord (0.0, 0.0);
    double rotationAngle = 0.0;
    if (rotationOs != nullptr) {
        if (rotationOs->Get ("origin") == nullptr || !rotationOs->Get ("angle", rotationAngle)) {
            return CreateErrorResponse (APIERR_BADPARS, "rotation needs origin and angle.");
        }
        rotationOrigin = Get2DCoordinateFromObjectState (*rotationOs->Get ("origin"));
    }
    const double stepScale = distribute ? 1.0 / numberOfCopies : 1.0;

    GS::ObjectState response;
    const auto& copiesOfElements = response.AddList<GS::ObjectState> ("copiesOfElements");

    ACAPI_CallUndoableCommand ("Multiply Elements", [&] () -> GSErrCode {
        for (const GS::ObjectState& item : elements) {
            const API_Guid guid = GetGuidFromElementsArrayItem (item);
            const GS::Optional<GS::ObjectState> notEditable = CheckEditable (guid, "multiplied");
            if (notEditable.HasValue ()) {
                copiesOfElements (*notEditable);
                continue;
            }
            std::vector<API_Guid> copies;
            GSErrCode err = NoError;
            for (Int32 i = 1; i <= numberOfCopies && err == NoError; ++i) {
                const double factor = i * stepScale;
                API_Guid copyGuid = APINULLGuid;
                if (rotationOs != nullptr) {
                    err = EditElement (guid, RotatePars (rotationOrigin, rotationAngle * factor, true), copyGuid);
                }
                if (err == NoError && moveVectorOs != nullptr) {
                    const API_Coord3D step = MakeCoord3D (moveVector.x * factor, moveVector.y * factor, moveVector.z * factor);
                    const bool isCopy = copyGuid == APINULLGuid;
                    err = EditElement (isCopy ? guid : copyGuid, DragPars (step, isCopy), copyGuid);
                }
                if (err == NoError) {
                    copies.push_back (copyGuid);
                }
            }
            if (err != NoError) {
                copiesOfElements (CreateErrorResponse (err, GS::UniString::Printf ("Failed to make copy %u of the element.", (unsigned) (copies.size () + 1))));
                continue;
            }
            copiesOfElements (CreateElementsWrapper (copies));
        }
        return NoError;
    });

    return response;
}
