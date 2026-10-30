#include "DrawingPrimitiveCommands.hpp"
#include "ElementCreationCommands.hpp"
#include "MigrationHelper.hpp"
#include "Base64Converter.hpp"

#include <cstring>

// ── Primitive collector ───────────────────────────────────────────────────────

// The largest picture whose image is returned by default, in bytes before base64 encoding.
static const GSSize DefaultMaxPictureDataSize = 4 * 1024 * 1024;

// The shape primitive callback has no user pointer, so - like CollectCutFillPolygons in
// ElementCommands.cpp - it writes into a collector that is only set while the decomposition runs.
struct DrawingPrimitiveCollectorContext {
    GS::Array<GS::ObjectState> primitives;

    // Pictures: when includePictureData is set, the image of each Pict primitive no larger than
    // maxPictureDataSize is returned too.
    bool   includePictureData = false;
    GSSize maxPictureDataSize = DefaultMaxPictureDataSize;

    struct CachedAttributeGuid {
        API_AttrTypeID     typeID;
        API_AttributeIndex index;
        API_Guid           guid;
    };
    GS::Array<CachedAttributeGuid> attributeGuids;

    // An element is drawn from many primitives, nearly all on the same layer, with a handful of
    // line types and fills, so each attribute is looked up once.
    API_Guid GetAttributeGuid (API_AttrTypeID typeID, const API_AttributeIndex& index)
    {
        for (const CachedAttributeGuid& cached : attributeGuids) {
            if (cached.typeID == typeID && cached.index == index) {
                return cached.guid;
            }
        }
        const API_Guid guid = GetAttributeGuidFromIndex (typeID, index);
        const CachedAttributeGuid cached = { typeID, index, guid };
        attributeGuids.Push (cached);
        return guid;
    }
};

static thread_local DrawingPrimitiveCollectorContext* tl_drawingPrimitiveCtx = nullptr;

static const char* PrimitiveTypeToString (API_PrimTypeID typeID)
{
    switch (typeID) {
        case API_PrimPointID:                    return "Point";
        case API_PrimLineID:                     return "Line";
        case API_PrimArcID:                      return "Arc";
        case API_PrimTextID:                     return "Text";
        case API_PrimPLineID:                    return "PLine";
        case API_PrimTriID:                      return "Tri";
        case API_PrimPolyID:                     return "Poly";
        case API_PrimPictID:                     return "Pict";
        case API_PrimCtrl_BegID:                 return "Ctrl_Beg";
        case API_PrimCtrl_HatchBorderBegID:      return "Ctrl_HatchBorderBeg";
        case API_PrimCtrl_HatchBorderEndID:      return "Ctrl_HatchBorderEnd";
        case API_PrimCtrl_HatchLinesBegID:       return "Ctrl_HatchLinesBeg";
        case API_PrimCtrl_HatchLinesEndID:       return "Ctrl_HatchLinesEnd";
        case API_PrimCtrl_HoledimLinesBegID:     return "Ctrl_HoledimLinesBeg";
        case API_PrimCtrl_HoledimLinesEndID:     return "Ctrl_HoledimLinesEnd";
        case API_PrimCtrl_ArrowBegID:            return "Ctrl_ArrowBeg";
        case API_PrimCtrl_ArrowEndID:            return "Ctrl_ArrowEnd";
        case API_PrimCtrl_ElementRefID:          return "Ctrl_ElementRef";
        case API_PrimCtrl_EndID:                 return "Ctrl_End";
        case API_PrimCtrl_PlacedBorderBegID:     return "Ctrl_PlacedBorderBeg";
        case API_PrimCtrl_PlacedBorderEndID:     return "Ctrl_PlacedBorderEnd";
        case API_PrimCtrl_TextBegID:             return "Ctrl_TextBeg";
        case API_PrimCtrl_TextEndID:             return "Ctrl_TextEnd";
        case API_PrimCtrl_CWallPanelBegID:       return "Ctrl_CWallPanelBeg";
        case API_PrimCtrl_CWallPanelEndID:       return "Ctrl_CWallPanelEnd";
#ifdef ServerMainVers_2800
        case API_PrimCtrl_BackgroundFillBegID:   return "Ctrl_BackgroundFillBeg";
        case API_PrimCtrl_BackgroundFillEndID:   return "Ctrl_BackgroundFillEnd";
        case API_PrimCtrl_FrameBegID:            return "Ctrl_FrameBeg";
        case API_PrimCtrl_FrameEndID:            return "Ctrl_FrameEnd";
#endif
        default:                                 return "Unknown";
    }
}

static const char* RenovationStatusToString (API_RenovationStatusType status)
{
    switch (status) {
        case API_ExistingStatus:   return "Existing";
        case API_NewStatus:        return "New";
        case API_DemolishedStatus: return "Demolished";
        default:                   return "Undefined";
    }
}

