#include "ElementDetailsReaders.hpp"

#include "MigrationHelper.hpp"

#include <algorithm>
#include <cstring>
#include <utility>
#include <vector>

namespace ElementDetailsReaders {

namespace {

// Number of records a memo handle really holds, whatever the element header claims.
template <typename T>
Int32 GetHandleRecordCount (T** handle)
{
    if (handle == nullptr) {
        return 0;
    }
    return static_cast<Int32> (BMGetHandleSize (reinterpret_cast<GSHandle> (handle)) / sizeof (T));
}

void AddAttributeIdIfValid (GS::ObjectState& os, const char* fieldName, API_AttrTypeID typeID, API_AttributeIndex index)
{
    if (index != APIInvalidAttributeIndex) {
        os.Add (fieldName, CreateGuidObjectState (GetAttributeGuidFromIndex (typeID, index)));
    }
}

GS::ObjectState CreateBoxObjectState (const API_Box& box)
{
    return GS::ObjectState ("xMin", box.xMin, "yMin", box.yMin, "xMax", box.xMax, "yMax", box.yMax);
}

GS::ObjectState CreateBezierDirectionObjectState (double lenPrev, double lenNext, double dirAng)
{
    return GS::ObjectState ("previousLength", lenPrev, "nextLength", lenNext, "angle", dirAng);
}

// A memo polygon (API_Polygon): coords are 1-based and every contour repeats its first vertex at
// its end, pends[i] is the index of the last vertex of contour i (pends[0] is 0), and parcs hold
// 1-based indices into coords. The first contour goes to coordsField/arcsField, every further
// contour to holesField as {polygonOutline, polygonArcs}, with the arc indices rebased to the
// contour (0-based, the same convention as the other polygons of GetDetailsOfElements). The counts
// in `poly` are clamped to what the handles hold, so a header that disagrees with its memo can
// never make this read past a handle.
void AddMemoPolygon (const API_ElementMemo& memo, const API_Polygon& poly, GS::ObjectState& os,
                     const char* coordsField, const char* arcsField, const char* holesField)
{
    const Int32 nCoords = std::min<Int32> (poly.nCoords, GetHandleRecordCount (memo.coords) - 1);
    if (nCoords < 1) {
        return;
    }

    std::vector<std::pair<Int32, Int32>> contours;
    const Int32 nContours = std::min<Int32> (std::max<Int32> (poly.nSubPolys, 1), GetHandleRecordCount (memo.pends) - 1);
    Int32 first = 1;
    for (Int32 i = 1; i <= nContours; ++i) {
        const Int32 last = std::min<Int32> ((*memo.pends)[i], nCoords);
        if (last < first) {
            break;
        }
        contours.emplace_back (first, last);
        first = last + 1;
    }
    if (contours.empty ()) {
        contours.emplace_back (1, nCoords);
    }

    const Int32 nArcs = std::min<Int32> (poly.nArcs, GetHandleRecordCount (memo.parcs));

    const auto addContour = [&] (const std::pair<Int32, Int32>& contour, GS::ObjectState& target, const char* coordsName, const char* arcsName) {
        const auto& coordsList = target.AddList<GS::ObjectState> (coordsName);
        for (Int32 i = contour.first; i <= contour.second; ++i) {
            coordsList (Create2DCoordinateObjectState ((*memo.coords)[i]));
        }
        const auto& arcsList = target.AddList<GS::ObjectState> (arcsName);
        for (Int32 i = 0; i < nArcs; ++i) {
            const API_PolyArc& arc = (*memo.parcs)[i];
            if (arc.begIndex >= contour.first && arc.begIndex <= contour.second && arc.endIndex >= contour.first && arc.endIndex <= contour.second) {
                arcsList (GS::ObjectState ("begIndex", arc.begIndex - contour.first, "endIndex", arc.endIndex - contour.first, "arcAngle", arc.arcAngle));
            }
        }
    };

    addContour (contours[0], os, coordsField, arcsField);
    if (contours.size () > 1) {
        const auto& holesList = os.AddList<GS::ObjectState> (holesField);
        for (size_t i = 1; i < contours.size (); ++i) {
            GS::ObjectState hole;
            addContour (contours[i], hole, "polygonOutline", "polygonArcs");
            holesList (hole);
        }
    }
}

const char* SkylightAnchorToString (API_SkylightAnchorID anchor)
{
    switch (anchor) {
        case APISkylightAnchor_BL: return "BottomLeft";
        case APISkylightAnchor_BR: return "BottomRight";
        case APISkylightAnchor_TC: return "TopCenter";
        case APISkylightAnchor_TL: return "TopLeft";
        case APISkylightAnchor_TR: return "TopRight";
        case APISkylightAnchor_BC:
        default:                   return "BottomCenter";
    }
}

const char* ChangeMarkerLinkTypeToString (API_ChangeMarkerLinkType linkType)
{
    switch (linkType) {
        case APICMLT_Linked:          return "Linked";
        case APICMLT_CreateNewChange: return "CreateNewChange";
        case APICMLT_Unlinked:
        default:                      return "Unlinked";
    }
}

const char* ChangeMarkerComponentToString (API_ChangeMarkerComponentType component)
{
    switch (component) {
        case APICMCT_HeadOnly:  return "HeadOnly";
        case APICMCT_CloudOnly: return "CloudOnly";
        case APICMCT_HeadAndCloud:
        default:                return "HeadAndCloud";
    }
}

const char* PictureFormatToString (API_PictureFormat format)
{
    switch (format) {
        case APIPictForm_Bitmap: return "Bitmap";
        case APIPictForm_GIF:    return "GIF";
        case APIPictForm_TIFF:   return "TIFF";
        case APIPictForm_JPEG:   return "JPEG";
        case APIPictForm_PNG:    return "PNG";
        case APIPictForm_Default:
        default:                 return "Default";
    }
}

}

void AddSkylightDetails (const API_Element& elem, const Stories& stories, GS::ObjectState& typeSpecificDetails)
{
    const API_SkylightType& skylight = elem.skylight;
    const API_OpeningBaseType& base = skylight.openingBase;

    typeSpecificDetails.Add ("width", base.width);
    typeSpecificDetails.Add ("height", base.height);
    typeSpecificDetails.Add ("anchorPoint", SkylightAnchorToString (skylight.anchorPoint));
    typeSpecificDetails.Add ("anchorPosition", Create2DCoordinateObjectState (skylight.anchorPosition));
    // The DevKit calls anchorLevel global, but Archicad 30 stores it from the skylight's home
    // story (a skylight on a roof whose story is at 3 m reads 1.92 for an anchor 4.92 m high).
    typeSpecificDetails.Add ("anchorLevel", skylight.anchorLevel);
    typeSpecificDetails.Add ("zCoordinate", GetZPos (elem.header.floorInd, skylight.anchorLevel, stories));
    typeSpecificDetails.Add ("azimuthAngle", skylight.azimuthAngle);
    typeSpecificDetails.Add ("elevationAngle", skylight.elevationAngle);
    typeSpecificDetails.Add ("fixMode", skylight.fixMode == APISkylightFixMode_Vertical ? "Vertical" : "Horizontal");
    typeSpecificDetails.Add ("pivotPolygonVertexId", skylight.vertexID);
    typeSpecificDetails.Add ("reflected", base.reflected);
    typeSpecificDetails.Add ("oSide", base.oSide);
    typeSpecificDetails.Add ("refSide", base.refSide);
    typeSpecificDetails.Add ("displayOption", ElemDisplayOptionToString (base.displayOption));

    typeSpecificDetails.Add ("pen", (Int32) base.pen);
    AddAttributeIdIfValid (typeSpecificDetails, "lineTypeId", API_LinetypeID, base.ltypeInd);
    AddAttributeIdIfValid (typeSpecificDetails, "surfaceId", API_MaterialID, base.mat);
    AddAttributeIdIfValid (typeSpecificDetails, "sectionFillId", API_FilltypeID, base.sectFill);
    typeSpecificDetails.Add ("sectionFillPen", (Int32) base.sectFillPen);
    typeSpecificDetails.Add ("sectionFillBackgroundPen", (Int32) base.sectBGPen);
    typeSpecificDetails.Add ("sectionContourPen", (Int32) base.sectContPen);
    AddAttributeIdIfValid (typeSpecificDetails, "cutLineTypeId", API_LinetypeID, base.cutLineType);
    typeSpecificDetails.Add ("aboveViewLinePen", (Int32) base.aboveViewLinePen);
    AddAttributeIdIfValid (typeSpecificDetails, "aboveViewLineTypeId", API_LinetypeID, base.aboveViewLineType);
    typeSpecificDetails.Add ("belowViewLinePen", (Int32) base.belowViewLinePen);
    AddAttributeIdIfValid (typeSpecificDetails, "belowViewLineTypeId", API_LinetypeID, base.belowViewLineType);
    typeSpecificDetails.Add ("useObjectPens", base.useObjPens);
    typeSpecificDetails.Add ("useObjectLineTypes", base.useObjLtypes);
    typeSpecificDetails.Add ("useObjectMaterials", base.useObjMaterials);
    typeSpecificDetails.Add ("useObjectSectionAttributes", base.useObjSectAttrs);
}

void AddCameraDetails (const API_Element& elem, GS::ObjectState& typeSpecificDetails)
{
    const API_PerspCamPars& camera = elem.camera.perspCam;
    const API_PerspPars& persp = camera.persp;

    if (elem.camera.camSetGuid != APINULLGuid) {
        typeSpecificDetails.Add ("cameraSetId", CreateGuidObjectState (elem.camera.camSetGuid));
    }
    typeSpecificDetails.Add ("perspective", GS::ObjectState (
        "cameraPosition", Create3DCoordinateObjectState (API_Coord3D { persp.pos.x, persp.pos.y, persp.cameraZ }),
        "targetPosition", Create3DCoordinateObjectState (API_Coord3D { persp.target.x, persp.target.y, persp.targetZ }),
        "distance", persp.distance,
        "azimuth", persp.azimuth,
        "viewCone", persp.viewCone,
        "rollAngle", persp.rollAngle,
        "isTwoPointPersp", persp.isTwoPointPersp));
    typeSpecificDetails.Add ("pen", (Int32) camera.pen);
    typeSpecificDetails.Add ("waitFrames", (Int32) camera.waitFrames);
    typeSpecificDetails.Add ("isActive", camera.active);
    if (camera.prevCam != APINULLGuid) {
        typeSpecificDetails.Add ("previousCameraId", CreateGuidObjectState (camera.prevCam));
    }
    if (camera.nextCam != APINULLGuid) {
        typeSpecificDetails.Add ("nextCameraId", CreateGuidObjectState (camera.nextCam));
    }
    typeSpecificDetails.Add ("pathDirection", CreateBezierDirectionObjectState (camera.lenPrev, camera.lenNext, camera.dirAng));
}

void AddCameraSetDetails (const API_Element& elem, GS::ObjectState& typeSpecificDetails)
{
    const API_CamSetType& cameraSet = elem.camset;

    typeSpecificDetails.Add ("name", GS::UniString (cameraSet.name));
    typeSpecificDetails.Add ("isActive", cameraSet.active);
    if (cameraSet.firstCam != APINULLGuid) {
        typeSpecificDetails.Add ("firstCameraId", CreateGuidObjectState (cameraSet.firstCam));
    }
    if (cameraSet.actCam != APINULLGuid) {
        typeSpecificDetails.Add ("currentCameraId", CreateGuidObjectState (cameraSet.actCam));
    }
    typeSpecificDetails.Add ("pen", (Int32) cameraSet.perspPars.pen);
    typeSpecificDetails.Add ("isOpenPath", cameraSet.perspPars.openedPath);
    typeSpecificDetails.Add ("isBezierPath", cameraSet.perspPars.bezierPath);
    typeSpecificDetails.Add ("smoothTarget", cameraSet.perspPars.smoothTarget);
    typeSpecificDetails.Add ("inBetweenFrames", cameraSet.perspPars.inFrames);
    typeSpecificDetails.Add ("showFrom", cameraSet.perspPars.showFrom);
    typeSpecificDetails.Add ("showTo", cameraSet.perspPars.showTo);
}

void AddChangeMarkerDetails (const API_Element& elem, GS::ObjectState& typeSpecificDetails)
{
    const API_ChangeMarkerType& marker = elem.changeMarker;

    typeSpecificDetails.Add ("linkType", ChangeMarkerLinkTypeToString (marker.linkType));
    if (marker.linkType == APICMLT_Linked) {
        typeSpecificDetails.Add ("changeId", GS::UniString (marker.changeId));
        typeSpecificDetails.Add ("changeName", GS::UniString (marker.changeName));
    }
    typeSpecificDetails.Add ("markerComponent", ChangeMarkerComponentToString (marker.markerComponent));
    // pos only means something without a cloud (it reads 0, 0 on a marker with one).
    const bool hasCloud = marker.poly.nCoords > 0;
    if (!hasCloud) {
        typeSpecificDetails.Add ("position", Create2DCoordinateObjectState (marker.pos));
    }
    if (marker.markerGuid != APINULLGuid) {
        typeSpecificDetails.Add ("markerId", CreateGuidObjectState (marker.markerGuid));
    }
    typeSpecificDetails.Add ("angle", marker.angle);
    typeSpecificDetails.Add ("isHorizontalMarker", marker.horizontalMarker);

    if (!hasCloud) {
        return;
    }
    API_ElementMemo memo = {};
    const GS::OnExit guard ([&memo] () { ACAPI_DisposeElemMemoHdls (&memo); });
    if (ACAPI_Element_GetMemo (elem.header.guid, &memo, APIMemoMask_Polygon) != NoError) {
        return;
    }
    AddMemoPolygon (memo, marker.poly, typeSpecificDetails, "cloudPolygon", "cloudArcs", "cloudHoles");
}

void AddPictureDetails (const API_Element& elem, GS::ObjectState& typeSpecificDetails)
{
    const API_PictureType& picture = elem.picture;

    typeSpecificDetails.Add ("name", GS::UniString (picture.pictName));
    // mime is a fixed size char array the API fills; never read past it.
    const size_t mimeLength = strnlen (picture.mime, sizeof (picture.mime));
    typeSpecificDetails.Add ("mimeType", GS::UniString (std::string (picture.mime, mimeLength).c_str (), CC_UTF8));
    typeSpecificDetails.Add ("storageFormat", PictureFormatToString (picture.storageFormat));
    typeSpecificDetails.Add ("box", CreateBoxObjectState (picture.destBox));
    typeSpecificDetails.Add ("originalBox", CreateBoxObjectState (picture.origBox));
    typeSpecificDetails.Add ("rotationAngle", picture.rotAngle);
    typeSpecificDetails.Add ("mirrored", picture.mirrored);
    typeSpecificDetails.Add ("anchorPoint", AnchorIdToString (picture.anchorPoint));
    typeSpecificDetails.Add ("pixelWidth", (Int32) picture.pixelSizeX);
    typeSpecificDetails.Add ("pixelHeight", (Int32) picture.pixelSizeY);
    typeSpecificDetails.Add ("transparent", picture.transparent);
}

void AddSplineBezierDirections (const API_Element& elem, GS::ObjectState& typeSpecificDetails)
{
    typeSpecificDetails.Add ("autoSmooth", elem.spline.autoSmooth);

    API_ElementMemo memo = {};
    const GS::OnExit guard ([&memo] () { ACAPI_DisposeElemMemoHdls (&memo); });
    if (ACAPI_Element_GetMemo (elem.header.guid, &memo, APIMemoMask_Polygon) != NoError || memo.bezierDirs == nullptr) {
        return;
    }
    // One handle per spline point, index 0 included (unlike the polygon memos).
    Int32 nDirections = GetHandleRecordCount (memo.bezierDirs);
    if (memo.coords != nullptr) {
        nDirections = std::min<Int32> (nDirections, GetHandleRecordCount (memo.coords));
    }
    const auto& directions = typeSpecificDetails.AddList<GS::ObjectState> ("bezierDirections");
    for (Int32 i = 0; i < nDirections; ++i) {
        const API_SplineDir& dir = (*memo.bezierDirs)[i];
        directions (CreateBezierDirectionObjectState (dir.lenPrev, dir.lenNext, dir.dirAng));
    }
}

}
