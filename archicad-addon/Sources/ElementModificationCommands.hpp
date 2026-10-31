#pragma once

#include "CommandBase.hpp"

// Modify commands for element types that could only be changed by deleting and creating them
// again. Each command changes the elements in place with ACAPI_Element_Change and a mask inside
// one undoable command, so every element keeps its GUID and with it its links, properties and
// classifications. The items take the keys of the type's Create command, plus elementId.

// The settings of a linear dimension in the format ModifyDimensions takes them, for GetDimensionData.
GS::ObjectState CreateDimensionSettingsObjectState (const API_Element& element);
// The text settings of one dimension note, the text fields of those settings.
void AddDimensionNoteSettings (GS::ObjectState& os, const API_NoteType& note);

class ModifyElementsCommandBase : public CommandBase
{
public:
    ModifyElementsCommandBase (const GS::String& commandName, const GS::String& arrayFieldName, const GS::UniString& elemTypeName);
    virtual GS::String GetName () const override;
    virtual GS::Optional<GS::UniString> GetRawResponseSchema () const override;
    virtual GS::ObjectState Execute (const GS::ObjectState& parameters, GS::ProcessControl& processControl) const override;

protected:
    // Whether the element is of the type the command modifies.
    virtual bool IsExpectedType (const API_Element& element) const = 0;
    // Whether the type has an Element ID (the ID field of its Settings dialog). Drafting
    // elements have none: Archicad accepts an ID for them but does not keep it.
    virtual bool HasElementId () const { return true; }
    // The name of the story field, the one the Create command takes, or nullptr for a type
    // Archicad does not move to another story with ACAPI_Element_Change (drafting elements and
    // dimensions: tested in Archicad 30, the element stays where it is).
    virtual const char* GetFloorFieldName () const { return nullptr; }
    // Writes the type specific fields of the item into the element and the memo and sets their
    // bits in mask and memoMask. memo is empty on entry. Returns the error text of an invalid
    // item; nothing is changed then.
    virtual GS::Optional<GS::UniString> ApplyTypeSpecificParameters (const GS::ObjectState& item, API_Element& element, API_Element& mask, API_ElementMemo& memo, UInt64& memoMask) const = 0;
    // Called after the element was changed, in the same undoable command, for the parts that are
    // changed as elements of their own (the segments of a railing). Returns an error text.
    virtual GS::Optional<GS::UniString> AfterChange (const GS::ObjectState& /*item*/, const API_Element& /*element*/) const { return {}; }
    // Whether the item has fields that AfterChange writes.
    virtual bool HasAfterChangeFields (const GS::ObjectState& /*item*/) const { return false; }

private:
    GS::String commandName;
    GS::String arrayFieldName;
    GS::UniString elemTypeName;
};

class ModifyLinesCommand : public ModifyElementsCommandBase
{
public:
    ModifyLinesCommand ();
    virtual GS::Optional<GS::UniString> GetInputParametersSchema () const override;

protected:
    virtual bool IsExpectedType (const API_Element& element) const override;
    virtual bool HasElementId () const override { return false; }
    virtual GS::Optional<GS::UniString> ApplyTypeSpecificParameters (const GS::ObjectState& item, API_Element& element, API_Element& mask, API_ElementMemo& memo, UInt64& memoMask) const override;
};

class ModifyArcsCommand : public ModifyElementsCommandBase
{
public:
    ModifyArcsCommand ();
    virtual GS::Optional<GS::UniString> GetInputParametersSchema () const override;

protected:
    virtual bool IsExpectedType (const API_Element& element) const override;
    virtual bool HasElementId () const override { return false; }
    virtual GS::Optional<GS::UniString> ApplyTypeSpecificParameters (const GS::ObjectState& item, API_Element& element, API_Element& mask, API_ElementMemo& memo, UInt64& memoMask) const override;
};

class ModifyCirclesCommand : public ModifyElementsCommandBase
{
public:
    ModifyCirclesCommand ();
    virtual GS::Optional<GS::UniString> GetInputParametersSchema () const override;

protected:
    virtual bool IsExpectedType (const API_Element& element) const override;
    virtual bool HasElementId () const override { return false; }
    virtual GS::Optional<GS::UniString> ApplyTypeSpecificParameters (const GS::ObjectState& item, API_Element& element, API_Element& mask, API_ElementMemo& memo, UInt64& memoMask) const override;
};

class ModifyPolylinesCommand : public ModifyElementsCommandBase
{
public:
    ModifyPolylinesCommand ();
    virtual GS::Optional<GS::UniString> GetInputParametersSchema () const override;

protected:
    virtual bool IsExpectedType (const API_Element& element) const override;
    virtual bool HasElementId () const override { return false; }
    virtual GS::Optional<GS::UniString> ApplyTypeSpecificParameters (const GS::ObjectState& item, API_Element& element, API_Element& mask, API_ElementMemo& memo, UInt64& memoMask) const override;
};

class ModifySplinesCommand : public ModifyElementsCommandBase
{
public:
    ModifySplinesCommand ();
    virtual GS::Optional<GS::UniString> GetInputParametersSchema () const override;

protected:
    virtual bool IsExpectedType (const API_Element& element) const override;
    virtual bool HasElementId () const override { return false; }
    virtual GS::Optional<GS::UniString> ApplyTypeSpecificParameters (const GS::ObjectState& item, API_Element& element, API_Element& mask, API_ElementMemo& memo, UInt64& memoMask) const override;
};

class ModifyDimensionsCommand : public ModifyElementsCommandBase
{
public:
    ModifyDimensionsCommand ();
    virtual GS::Optional<GS::UniString> GetInputParametersSchema () const override;

protected:
    virtual bool IsExpectedType (const API_Element& element) const override;
    virtual bool HasElementId () const override { return false; }
    virtual GS::Optional<GS::UniString> ApplyTypeSpecificParameters (const GS::ObjectState& item, API_Element& element, API_Element& mask, API_ElementMemo& memo, UInt64& memoMask) const override;
};

class ModifyRailingsCommand : public ModifyElementsCommandBase
{
public:
    ModifyRailingsCommand ();
    virtual GS::Optional<GS::UniString> GetInputParametersSchema () const override;

protected:
    virtual bool IsExpectedType (const API_Element& element) const override;
    virtual const char* GetFloorFieldName () const override { return "floorIndex"; }
    virtual GS::Optional<GS::UniString> ApplyTypeSpecificParameters (const GS::ObjectState& item, API_Element& element, API_Element& mask, API_ElementMemo& memo, UInt64& memoMask) const override;
    virtual GS::Optional<GS::UniString> AfterChange (const GS::ObjectState& item, const API_Element& element) const override;
    virtual bool HasAfterChangeFields (const GS::ObjectState& item) const override;
};

class ModifyStairsCommand : public ModifyElementsCommandBase
{
public:
    ModifyStairsCommand ();
    virtual GS::Optional<GS::UniString> GetInputParametersSchema () const override;

protected:
    virtual bool IsExpectedType (const API_Element& element) const override;
    virtual GS::Optional<GS::UniString> ApplyTypeSpecificParameters (const GS::ObjectState& item, API_Element& element, API_Element& mask, API_ElementMemo& memo, UInt64& memoMask) const override;
};