static GS::ObjectState CreatePrimitiveHeadObjectState (const API_Prim_Head& head, DrawingPrimitiveCollectorContext& ctx)
{
    GS::ObjectState os ("type", PrimitiveTypeToString (head.typeID));
    const API_Guid layerGuid = ctx.GetAttributeGuid (API_LayerID, head.layer);
    if (layerGuid != APINULLGuid) {
        os.Add ("layer", CreateAttributeIdObjectState (layerGuid));
    }
    os.Add ("pen", head.pen.penIndex);
    os.Add ("drwIndex", head.drwIndex);
    os.Add ("renovationStatus", RenovationStatusToString (head.renovationStatus));
    return os;
}

static void AddLineTypeToObjectState (GS::ObjectState& os, const API_AttributeIndex& ltypeInd, DrawingPrimitiveCollectorContext& ctx)
{
    const API_Guid lineTypeGuid = ctx.GetAttributeGuid (API_LinetypeID, ltypeInd);
    if (lineTypeGuid != APINULLGuid) {
        os.Add ("lineType", CreateAttributeIdObjectState (lineTypeGuid));
    }
}

// coords is 1 based, as CollectCutFillPolygons reads it.
template <typename ListAdder>
static void AddCoordinatesToList (const ListAdder& list, const API_Coord* coords, Int32 begIndex, Int32 endIndex)
{
    for (Int32 i = begIndex; i <= endIndex; ++i) {
        list (Create2DCoordinateObjectState (coords[i]));
    }
}

// The arcs of a polyline or polygon primitive come in par3 (APIdefs_Callback.h, ShapePrimsProc):
// nArcs API_PolyArc items, 0 based like the parcs of an API_ElementMemo, whose begIndex and endIndex
// are 1 based indices into the coordinates in par1 (so read in Archicad 30, matching the arcs
// GetDetailsOfElements returns for the same elements). Adds the arcs whose both ends are in
// [begIndex, endIndex] as "arcs", with the indices made 0 based within that range, the way
// GetDetailsOfElements returns polygonArcs. An arc with an index outside the range is not added.
static void AddArcsToObjectState (GS::ObjectState& os, const API_PolyArc* arcs, Int32 nArcs, Int32 begIndex, Int32 endIndex)
{
    if (arcs == nullptr || nArcs <= 0) {
        return;
    }
    GS::Array<GS::ObjectState> arcsInRange;
    for (Int32 i = 0; i < nArcs; ++i) {
        const API_PolyArc& arc = arcs[i];
        if (arc.begIndex < begIndex || arc.begIndex > endIndex || arc.endIndex < begIndex || arc.endIndex > endIndex) {
            continue;
        }
        arcsInRange.Push (GS::ObjectState (
            "begIndex", arc.begIndex - begIndex,
            "endIndex", arc.endIndex - begIndex,
            "arcAngle", arc.arcAngle));
    }
    if (arcsInRange.IsEmpty ()) {
        return;
    }
    const auto& arcList = os.AddList<GS::ObjectState> ("arcs");
    for (const GS::ObjectState& arc : arcsInRange) {
        arcList (arc);
    }
}

// The length of a NUL terminated string of at most maxLength characters.
template <typename CharType>
static USize GetBoundedLength (const CharType* str, USize maxLength)
{
    USize length = 0;
    while (length < maxLength && str[length] != 0) {
        ++length;
    }
    return length;
}

static const char* PictureFormatToString (API_PictureFormat format)
{
    switch (format) {
        case APIPictForm_Bitmap: return "Bitmap";
        case APIPictForm_GIF:    return "GIF";
        case APIPictForm_TIFF:   return "TIFF";
        case APIPictForm_JPEG:   return "JPEG";
        case APIPictForm_PNG:    return "PNG";
        default:                 return "Default";
    }
}

static GS::ObjectState CreateRGBColorObjectState (const API_RGBColor& color)
{
    return GS::ObjectState ("red", color.f_red, "green", color.f_green, "blue", color.f_blue);
}

// The pen index that stands for an RGB color in API_PrimHatchBorder (APIdefs_Elements.h).
static const short RGBPenIndex = 1008;

// API_PrimHatchBorder: the par1 of Ctrl_HatchBorderBeg, and of Ctrl_HatchLinesBeg and
// Ctrl_PlacedBorderBeg when it is not nullptr (read so by the DevKit's Element_Test example, Dumper.cpp).
static GS::ObjectState CreateHatchBorderObjectState (const API_PrimHatchBorder& border, DrawingPrimitiveCollectorContext& ctx)
{
    GS::ObjectState hatch;
    hatch.Add ("doFrame", border.doFrame);
    const API_Guid fillGuid = ctx.GetAttributeGuid (API_FilltypeID, border.fillInd);
    if (fillGuid != APINULLGuid) {
        hatch.Add ("fill", CreateAttributeIdObjectState (fillGuid));
    }
    hatch.Add ("fillPen", border.fillPen.penIndex);
    hatch.Add ("fillbkgPen", border.fillbkgPen);
    hatch.Add ("determination", border.determination);
    hatch.Add ("transparency", border.transparency);
    if (border.fillPen.penIndex == RGBPenIndex) {
        hatch.Add ("fillRgb", CreateRGBColorObjectState (border.fillRgb));
    }
    if (border.fillbkgPen == RGBPenIndex) {
        hatch.Add ("fillbkgRgb", CreateRGBColorObjectState (border.fillbkgRgb));
    }
    hatch.Add ("isGradient", border.isGradient);
    hatch.Add ("isLinear", border.isLinear);
    const API_HatchOrientation& orientation = border.hatchOrientation;
    hatch.Add ("hatchOrientation", GS::ObjectState (
        "type", static_cast<Int32> (orientation.type),
        "flags", orientation.flags,
        "origo", Create2DCoordinateObjectState (orientation.origo),
        "matrix00", orientation.matrix00,
        "matrix10", orientation.matrix10,
        "matrix01", orientation.matrix01,
        "matrix11", orientation.matrix11));
    return hatch;
}

// The MIME type of a stored picture: from its signature for the common formats, otherwise the one
// Archicad gives in API_PrimPict.mime.
static GS::UniString GetPictureDataMimeType (const char* bytes, GSSize size, const GS::UniString& mimeType)
{
    const auto startsWith = [&] (const char* signature, GSSize length) {
        return size >= length && memcmp (bytes, signature, static_cast<size_t> (length)) == 0;
    };
    if (startsWith ("\x89PNG\r\n\x1a\n", 8)) {
        return "image/png";
    }
    if (startsWith ("\xff\xd8\xff", 3)) {
        return "image/jpeg";
    }
    if (startsWith ("GIF87a", 6) || startsWith ("GIF89a", 6)) {
        return "image/gif";
    }
    if (startsWith ("II*\0", 4) || startsWith ("MM\0*", 4)) {
        return "image/tiff";
    }
    if (startsWith ("BM", 2)) {
        return "image/bmp";
    }
    return mimeType;
}

// par1 of a Pict primitive is the picture data as a handle ("(char **) par1 picture data" in
// APIdefs_Elements.h). Checked before use, so a parameter that is not a readable handle is skipped.
static GSSize GetPictureDataSize (const void* par1)
{
    if (par1 == nullptr || !BNValidReadPtr (par1, sizeof (GSPtr))) {
        return 0;
    }
    const GSConstHandle handle = static_cast<GSConstHandle> (par1);
    if (*handle == nullptr) {
        return 0;
    }
    const GSSize size = BMGetHandleSize (handle);
    if (size <= 0 || !BNValidReadPtr (*handle, size)) {
        return 0;
    }
    return size;
}

static GSErrCode CollectDrawingPrimitives (const API_PrimElement* primElem,
                                           const void* par1, const void* par2, const void* par3)
{
    if (tl_drawingPrimitiveCtx == nullptr || primElem == nullptr)
        return NoError;

    DrawingPrimitiveCollectorContext& ctx = *tl_drawingPrimitiveCtx;
    GS::ObjectState os = CreatePrimitiveHeadObjectState (primElem->header, ctx);

    switch (primElem->header.typeID) {
        case API_PrimPointID:
            os.Add ("loc", Create2DCoordinateObjectState (primElem->point.loc));
            break;
        case API_PrimLineID: {
            const API_PrimLine& line = primElem->line;
            AddLineTypeToObjectState (os, line.ltypeInd, ctx);
            os.Add ("determination", line.determination);
            os.Add ("penWeight", line.penWeight);
            os.Add ("c1", Create2DCoordinateObjectState (line.c1));
            os.Add ("c2", Create2DCoordinateObjectState (line.c2));
            break;
        }
        case API_PrimArcID: {
            const API_PrimArc& arc = primElem->arc;
            AddLineTypeToObjectState (os, arc.ltypeInd, ctx);
            os.Add ("solid", arc.solid);
            os.Add ("whole", arc.whole);
            os.Add ("determination", arc.determination);
            os.Add ("reflected", arc.reflected);
            os.Add ("penWeight", arc.penWeight);
            os.Add ("orig", Create2DCoordinateObjectState (arc.orig));
            os.Add ("r", arc.r);
            os.Add ("begAng", arc.begAng);
            os.Add ("endAng", arc.endAng);
            os.Add ("angle", arc.angle);
            os.Add ("ratio", arc.ratio);
            break;
        }
        case API_PrimTextID: {
            const API_PrimText& text = primElem->text;
            os.Add ("font", text.font);
            os.Add ("faceBits", static_cast<Int32> (text.faceBits));
            os.Add ("effectBits", text.effectBits);
            os.Add ("anchor", AnchorIdToString (text.anchor));
            os.Add ("just", JustificationToString (text.just));
            os.Add ("loc", Create2DCoordinateObjectState (text.loc));
            os.Add ("heightMM", text.heightMM);
            os.Add ("widthMM", text.widthMM);
            os.Add ("angle", text.angle);
            os.Add ("widthFactor", text.widthFactor);
            os.Add ("charSpaceFactor", text.charSpaceFactor);
            // par2 is the content as a NUL terminated UTF-16 string, or nullptr (the ShapePrimsProc
            // table in APIdefs_Callback.h; the DevKit's Element_Test example reads it so in Dumper.cpp).
            // The length is bounded, so a missing terminator cannot run through memory.
            const auto* content = static_cast<const GS::UniChar::Layout*> (par2);
            if (content != nullptr) {
                const USize MaxTextLength = 65536;
                os.Add ("text", GS::UniString (content, GetBoundedLength (content, MaxTextLength)));
            }
            break;
        }
        case API_PrimPLineID: {
            const API_PrimPLine& pline = primElem->pline;
            AddLineTypeToObjectState (os, pline.ltypeInd, ctx);
            os.Add ("determination", pline.determination);
            os.Add ("drawSegmentMode", pline.drawSegmentMode);
            os.Add ("nCoords", pline.nCoords);
            os.Add ("nArcs", pline.nArcs);
            os.Add ("penWeight", pline.penWeight);
            // par1 holds the vertices, 1 based like the coordinates of a Poly (in Archicad 30 the
            // unused item 0 is (-1, 0)); par3 the arcs.
            const auto* coords = static_cast<const API_Coord*> (par1);
            if (coords != nullptr && pline.nCoords > 0) {
                const auto& coordinates = os.AddList<GS::ObjectState> ("coordinates");
                AddCoordinatesToList (coordinates, coords, 1, pline.nCoords);
                AddArcsToObjectState (os, static_cast<const API_PolyArc*> (par3), pline.nArcs, 1, pline.nCoords);
            }
            break;
        }
        case API_PrimTriID: {
            const API_PrimTri& tri = primElem->tri;
            AddLineTypeToObjectState (os, tri.ltypeInd, ctx);
            os.Add ("solid", tri.solid);
            os.Add ("determination", tri.determination);
            os.Add ("penWeight", tri.penWeight);
            const auto& coordinates = os.AddList<GS::ObjectState> ("coordinates");
            for (const API_Coord& c : tri.c) {
                coordinates (Create2DCoordinateObjectState (c));
            }
            break;
        }
        case API_PrimPolyID: {
            const API_PrimPoly& poly = primElem->poly;
            AddLineTypeToObjectState (os, poly.ltypeInd, ctx);
            os.Add ("solid", poly.solid);
            os.Add ("useRgb", poly.useRgb);
            os.Add ("determination", poly.determination);
            os.Add ("penWeight", poly.penWeight);
            os.Add ("nCoords", poly.nCoords);
            os.Add ("nSubPolys", poly.nSubPolys);
            os.Add ("nArcs", poly.nArcs);
            os.Add ("fillPen", poly.fillPen.penIndex);
            os.Add ("rgbColor", CreateRGBColorObjectState (poly.rgbColor));
            const auto* coords = static_cast<const API_Coord*> (par1);
            const auto* pends  = static_cast<const Int32*> (par2);
            const auto* arcs   = static_cast<const API_PolyArc*> (par3);
            if (coords != nullptr && poly.nCoords > 0) {
                // The first sub polygon is the outline, the others are holes. pends is 1 based and
                // its last entry is nCoords; clamped as in CollectCutFillPolygons. Each arc goes to
                // the sub polygon both of its ends are in.
                const auto& polygons = os.AddList<GS::ObjectState> ("polygons");
                Int32 begIndex = 1;
                const Int32 nSubPolys = (pends != nullptr) ? poly.nSubPolys : 1;
                for (Int32 sub = 1; sub <= nSubPolys && begIndex <= poly.nCoords; ++sub) {
                    Int32 endIndex = (pends != nullptr) ? pends[sub] : poly.nCoords;
                    if (endIndex > poly.nCoords)
                        endIndex = poly.nCoords;
                    if (endIndex < begIndex)
                        break;
                    GS::ObjectState polygon;
                    const auto& coordinates = polygon.AddList<GS::ObjectState> ("coordinates");
                    AddCoordinatesToList (coordinates, coords, begIndex, endIndex);
                    AddArcsToObjectState (polygon, arcs, poly.nArcs, begIndex, endIndex);
                    polygons (polygon);
                    begIndex = endIndex + 1;
                }
            }
            break;
        }
        case API_PrimPictID: {
            const API_PrimPict& pict = primElem->pict;
            os.Add ("rotAngle", pict.rotAngle);
            os.Add ("destBox", GS::ObjectState (
                "xMin", pict.destBox.xMin,
                "yMin", pict.destBox.yMin,
                "xMax", pict.destBox.xMax,
                "yMax", pict.destBox.yMax));
            os.Add ("pixelSizeX", pict.pixelSizeX);
            os.Add ("pixelSizeY", pict.pixelSizeY);
            os.Add ("anchor", AnchorIdToString (pict.anchor));
            os.Add ("mirrored", pict.mirrored);
            os.Add ("transparent", pict.transparent);
            os.Add ("storageFormat", PictureFormatToString (pict.storageFormat));
            const GS::UniString mimeType (pict.mime, GetBoundedLength (pict.mime, sizeof (pict.mime)), CC_UTF8);
            if (!mimeType.IsEmpty ()) {
                os.Add ("mimeType", mimeType);
            }
            const USize nameLength = GetBoundedLength (pict.pictName, API_UniLongNameLen);
            if (nameLength > 0) {
                os.Add ("name", GS::UniString (pict.pictName, nameLength));
            }
            const GSSize dataSize = GetPictureDataSize (par1);
            if (dataSize > 0) {
                os.Add ("pictureDataSize", static_cast<UInt32> (dataSize));
                if (ctx.includePictureData && dataSize <= ctx.maxPictureDataSize) {
                    // Returned as Archicad stores it: GDL pictures (PICTURE2) come as PNG.
                    const char* data = *static_cast<GSConstHandle> (par1);
                    GS::UniString base64 = Base64Converter::Encode (data, static_cast<USize> (dataSize));
                    base64.DeleteAll (GS::UniChar (char ('\n')));
                    os.Add ("pictureData", base64);
                    const GS::UniString dataMimeType = GetPictureDataMimeType (data, dataSize, mimeType);
                    if (!dataMimeType.IsEmpty ()) {
                        os.Add ("pictureDataMimeType", dataMimeType);
                    }
                }
            }
            break;
        }
        case API_PrimCtrl_HatchBorderBegID:
        case API_PrimCtrl_HatchLinesBegID:
        case API_PrimCtrl_PlacedBorderBegID: {
            // par1 read as CollectCutFillPolygons in ElementCommands.cpp and the DevKit's
            // Element_Test example (Dumper.cpp) read it.
            const auto* border = static_cast<const API_PrimHatchBorder*> (par1);
            if (border != nullptr) {
                os.Add ("hatchBorder", CreateHatchBorderObjectState (*border, ctx));
            }
            break;
        }
        case API_PrimCtrl_ElementRefID: {
            // par1 is an API_PrimElemRef* (APIdefs_Callback.h, ShapePrimsProc), e.g. the doors and
            // windows a wall is drawn with.
            const auto* ref = static_cast<const API_PrimElemRef*> (par1);
            if (ref != nullptr) {
#ifdef ServerMainVers_2600
                const API_ElemTypeID refType = GetElemTypeId (ref->type);
#else
                const API_ElemTypeID refType = ref->typeID;
#endif
                os.Add ("elementRef", GS::ObjectState (
                    "elementId", CreateGuidObjectState (ref->guid),
                    "elementType", GetElementTypeNonLocalizedName (refType),
                    "drwIndex", ref->drwIndex));
            }
            break;
        }
        default:
            break;
    }

    ctx.primitives.Push (os);
    return NoError;
}

static GSErrCode GetDrawingPrimitives (const API_Elem_Head& elemHead, DrawingPrimitiveCollectorContext& ctx)
{
    tl_drawingPrimitiveCtx = &ctx;
    const GS::OnExit ctxGuard ([&] () { tl_drawingPrimitiveCtx = nullptr; });

    // The same parameters as AddFloorPlanPolygonsIfAvailable in ElementCommands.cpp: draw the
    // whole element, whichever story is shown and whatever the window shows of it.
    API_ShapePrimsParams params = {};
    params.dontClip   = true;
    params.allStories = true;
    params.polygon    = nullptr;

#if defined(ServerMainVers_2700)
    return ACAPI_DrawingPrimitive_ShapePrimsExt (elemHead, CollectDrawingPrimitives, &params);
#else
    return ACAPI_Element_ShapePrimsExt (elemHead, CollectDrawingPrimitives, &params);
#endif
}

static GSErrCode GetStoryControlPrimitives (API_DatabaseUnId& databaseUnId, DrawingPrimitiveCollectorContext& ctx)
{
    tl_drawingPrimitiveCtx = &ctx;
    const GS::OnExit ctxGuard ([&] () { tl_drawingPrimitiveCtx = nullptr; });

    return ACAPI_DrawingPrimitive_DrawStoryControl (&databaseUnId, CollectDrawingPrimitives);
}

// The schema of one item of the primitives list, shared by the commands of this file.
static const char* DrawingPrimitiveSchema = R"({
    "type": "object",
    "properties": {
        "type": {
            "type": "string",
            "description": "The API_PrimTypeID name without its API_Prim prefix and ID suffix.",
            "enum": [
                "Point", "Line", "Arc", "Text", "PLine", "Tri", "Poly", "Pict",
                "Ctrl_Beg", "Ctrl_HatchBorderBeg", "Ctrl_HatchBorderEnd", "Ctrl_HatchLinesBeg", "Ctrl_HatchLinesEnd",
                "Ctrl_HoledimLinesBeg", "Ctrl_HoledimLinesEnd", "Ctrl_ArrowBeg", "Ctrl_ArrowEnd", "Ctrl_ElementRef", "Ctrl_End",
                "Ctrl_PlacedBorderBeg", "Ctrl_PlacedBorderEnd", "Ctrl_TextBeg", "Ctrl_TextEnd", "Ctrl_CWallPanelBeg", "Ctrl_CWallPanelEnd",
                "Ctrl_BackgroundFillBeg", "Ctrl_BackgroundFillEnd", "Ctrl_FrameBeg", "Ctrl_FrameEnd", "Unknown"
            ]
        },
        "layer": {
            "$ref": "#/AttributeIdArrayItem",
            "description": "The layer of the element."
        },
        "pen": {
            "type": "integer",
            "description": "The pen index of the primitive."
        },
        "drwIndex": {
            "type": "integer"
        },
        "renovationStatus": {
            "type": "string",
            "enum": ["Undefined", "Existing", "New", "Demolished"]
        },
        "lineType": {
            "$ref": "#/AttributeIdArrayItem",
            "description": "Line, Arc, PLine, Tri and Poly: the line type."
        },
        "determination": {
            "type": "integer",
            "description": "How the type of the line or fill is determined (origin, inner line, contour line)."
        },
        "penWeight": {
            "type": "number"
        },
        "solid": {
            "type": "boolean"
        },
        "loc": {
            "$ref": "#/Coordinate2D",
            "description": "Point: the location. Text: the left baseline position."
        },
        "c1": {
            "$ref": "#/Coordinate2D"
        },
        "c2": {
            "$ref": "#/Coordinate2D"
        },
        "whole": {
            "type": "boolean"
        },
        "reflected": {
            "type": "boolean",
            "description": "The arc goes clockwise."
        },
        "orig": {
            "$ref": "#/Coordinate2D"
        },
        "r": {
            "type": "number"
        },
        "begAng": {
            "type": "number"
        },
        "endAng": {
            "type": "number"
        },
        "angle": {
            "type": "number"
        },
        "ratio": {
            "type": "number"
        },
        "text": {
            "type": "string",
            "description": "Text: the content."
        },
        "font": {
            "type": "integer"
        },
        "faceBits": {
            "type": "integer"
        },
        "effectBits": {
            "type": "integer"
        },
        "anchor": {
            "type": "string"
        },
        "just": {
            "type": "string"
        },
        "heightMM": {
            "type": "number"
        },
        "widthMM": {
            "type": "number"
        },
        "widthFactor": {
            "type": "number"
        },
        "charSpaceFactor": {
            "type": "number"
        },
        "drawSegmentMode": {
            "type": "integer"
        },
        "nCoords": {
            "type": "integer"
        },
        "nSubPolys": {
            "type": "integer"
        },
        "nArcs": {
            "type": "integer",
            "description": "PLine and Poly: the number of curved segments, returned in arcs."
        },
        "useRgb": {
            "type": "boolean"
        },
        "fillPen": {
            "type": "integer"
        },
        "rgbColor": {
            "$ref": "#/ColorRGB"
        },
        "coordinates": {
            "type": "array",
            "description": "PLine: the vertices. Tri: the three corners.",
            "items": {
                "$ref": "#/Coordinate2D"
            }
        },
        "arcs": {
            "type": "array",
            "description": "PLine: the curved segments; begIndex and endIndex are indices into coordinates.",
            "items": {
                "$ref": "#/PolyArc"
            }
        },
        "polygons": {
            "type": "array",
            "description": "Poly: the sub polygons, the first one is the outline, the others are holes.",
            "items": {
                "type": "object",
                "properties": {
                    "coordinates": {
                        "type": "array",
                        "items": {
                            "$ref": "#/Coordinate2D"
                        }
                    },
                    "arcs": {
                        "type": "array",
                        "description": "The curved segments of this polygon; begIndex and endIndex are indices into its coordinates.",
                        "items": {
                            "$ref": "#/PolyArc"
                        }
                    }
                },
                "additionalProperties": false,
                "required": ["coordinates"]
            }
        },
        "rotAngle": {
            "type": "number",
            "description": "Pict: the rotation of the picture in radians."
        },
        "destBox": {
            "type": "object",
            "description": "Pict: the box of the picture before its transformation. Pictures drawn by GDL objects are mirrored about the left edge of the box when mirrored is true, then rotated by rotAngle about its lower left corner (xMin, yMin).",
            "properties": {
                "xMin": { "type": "number" },
                "yMin": { "type": "number" },
                "xMax": { "type": "number" },
                "yMax": { "type": "number" }
            },
            "additionalProperties": false,
            "required": ["xMin", "yMin", "xMax", "yMax"]
        },
        "pixelSizeX": {
            "type": "integer",
            "description": "Pict: the width of the original picture in pixels."
        },
        "pixelSizeY": {
            "type": "integer",
            "description": "Pict: the height of the original picture in pixels."
        },
        "mirrored": {
            "type": "boolean",
            "description": "Pict: the picture is mirrored."
        },
        "transparent": {
            "type": "boolean",
            "description": "Pict: the picture is drawn transparent."
        },
        "storageFormat": {
            "type": "string",
            "description": "Pict: the format the picture is stored in.",
            "enum": ["Default", "Bitmap", "GIF", "TIFF", "JPEG", "PNG"]
        },
        "mimeType": {
            "type": "string",
            "description": "Pict: the MIME type of the stored picture, when Archicad gives one."
        },
        "name": {
            "type": "string",
            "description": "Pict: the name of the picture."
        },
        "pictureDataSize": {
            "type": "integer",
            "description": "Pict: the size of the stored picture in bytes."
        },
        "pictureData": {
            "type": "string",
            "description": "Pict, with includePictureData: the picture as Archicad stores it, base64 encoded, in the format pictureDataMimeType names. Missing when the picture is larger than maxPictureDataSize."
        },
        "pictureDataMimeType": {
            "type": "string",
            "description": "Pict, with pictureData: its MIME type, for example image/png. Pictures drawn by GDL objects with PICTURE2 are stored as PNG."
        },
        "hatchBorder": {
            "type": "object",
            "description": "Ctrl_HatchBorderBeg, and Ctrl_HatchLinesBeg and Ctrl_PlacedBorderBeg when Archicad gives it: the fill that follows.",
            "properties": {
                "doFrame": { "type": "boolean" },
                "fill": { "$ref": "#/AttributeIdArrayItem" },
                "fillPen": { "type": "integer", "description": "The pen of the fill pattern; 1008 means the color in fillRgb." },
                "fillbkgPen": { "type": "integer", "description": "The pen of the fill background; 1008 means the color in fillbkgRgb." },
                "determination": { "type": "integer" },
                "transparency": { "type": "integer" },
                "fillRgb": { "$ref": "#/ColorRGB" },
                "fillbkgRgb": { "$ref": "#/ColorRGB" },
                "isGradient": { "type": "boolean" },
                "isLinear": { "type": "boolean", "description": "A gradient fill is linear, otherwise radial." },
                "hatchOrientation": {
                    "type": "object",
                    "description": "The orientation and distortion of the fill pattern (API_HatchOrientation).",
                    "properties": {
                        "type": { "type": "integer", "description": "0 global, 1 rotated, 2 distorted, 3 centered." },
                        "flags": { "type": "integer" },
                        "origo": { "$ref": "#/Coordinate2D" },
                        "matrix00": { "type": "number" },
                        "matrix10": { "type": "number" },
                        "matrix01": { "type": "number" },
                        "matrix11": { "type": "number" }
                    },
                    "additionalProperties": false,
                    "required": ["type", "flags", "origo", "matrix00", "matrix10", "matrix01", "matrix11"]
                }
            },
            "additionalProperties": false,
            "required": ["doFrame", "fillPen", "fillbkgPen", "determination"]
        },
        "elementRef": {
            "type": "object",
            "description": "Ctrl_ElementRef: the element referred to, for example a door or window of a wall.",
            "properties": {
                "elementId": { "$ref": "#/ElementId" },
                "elementType": { "type": "string" },
                "drwIndex": { "type": "integer" }
            },
            "additionalProperties": false,
            "required": ["elementId", "elementType", "drwIndex"]
        }
    },
    "additionalProperties": false,
    "required": [
        "type",
        "pen",
        "drwIndex",
        "renovationStatus"
    ]
})";

// ── GetDrawingPrimitivesOfElements ────────────────────────────────────────────

GetDrawingPrimitivesOfElementsCommand::GetDrawingPrimitivesOfElementsCommand () :
    CommandBase (CommonSchema::Used)
{
}

GS::String GetDrawingPrimitivesOfElementsCommand::GetName () const
{
    return "GetDrawingPrimitivesOfElements";
}

GS::Optional<GS::UniString> GetDrawingPrimitivesOfElementsCommand::GetInputParametersSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "elements": {
                "$ref": "#/Elements"
            },
            "includePictureData": {
                "type": "boolean",
                "description": "Return the image of each picture primitive (type Pict) in pictureData. False by default."
            },
            "maxPictureDataSize": {
                "type": "integer",
                "minimum": 0,
                "description": "With includePictureData: the largest stored picture, in bytes, whose image is returned. 4194304 (4 MB) by default."
            }
        },
        "additionalProperties": false,
        "required": [
            "elements"
        ]
    })";
}

GS::Optional<GS::UniString> GetDrawingPrimitivesOfElementsCommand::GetRawResponseSchema () const
{
    return GS::UniString (R"({
        "type": "object",
        "properties": {
            "drawingPrimitivesOfElements": {
                "type": "array",
                "description": "The drawing primitives of each element, in the order of the input elements.",
                "items": {
                    "oneOf": [
                        {
                            "type": "object",
                            "properties": {
                                "primitives": {
                                    "type": "array",
                                    "description": "The primitives in the order Archicad draws them. The Ctrl_* items mark the beginning and the end of sub-parts (hatch borders, hatch lines, texts, curtain wall panels, ...).",
                                    "items": )") + DrawingPrimitiveSchema + R"(
                                }
                            },
                            "additionalProperties": false,
                            "required": [
                                "primitives"
                            ]
                        },
                        {
                            "$ref": "#/ErrorItem"
                        }
                    ]
                }
            }
        },
        "additionalProperties": false,
        "required": [
            "drawingPrimitivesOfElements"
        ]
    })";
}

GS::ObjectState GetDrawingPrimitivesOfElementsCommand::Execute (const GS::ObjectState& parameters, GS::ProcessControl& /*processControl*/) const
{
    bool includePictureData = false;
    parameters.Get ("includePictureData", includePictureData);
    Int32 maxPictureDataSize = static_cast<Int32> (DefaultMaxPictureDataSize);
    parameters.Get ("maxPictureDataSize", maxPictureDataSize);
    if (maxPictureDataSize < 0) {
        return CreateErrorResponse (APIERR_BADPARS, "maxPictureDataSize must not be negative.");
    }

    // The elements are floor plan elements and ShapePrims draws them from the current database, so
    // switch the CURRENT DATABASE - not the window the user sees - to the floor plan when another one
    // (a section, a layout, the 3D window, ...) is current, and put the previous one back on every
    // exit path, as Get3DGeometryOfElements does.
    API_DatabaseInfo previousDatabase = {};
    bool databaseSwitched = false;
    GSErrCode err = SwitchCurrentDatabaseToFloorPlan (previousDatabase, databaseSwitched);
    const GS::OnExit restoreDatabase ([&] () {
        if (databaseSwitched) {
            ACAPI_Database_ChangeCurrentDatabase (&previousDatabase);
        }
    });
    if (err != NoError) {
        return CreateErrorResponse (err, "Failed to switch the current database to the floor plan.");
    }

    GS::Array<GS::ObjectState> elements;
    parameters.Get ("elements", elements);

    GS::ObjectState response;
    const auto& drawingPrimitivesOfElements = response.AddList<GS::ObjectState> ("drawingPrimitivesOfElements");

    for (const GS::ObjectState& element : elements) {
        const GS::ObjectState* elementId = element.Get ("elementId");
        if (elementId == nullptr) {
            drawingPrimitivesOfElements (CreateErrorResponse (APIERR_BADPARS, "elementId is missing"));
            continue;
        }

        API_Elem_Head elemHead = {};
        elemHead.guid = GetGuidFromObjectState (*elementId);
        err = ACAPI_Element_GetHeader (&elemHead);
        if (err != NoError) {
            drawingPrimitivesOfElements (CreateErrorResponse (err, "Failed to find element in Archicad"));
            continue;
        }

        DrawingPrimitiveCollectorContext ctx;
        ctx.includePictureData = includePictureData;
        ctx.maxPictureDataSize = maxPictureDataSize;
        err = GetDrawingPrimitives (elemHead, ctx);
        if (err != NoError) {
            drawingPrimitivesOfElements (CreateErrorResponse (err, "Failed to get the drawing primitives of the element"));
            continue;
        }

        GS::ObjectState result;
        const auto& primitives = result.AddList<GS::ObjectState> ("primitives");
        for (const GS::ObjectState& primitive : ctx.primitives) {
            primitives (primitive);
        }

        drawingPrimitivesOfElements (result);
    }

    return response;
}

// ── GetStoryControlPrimitives ─────────────────────────────────────────────────

GetStoryControlPrimitivesCommand::GetStoryControlPrimitivesCommand () :
    CommandBase (CommonSchema::Used)
{
}

GS::String GetStoryControlPrimitivesCommand::GetName () const
{
    return "GetStoryControlPrimitives";
}

GS::Optional<GS::UniString> GetStoryControlPrimitivesCommand::GetInputParametersSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "databaseId": {
                "$ref": "#/DatabaseId",
                "description": "A section, elevation or interior elevation database, for example as GetDatabaseIdFromNavigatorItemId returns it."
            }
        },
        "additionalProperties": false,
        "required": [
            "databaseId"
        ]
    })";
}

GS::Optional<GS::UniString> GetStoryControlPrimitivesCommand::GetRawResponseSchema () const
{
    return GS::UniString (R"({
        "type": "object",
        "properties": {
            "primitives": {
                "type": "array",
                "description": "The primitives of the story levels drawn in the database, in the order Archicad draws them. The Ctrl_* items mark the beginning and the end of sub-parts (texts, ...).",
                "items": )") + DrawingPrimitiveSchema + R"(
            }
        },
        "additionalProperties": false,
        "required": [
            "primitives"
        ]
    })";
}

GS::ObjectState GetStoryControlPrimitivesCommand::Execute (const GS::ObjectState& parameters, GS::ProcessControl& /*processControl*/) const
{
    const GS::ObjectState* databaseId = parameters.Get ("databaseId");
    if (databaseId == nullptr) {
        return CreateErrorResponse (APIERR_BADPARS, "Missing parameter: databaseId.");
    }

    API_DatabaseInfo database = DatabaseIdResolver::Instance ().GetDatabaseWithId (GetGuidFromObjectState (*databaseId));
    GSErrCode err = ACAPI_Window_GetDatabaseInfo (&database);
    if (err != NoError) {
        return CreateErrorResponse (err, "Failed to resolve the database.");
    }
    if (database.typeID != APIWind_SectionID &&
        database.typeID != APIWind_ElevationID &&
        database.typeID != APIWind_InteriorElevationID) {
        return CreateErrorResponse (APIERR_BADDATABASE, "The database must be a section, elevation or interior elevation.");
    }

    DrawingPrimitiveCollectorContext ctx;
    err = GetStoryControlPrimitives (database.databaseUnId, ctx);
    if (err != NoError) {
        return CreateErrorResponse (err, "Failed to get the story control primitives of the database.");
    }

    GS::ObjectState response;
    const auto& primitives = response.AddList<GS::ObjectState> ("primitives");
    for (const GS::ObjectState& primitive : ctx.primitives) {
        primitives (primitive);
    }
    return response;
}
