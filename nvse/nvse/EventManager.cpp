#include <stdarg.h>
#include "EventManager.h"

#include "ArrayVar.h"
#include "GameAPI.h"
#include "Utilities.h"
#include "SafeWrite.h"
#include "GameObjects.h"
#include "ThreadLocal.h"
#include "common/ICriticalSection.h"
#include "GameOSDepend.h"
//#include "InventoryReference.h"

#include "GameData.h"
#include "GameRTTI.h"
#include "GameScript.h"
#include "EventParams.h"

namespace EventManager {

ICriticalSection s_criticalSection;

std::list<EventInfo> s_eventInfos;

EventInfoMap s_eventInfoMap(0x40);

InternalEventVec s_internalEventInfos(kEventID_InternalMAX);

//////////////////////
// Event definitions
/////////////////////

// Hook routines need to be forward declared so they can be used in EventInfo structs.
static void  InstallHook();
static void  InstallActivateHook();
static void	 InstallOnActorEquipHook();

enum {
	kEventMask_OnActivate		= 0x01000000,		// special case as OnActivate has no event mask
};

// hook installers
static EventHookInstaller s_MainEventHook = InstallHook;
static EventHookInstaller s_ActivateHook = InstallActivateHook;
static EventHookInstaller s_ActorEquipHook = InstallOnActorEquipHook;


///////////////////////////
// internal functions
//////////////////////////

void __stdcall HandleGameEvent(UInt32 eventMask, TESObjectREFR* source, TESForm* object);

static const UInt32 kVtbl_PlayerCharacter = 0x0108AA3C;
static const UInt32 kVtbl_Character = 0x01086A6C;
static const UInt32 kVtbl_Creature = 0x010870AC;
static const UInt32 kVtbl_ArrowProjectile = 0x01085954;
static const UInt32 kVtbl_MagicBallProjectile = 0x0107A554;
static const UInt32 kVtbl_MagicBoltProjectile = 0x0107A8F4;
static const UInt32 kVtbl_MagicFogProjectile = 0x0107AD84;
static const UInt32 kVtbl_MagicSprayProjectile = 0x0107B8C4;
static const UInt32 kVtbl_TESObjectREFR = 0x0102F55C;

static const UInt32 kMarkEvent_HookAddr = 0x005AC750;
static const UInt32 kMarkEvent_RetnAddr = 0x005AC756;

static const UInt32 kActivate_HookAddr = 0x0057318E;
static const UInt32 kActivate_RetnAddr = 0x00573194;

static const UInt32 kDestroyCIOS_HookAddr = 0x008232B5;
static const UInt32 kDestroyCIOS_RetnAddr = 0x008232EF;

// cheap check to prevent duplicate events being processed in immediate succession
// (e.g. game marks OnHitWith twice per event, this way we only process it once)
static TESObjectREFR* s_lastObj = nullptr;
static TESForm* s_lastTarget = nullptr;
static UInt32 s_lastEvent = NULL;

// OnHitWith often gets marked twice per event. If weapon enchanted, may see:
//  OnHitWith >> OnMGEFHit >> ... >> OnHitWith. 
// Prevent OnHitWith being reported more than once by recording OnHitWith events processed
static TESObjectREFR* s_lastOnHitWithActor = nullptr;
static TESForm* s_lastOnHitWithWeapon = nullptr;

// And it turns out OnHit is annoyingly marked several times per frame for spells/enchanted weapons
static TESObjectREFR* s_lastOnHitVictim = nullptr;
static TESForm* s_lastOnHitAttacker = nullptr;

//////////////////////////////////
// Hooks
/////////////////////////////////

UInt32 s_eventsInUse = 0;

static __declspec(naked) void MarkEventHook(void)
{
	__asm
	{
		// overwritten code
		push	ebp
		mov		ebp, esp
		sub		esp, 8

		// grab args
		mov		eax, [ebp+0xC]			// ExtraDataList*
		test	eax, eax
		jz		skipHandle

		mov		edx, [ebp+0x10]
		test	s_eventsInUse, edx
		jz		skipHandle

		push	dword ptr [ebp+8]		// target
		sub		eax, 0x44				// TESObjectREFR* thisObj
		push	eax
		push	edx						// event mask
		call	HandleGameEvent

	skipHandle:
		jmp		kMarkEvent_RetnAddr
	}
}		

void InstallHook()
{
	WriteRelJump(kMarkEvent_HookAddr, (UInt32)&MarkEventHook);
}

static __declspec(naked) void DestroyCIOSHook(void)
{
	__asm
	{
		test	ecx, ecx
		jz		doHook
		mov		edx, [ecx]
		ret
	doHook:
		fldz
		jmp		kDestroyCIOS_RetnAddr
	}
}

static void InstallDestroyCIOSHook()
{
	WriteRelCall(kDestroyCIOS_HookAddr, (UInt32)&DestroyCIOSHook);
}

static __declspec(naked) void OnActorEquipHook(void)
{
	__asm
	{
		test	s_eventsInUse, 2
		jz		skipHandle

		push	dword ptr [ebp+8]
		push	dword ptr [ebp+0x10]
		push	2
		call	HandleGameEvent
		mov		ecx, [ebp-0xA8]

	skipHandle:
		mov		eax, 0x4BF0E0
		jmp		eax
	}
}

static void InstallOnActorEquipHook()
{
	static const UInt32 kOnActorEquipHookAddr = 0x004C0032;
	if (s_MainEventHook) {
		// OnActorEquip event also (unreliably) passes through main hook, so install that
		s_MainEventHook();
		// since it's installed, prevent it from being installed again
		s_MainEventHook = nullptr;
	}

	// additional hook to overcome game's failure to consistently mark this event type

	// OBSE: The issue is that our Console_Print routine interacts poorly with the game's debug text (turned on with TDT console command)
	// OBSE: when called from a background thread.
	// OBSE: So if the handler associated with this event calls Print, PrintC, etc, there is a chance it will crash.
	//
	// Fix:
	// Added s_InsideOnActorEquipHook to neutralize Print during OnEquip events, with an optional s_CheckInsideOnActorEquipHook to bypass it for testing.
	WriteRelCall(kOnActorEquipHookAddr, (UInt32)&OnActorEquipHook);
}

static __declspec(naked) void TESObjectREFR_ActivateHook(void)
{
	__asm
	{
		test	s_eventsInUse, kEventMask_OnActivate
		jz		skipHandle

		push	dword ptr [ebp+8]	// activating refr
		push	ecx					// this
		push	kEventMask_OnActivate
		call	HandleGameEvent
		mov		ecx, [ebp-0x12C]

	skipHandle:
		jmp		kActivate_RetnAddr
	}
}

void InstallActivateHook()
{
	WriteRelJump(kActivate_HookAddr, (UInt32)&TESObjectREFR_ActivateHook);
}

eEventID EventIDForMask(UInt32 eventMask)
{
	switch (eventMask) {
		case ScriptEventList::kEvent_OnAdd:
			return kEventID_OnAdd;
		case ScriptEventList::kEvent_OnActorEquip:
			return kEventID_OnActorEquip;
		case ScriptEventList::kEvent_OnDrop:
			return kEventID_OnDrop;
		case ScriptEventList::kEvent_OnActorUnequip:
			return kEventID_OnActorUnequip;
		case ScriptEventList::kEvent_OnDeath:
			return kEventID_OnDeath;
		case ScriptEventList::kEvent_OnMurder:
			return kEventID_OnMurder;
		case ScriptEventList::kEvent_OnCombatEnd:
			return kEventID_OnCombatEnd;
		case ScriptEventList::kEvent_OnHit:
			return kEventID_OnHit;
		case ScriptEventList::kEvent_OnHitWith:
			return kEventID_OnHitWith;
		case ScriptEventList::kEvent_OnPackageStart:
			return kEventID_OnPackageStart;
		case ScriptEventList::kEvent_OnPackageDone:
			return kEventID_OnPackageDone;
		case ScriptEventList::kEvent_OnPackageChange:
			return kEventID_OnPackageChange;
		case ScriptEventList::kEvent_OnLoad:
			return kEventID_OnLoad;
		case ScriptEventList::kEvent_OnMagicEffectHit:
			return kEventID_OnMagicEffectHit;
		case ScriptEventList::kEvent_OnSell:
			return kEventID_OnSell;
		case ScriptEventList::kEvent_OnStartCombat:
			return kEventID_OnStartCombat;
		case ScriptEventList::kEvent_SayToDone:
			return kEventID_SayToDone;
		case ScriptEventList::kEvent_OnFire:
			return kEventID_OnFire;
		case ScriptEventList::kEvent_OnOpen:
			return kEventID_OnOpen;
		case ScriptEventList::kEvent_OnClose:
			return kEventID_OnClose;
		case ScriptEventList::kEvent_OnGrab:
			return kEventID_OnGrab;
		case ScriptEventList::kEvent_OnTrigger:
			return kEventID_OnTrigger;
		case ScriptEventList::kEvent_OnTriggerEnter:
			return kEventID_OnTriggerEnter;
		case ScriptEventList::kEvent_OnTriggerLeave:
			return kEventID_OnTriggerLeave;
		case ScriptEventList::kEvent_OnReset:
			return kEventID_OnReset;
		case kEventMask_OnActivate:
			return kEventID_OnActivate;
		default:
			return kEventID_INVALID;
	}
}

eEventID EventIDForMessage(UInt32 msgID)
{
	switch (msgID) {
		case NVSEMessagingInterface::kMessage_ExitGame:
			return kEventID_ExitGame;
		case NVSEMessagingInterface::kMessage_ExitToMainMenu:
			return kEventID_ExitToMainMenu;
		case NVSEMessagingInterface::kMessage_LoadGame:
			return kEventID_LoadGame;
		case NVSEMessagingInterface::kMessage_SaveGame:
			return kEventID_SaveGame;
		case NVSEMessagingInterface::kMessage_ExitGame_Console:
			return kEventID_QQQ;
		case NVSEMessagingInterface::kMessage_PostLoadGame:
			return kEventID_PostLoadGame;
		case NVSEMessagingInterface::kMessage_RuntimeScriptError:
			return kEventID_RuntimeScriptError;
		case NVSEMessagingInterface::kMessage_DeleteGame:
			return kEventID_DeleteGame;
		case NVSEMessagingInterface::kMessage_RenameGame:
			return kEventID_RenameGame;
		case NVSEMessagingInterface::kMessage_RenameNewGame:
			return kEventID_RenameNewGame;
		case NVSEMessagingInterface::kMessage_NewGame:
			return kEventID_NewGame;
		case NVSEMessagingInterface::kMessage_DeleteGameName:
			return kEventID_DeleteGameName;
		case NVSEMessagingInterface::kMessage_RenameGameName:
			return kEventID_RenameGameName;
		case NVSEMessagingInterface::kMessage_RenameNewGameName:
			return kEventID_RenameNewGameName;
		default:
			return kEventID_INVALID;
	}
}

// Prevent filters of the wrong type from being added to an Event Handler instance.
// Only needs to be called for SetEventHandlerAlt, to filter out most user weirdness.
bool IsPotentialFilterValid(EventFilterType const expectedParamType, std::string& outErrorMsg,
	const EventCallback::Filter &potentialFilter, size_t const filterNum)
{
	if (expectedParamType == EventFilterType::eParamType_Anything)
		return true;

	auto const dataType = potentialFilter.DataType();
	if (dataType == kDataType_Array)
	{
		// Could be an array of filters, or just a filter array.
		ArrayID arrID;
		potentialFilter.GetAsArray(&arrID);
		auto const arrPtr = g_ArrayMap.Get(arrID);
		if (!arrPtr) [[unlikely]]
		{
			outErrorMsg = FormatString("Invalid/uninitialized array filter was passed for filter #%u (array id %u).", filterNum, arrID);
			return false;
		}
		// If array of filters, assume that every element in the array is of the expected type.
		// If it's a (String)Map, then its keys will simply be ignored.
		return true;
	}

	auto const expectedDataType = ParamTypeToDataType(expectedParamType);
	if (dataType != expectedDataType) [[unlikely]]
	{
		outErrorMsg = FormatString("Invalid type for filter #%u: expected %s, got %s.", filterNum, 
			DataTypeToString(expectedDataType), DataTypeToString(dataType));
		return false;
	}

	if (dataType == kDataType_Form)
	{
		TESForm* form;
		// Allow null forms to go through, in case that would be a desired filter.
		if (UInt32 refID; potentialFilter.GetAsFormID(&refID) && (form = LookupFormByID(refID)))
		{
			if (expectedParamType == EventFilterType::eParamType_BaseForm
				&& form->GetIsReference()) [[unlikely]]
			{
				//Prefer not to sneakily convert the user's reference to its baseform, lessons must be learned.
				outErrorMsg = FormatString("Expected BaseForm-type filter for filter #%u, got Reference.", filterNum);
				return false;
			}
			// Allow passing a baseform to a Reference-type filter.
			// When the event is dispatched, it will check if the passed reference belongs to the baseform.
			// (The above assumes the baseform is not a formlist. If it is, then it'll repeat the above check for each form in the list until a match is found).
			// (We are not checking the validity of each form in a formlist filter for performance concerns).
		}
	}

	return true;
}

bool EventCallback::ValidateFirstOrSecondFilter(bool isFirst, const EventInfo& parent, std::string &outErrorMsg) const
{
	const TESForm* filter = isFirst ? source : object;
	if (!filter) // unfiltered
		return true;
	auto const filterName = isFirst ? R"("first"/"source")" : R"("second"/"object")";

	if (isFirst )
	{
		if (!parent.numParams) [[unlikely]]
		{
			outErrorMsg = R"(Cannot use "first"/"source" filter; event has 0 args.)";
			return false;
		}
	}
	else if (parent.numParams < 2) [[unlikely]]
	{
		outErrorMsg = FormatString(R"(Cannot use "second"/"object" filter; event has %u args.)", parent.numParams);
		return false;
	}
	auto const expectedType = parent.paramTypes[isFirst ? 0 : 1];

	if (!IsParamForm(expectedType)) [[unlikely]]
	{
		outErrorMsg = FormatString("Cannot set a non-Form type filter for %s.", filterName);
		return false;
	}

	if (expectedType == EventFilterType::eParamType_BaseForm 
		&& filter->GetIsReference()) [[unlikely]]
	{
		//Prefer not to sneakily convert the user's reference to its baseform, lessons must be learned.
		outErrorMsg = FormatString("Expected BaseForm-type filter for filter %s, got Reference.", filterName);
		return false;
	}

	return true;
}

bool EventCallback::ValidateFirstAndSecondFilter(const EventInfo& parent, std::string& outErrorMsg) const
{
	return ValidateFirstOrSecondFilter(true, parent, outErrorMsg)
		&& ValidateFirstOrSecondFilter(false, parent, outErrorMsg);
}

bool EventCallback::ValidateFilters(std::string& outErrorMsg, const EventInfo& parent)
{
	if (parent.IsUserDefined())
		return true;	// User-Defined events have no preset filters.

	if (!ValidateFirstAndSecondFilter(parent, outErrorMsg))
		return false;

	for (auto &[index, filter] : filters)
	{
		if (index > parent.numParams) [[unlikely]]
		{
			outErrorMsg = FormatString("Index %d exceeds max number of args for event (%u)", index, parent.numParams);
			return false;
		}

		// Index #0 is reserved for callingReference filter.
		bool const isCallingRefFilter = index == 0;
		auto const filterType = isCallingRefFilter ? EventFilterType::eParamType_Reference
			: parent.TryGetNthParamType(index - 1);

		if (!IsPotentialFilterValid(filterType, outErrorMsg, filter, index)) [[unlikely]]
			return false;

		if (filterType == EventFilterType::eParamType_Int)
		{
			filter.m_data.num = floor(filter.m_data.num);
		}
	}

	return true;
}

std::string EventCallback::GetFiltersAsStr() const
{
	std::string result;

	if (source)
	{
		result += std::string{ R"("first"::)" } + source->GetStringRepresentation();
	}

	if (object)
	{
		if (!result.empty())
			result += ", ";
		result += R"("second"::)" + object->GetStringRepresentation();
	}

	for (auto const &[index, filter] : filters)
	{
		if (!result.empty())
			result += ", ";
		result += std::to_string(index) + "::" + filter.GetStringRepresentation();
	}

	if (result.empty())
	{
		result = "(None)";
	}

	return result;
}

ArrayVar* EventCallback::GetFiltersAsArray(const Script* scriptObj) const
{
	ArrayVar* arr = g_ArrayMap.Create(kDataType_String, false, scriptObj->GetModIndex());

	if (source)
	{
		arr->SetElementFormID("first", source->refID);
	}
	if (object)
	{
		arr->SetElementFormID("second", object->refID);
	}
	for (auto const& [index, filter] : filters)
	{
		arr->SetElement(std::to_string(index).c_str(), &filter);
	}

	return arr;
}


std::string EventCallback::GetCallbackFuncAsStr() const
{
	return std::visit(overloaded
		{
			[](const LambdaManager::Maybe_Lambda& script) -> std::string
			{
				return script.Get()->GetStringRepresentation();
			},
			[](const EventHandler& handler) -> std::string
			{
				return FormatString("[addr: %X]", handler);
			}
		}, this->toCall);
}

bool EventCallback::EqualFilters(const EventCallback& rhs) const
{
	return (object == rhs.object) && (source == rhs.source) && (filters == rhs.filters);
}

Script* EventCallback::TryGetScript() const
{
	if (auto const maybe_lambda = std::get_if<LambdaManager::Maybe_Lambda>(&toCall))
		return maybe_lambda->Get();
	return nullptr;
}

bool EventCallback::HasCallbackFunc() const
{
	return std::visit([](auto const& vis) -> bool { return vis; }, toCall);
}

void EventCallback::TrySaveLambdaContext()
{
	if (auto const maybe_lambda = std::get_if<LambdaManager::Maybe_Lambda>(&toCall))
		maybe_lambda->TrySaveContext();
}

EventCallback::EventCallback(EventCallback&& other) noexcept: toCall(std::move(other.toCall)),
                                                              source(other.source),
                                                              object(other.object),
                                                              removed(other.removed),
                                                              pendingRemove(other.pendingRemove),
                                                              filters(std::move(other.filters))
{}

EventCallback& EventCallback::operator=(EventCallback&& other) noexcept
{
	if (this == &other)
		return *this;
	toCall = std::move(other.toCall);
	source = other.source;
	object = other.object;
	removed = other.removed;
	pendingRemove = other.pendingRemove;
	filters = std::move(other.filters);
	return *this;
}

void EventCallback::Confirm()
{
	TrySaveLambdaContext();
}

class ClassicEventHandlerCaller : public InternalFunctionCaller
{
public:
	ClassicEventHandlerCaller(Script* script, EventInfo* eventInfo, void* arg0, void* arg1) : InternalFunctionCaller(script, nullptr), m_eventInfo(eventInfo)
	{
		UInt8 numArgs = 2;
		if (!arg1)
			numArgs = 1;
		if (!arg0)
			numArgs = 0;

		SetArgs(numArgs, arg0, arg1);
	}

	bool ValidateParam(UserFunctionParam* param, UInt8 paramIndex) override
	{
		const auto paramType = VarTypeToParamType(param->varType);
		return ParamTypeMatches(paramType, m_eventInfo->paramTypes[paramIndex]);
	}

	bool PopulateArgs(ScriptEventList* eventList, FunctionInfo* info) override {
		// make sure we've got the same # of args as expected by event handler
		const DynamicParamInfo& dParams = info->ParamInfo();
		if (dParams.NumParams() != m_eventInfo->numParams || dParams.NumParams() > 2) {
			ShowRuntimeError(m_script, "Number of arguments to function script does not match those expected for event");
			return false;
		}

		return InternalFunctionCaller::PopulateArgs(eventList, info);
	}

private:
	EventInfo* m_eventInfo;
};


std::unique_ptr<ScriptToken> EventCallback::Invoke(EventInfo &eventInfo, void* arg0, void* arg1)
{
	return std::visit(overloaded
		{
			[=, &eventInfo](const LambdaManager::Maybe_Lambda& script)
			{
				ScopedLock lock(s_criticalSection);	//for event stack

				// handle immediately
				s_eventStack.Push(eventInfo.evName);
				auto ret = UserFunctionManager::Call(ClassicEventHandlerCaller(script.Get(), &eventInfo, arg0, arg1));
				s_eventStack.Pop();
				return ret;
			},
			[=](const EventHandler& handler) -> std::unique_ptr<ScriptToken>
			{
				// native plugin event handlers
				void* params[] = { arg0, arg1 };
				handler(nullptr, params);
				return nullptr;
			}
		}, this->toCall);
}

BasicCallbackFunc GetBasicCallback(const EventCallback::CallbackFunc& func)
{
	return std::visit(overloaded
		{
			[](const LambdaManager::Maybe_Lambda& script) -> BasicCallbackFunc
			{
				return script.Get();
			},
			[](const EventHandler& handler) -> BasicCallbackFunc
			{
				return handler;
			}
		}, func);
}

bool IsValidReference(void* refr)
{
	bool bIsRefr = false;
	__try
	{
		if ((*static_cast<UInt8*>(refr) & 4) && ((static_cast<UInt16*>(refr)[1] == 0x108) || (*static_cast<UInt32*>(refr) == 0x102F55C)))
			bIsRefr = true;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		bIsRefr = false;
	}

	return bIsRefr;
}

// stack of event names pushed when handler invoked, popped when handler returns
// used by GetCurrentEventName
Stack<const char*> s_eventStack;

// Some events are best deferred until Tick() invoked rather than being handled immediately.
// This stores info about such an event.
// Only used in the deprecated HandleEvent.
struct DeferredDeprecatedCallback
{
	EventCallback		&callback;	//assume this contains a Script* CallbackFunc.
	void				*arg0;
	void				*arg1;
	EventInfo			&eventInfo;

	DeferredDeprecatedCallback(EventCallback &pCallback, void *_arg0, void *_arg1, EventInfo &_eventInfo) :
		callback(pCallback), arg0(_arg0), arg1(_arg1), eventInfo(_eventInfo) {}

	~DeferredDeprecatedCallback()
	{
		if (callback.removed)
			return;

		// assume callback is owned by a global; prevent data race.
		ScopedLock lock(s_criticalSection);

		callback.Invoke(eventInfo, arg0, arg1);
	}
};

typedef Stack<DeferredDeprecatedCallback> DeferredCallbackList;
DeferredCallbackList s_deferredDeprecatedCallbacks;

DeferredRemoveList s_deferredRemoveList;

enum class RefState {NotSet, Invalid, Valid};

// Deprecated
void HandleEvent(EventInfo& eventInfo, void* arg0, void* arg1)
{
	auto isArg0Valid = RefState::NotSet;
	for (auto& iter : eventInfo.callbacks)
	{
		EventCallback& callback = iter.second;

		if (callback.IsRemoved())
			continue;

		// Check filters
		if (callback.source && arg0 != callback.source)
		{
			if (isArg0Valid == RefState::NotSet)
				isArg0Valid = IsValidReference(arg0) ? RefState::Valid : RefState::Invalid;
			if (isArg0Valid == RefState::Invalid || GetPermanentBaseForm(static_cast<TESObjectREFR*>(arg0)) != callback.source)
				continue;
		}

		if (callback.object && (callback.object != arg1))
			continue;

		if (GetCurrentThreadId() != g_mainThreadID)
		{
			// avoid potential issues with invoking handlers outside of main thread by deferring event handling
			s_deferredDeprecatedCallbacks.Push(callback, arg0, arg1, eventInfo);
		}
		else
		{
			callback.Invoke(eventInfo, arg0, arg1);
		}
	}
}

// Deprecated in favor of EventManager::DispatchEvent
void __stdcall HandleEvent(eEventID id, void* arg0, void* arg1)
{
	ScopedLock lock(s_criticalSection);
	EventInfo* eventInfo = s_internalEventInfos[id]; //assume ID is valid
	if (eventInfo->callbacks.empty()) 
		return;
	HandleEvent(*eventInfo, arg0, arg1);
}

////////////////
// public API
///////////////

const char* GetCurrentEventName()
{
	ScopedLock lock(s_criticalSection);

	return s_eventStack.Empty() ? "" : s_eventStack.Top();
}

static UInt32 recursiveLevel = 0;

// Avoid constantly looking up the eventName for potentially recursive calls.
bool DoSetHandler(EventInfo &info, EventCallback& toSet)
{
	if (auto const script = toSet.TryGetScript())
	{
		// Use recursion over formlists to Set the handler for each form.
		// Only kept for legacy purposes.
		UInt32 setted = 0;

		// trying to use a FormList to specify the source filter
		if (toSet.source && toSet.source->GetTypeID() == 0x055 && recursiveLevel < 100)
		{
			const auto formList = static_cast<BGSListForm*>(toSet.source);
			for (tList<TESForm>::Iterator iter = formList->list.Begin(); !iter.End(); ++iter)
			{
				EventCallback listHandler(script, iter.Get(), toSet.object);
				recursiveLevel++;
				if (DoSetHandler(info, listHandler)) setted++;
				recursiveLevel--;
			}
			return setted > 0;
		}

		// trying to use a FormList to specify the object filter
		if (toSet.object && toSet.object->GetTypeID() == 0x055 && recursiveLevel < 100)
		{
			const auto formList = static_cast<BGSListForm*>(toSet.object);
			for (tList<TESForm>::Iterator iter = formList->list.Begin(); !iter.End(); ++iter)
			{
				EventCallback listHandler(script, toSet.source, iter.Get());
				recursiveLevel++;
				if (DoSetHandler(info, listHandler)) setted++;
				recursiveLevel--;
			}
			return setted > 0;
		}
	}

	ScopedLock lock(s_criticalSection);

	// is hook installed for this event type?
	if (info.installHook)
	{
		if (*info.installHook)
		{
			// if this hook is used by more than one event type, prevent it being installed a second time
			(*info.installHook)();
		}
		// mark event as having had its hook installed
		info.installHook = nullptr;
	}

	auto const basicCallback = GetBasicCallback(toSet.toCall);

	if (!info.callbacks.empty())
	{
		// if an existing handler matches this one exactly, don't duplicate it
		auto const range = info.callbacks.equal_range(basicCallback);
		// loop over all EventCallbacks with the same callback script/function.
		for (auto i = range.first; i != range.second; ++i)
		{
			auto& existingCallback = i->second;
			if (existingCallback.EqualFilters(toSet))
			{
				// may be re-adding a previously removed handler, so clear the Removed flag
				if (existingCallback.IsRemoved())
				{
					existingCallback.SetRemoved(false);
					return true; 
				}
				return false; //attempting to add a duplicate handler.
			}
		}
	}

	toSet.Confirm();
	info.callbacks.emplace(basicCallback, std::move(toSet));

	s_eventsInUse |= info.eventMask;
	return true;
}

DeferredRemoveCallback::~DeferredRemoveCallback()
{
	if (iterator->second.removed)
	{
		eventInfo->callbacks.erase(iterator);
		if (eventInfo->callbacks.empty() && eventInfo->eventMask)
			s_eventsInUse &= ~eventInfo->eventMask;
	}
	else iterator->second.pendingRemove = false;
}

bool SetHandler(const char* eventName, EventCallback& toSet, ExpressionEvaluator* eval)
{
	if (!toSet.HasCallbackFunc())
		return false;

	EventInfo** eventInfoPtr = nullptr;
	{
		ScopedLock lock(s_criticalSection);
		if (s_eventInfoMap.Insert(eventName, &eventInfoPtr))
		{
			// have to assume registering for a user-defined event (for DispatchEvent) which has not been used before this point
			char* nameCopy = CopyString(eventName);
			StrToLower(nameCopy);
			*eventInfoPtr = &s_eventInfos.emplace_back(nameCopy, nullptr, 0, EventFlags::kFlag_IsUserDefined);
		}
	}
	if (!eventInfoPtr)
		return false;
	//else, assume ptr is valid
	EventInfo &info = **eventInfoPtr;

	{ // nameless scope
		std::string errMsg;
		if (!toSet.ValidateFilters(errMsg, info))
		{
			if (eval)
				eval->Error(errMsg.c_str());
			return false;
		}
	}

	return DoSetHandler(info, toSet);
}

// If the passed Callback is more or equally generic filter-wise than some already-set events, will remove those events.
// Ex: Callback with "SunnyREF" filter is already set.
// Calling this with a Callback with no filters will lead to the "SunnyREF"-filtered callback being removed.	
bool RemoveHandler(const char* eventName, const EventCallback& toRemove)
{
	if (!toRemove.HasCallbackFunc())
		return false;

	// Only handles script callbacks, since this is only kept for legacy purposes anyways.
	if (auto const script = toRemove.TryGetScript())
	{
		UInt32 removed = 0;

		// trying to use a FormList to specify the source filter
		if (toRemove.source && toRemove.source->GetTypeID() == 0x055 && recursiveLevel < 100)
		{
			const auto formList = static_cast<BGSListForm*>(toRemove.source);
			for (tList<TESForm>::Iterator iter = formList->list.Begin(); !iter.End(); ++iter)
			{
				EventCallback listHandler(script, iter.Get(), toRemove.object);
				recursiveLevel++;
				if (RemoveHandler(eventName, listHandler)) removed++;
				recursiveLevel--;
			}
			return removed > 0;
		}

		// trying to use a FormList to specify the object filter
		if (toRemove.object && toRemove.object->GetTypeID() == 0x055 && recursiveLevel < 100)
		{
			const auto formList = static_cast<BGSListForm*>(toRemove.object);
			for (tList<TESForm>::Iterator iter = formList->list.Begin(); !iter.End(); ++iter)
			{
				EventCallback listHandler(script, toRemove.source, iter.Get());
				recursiveLevel++;
				if (RemoveHandler(eventName, listHandler)) removed++;
				recursiveLevel--;
			}
			return removed > 0;
		}
	}

	ScopedLock lock(s_criticalSection);

	EventInfo** info = s_eventInfoMap.GetPtr(eventName);
	bool bRemovedAtLeastOne = false;
	if (info)
	{
		EventInfo &eventInfo = **info;
		if (!eventInfo.callbacks.empty())
		{
			auto const basicCallback = GetBasicCallback(toRemove.toCall);
			auto const range = eventInfo.callbacks.equal_range(basicCallback);
			// loop over all EventCallbacks with the same callback script/function.
			for (auto i = range.first; i != range.second; ++i)
			{
				EventCallback &nthCallback = i->second;

				if (!toRemove.ShouldRemoveCallback(nthCallback, eventInfo))
					continue;

				if (!nthCallback.IsRemoved())
				{
					nthCallback.SetRemoved(true);
					bRemovedAtLeastOne = true;
				}

				if (!nthCallback.pendingRemove)
				{
					nthCallback.pendingRemove = true;
					s_deferredRemoveList.Push(&eventInfo, i);
				}
			}
		}
	}
	
	return bRemovedAtLeastOne;
}

void __stdcall HandleGameEvent(UInt32 eventMask, TESObjectREFR* source, TESForm* object)
{
	if (!IsValidReference(source)) {
		return;
	}

	ScopedLock lock(s_criticalSection);

	// ScriptEventList can be marked more than once per event, cheap check to prevent sending duplicate events to scripts
	if (source != s_lastObj || object != s_lastTarget || eventMask != s_lastEvent)
	{
		s_lastObj = source;
		s_lastEvent = eventMask;
		s_lastTarget = object;
	}
	else {
		// duplicate event, ignore it
		return;
	}

	const eEventID eventID = EventIDForMask(eventMask);
	if (eventID != kEventID_INVALID)
	{
		if (eventID == kEventID_OnHitWith)
		{
			// special check for OnHitWith, since it gets called redundantly
			if (source != s_lastOnHitWithActor || object != s_lastOnHitWithWeapon)
			{
				s_lastOnHitWithActor = source;

				s_lastOnHitWithWeapon = object;
				HandleEvent(eventID, source, object);
			}
		}
		else if (eventID == kEventID_OnHit)
		{
			if (source != s_lastOnHitVictim || object != s_lastOnHitAttacker)
			{
				s_lastOnHitVictim = source;
				s_lastOnHitAttacker = object;
				HandleEvent(eventID, source, object);
			}
		}
		else
			HandleEvent(eventID, source, object);
	}
}

void HandleNVSEMessage(UInt32 msgID, void* data)
{
	const eEventID eventID = EventIDForMessage(msgID);
	if (eventID != kEventID_INVALID)
		HandleEvent(eventID, data, nullptr);
}

void ClearFlushOnLoadEvents()
{
	s_deferredRemoveList.Clear();

	for (auto& info : s_eventInfos)
	{
		if (info.FlushesOnLoad())
		{
			info.callbacks.clear(); //warning: may invalidate iterators in DeferredRemoveCallbacks.
			// Thus, ensure that list is cleared before this code is reached.
			if (info.eventMask)
			{
				s_eventsInUse &= ~info.eventMask;
			}
		}
		else
		{
			// Remove individual callbacks.
			for (auto iter = info.callbacks.begin(); iter != info.callbacks.end(); )
			{
				auto& callback = iter->second;
				if (callback.FlushesOnLoad())
				{
					iter = info.callbacks.erase(iter);
				}
				else
					++iter;
			}

			if (info.callbacks.empty() && info.eventMask)
				s_eventsInUse &= ~info.eventMask;
		}
	}
}

bool DoesFormMatchFilter(TESForm* inputForm, TESForm* filter, bool expectReference, const UInt32 recursionLevel)
{
	if (filter == inputForm)	//filter and form could both be null, and that's okay.
		return true;
	if (!filter || !inputForm)
		return false;
	if (recursionLevel > 100) [[unlikely]]
		return false;
	if (IS_ID(filter, BGSListForm))
	{
		const auto* list = static_cast<BGSListForm*>(filter);
		for (auto* listForm : list->list)
		{
			if (DoesFormMatchFilter(inputForm, listForm, expectReference, recursionLevel + 1))
				return true;
		}
	}
	// If input form is a reference, then try matching its baseForm to the filter.
	else if (expectReference && inputForm->GetIsReference())
	{
		if (filter == GetPermanentBaseForm(static_cast<TESObjectREFR*>(inputForm)))
			return true;
	}
	return false;
}

bool DoDeprecatedFiltersMatch(const EventCallback& callback, const ArgStack& params)
{
	// old filter system
	if (callback.source && !DoesFormMatchFilter(static_cast<TESForm*>(params->at(0)), callback.source, false))
	{
		return false;
	}
	if (callback.object && !DoesFormMatchFilter(static_cast<TESForm*>(params->at(1)), callback.object, false))
	{
		return false;
	}
	return true;
}

EventInfo* TryGetEventInfoForName(const char* eventName)
{
	if (EventInfo** infoPtr = s_eventInfoMap.GetPtr(eventName))
		return *infoPtr;
	return nullptr;
}

// Meant for use to validate param types, not much else.
Script::VariableType ParamTypeToVarType(EventFilterType pType)
{
	switch (pType)
	{
	case EventFilterType::eParamType_Int: // Int filter type is only different from Float when Dispatching (number is floored).
	case EventFilterType::eParamType_Float: return Script::VariableType::eVarType_Float;
	case EventFilterType::eParamType_String: return Script::VariableType::eVarType_String;
	case EventFilterType::eParamType_Array: return Script::VariableType::eVarType_Array;
	case EventFilterType::eParamType_RefVar:
	case EventFilterType::eParamType_Reference:
	case EventFilterType::eParamType_BaseForm:
		return Script::VariableType::eVarType_Ref;
	case NVSEEventManagerInterface::eParamType_Invalid:
	case NVSEEventManagerInterface::eParamType_Anything:
		return Script::VariableType::eVarType_Invalid;
	}
	return Script::VariableType::eVarType_Invalid;
}

EventFilterType VarTypeToParamType(Script::VariableType varType)
{
	switch (varType) {
	case Script::eVarType_Float: return EventFilterType::eParamType_Float;
	case Script::eVarType_Integer:	return EventFilterType::eParamType_Int;
	case Script::eVarType_String: return EventFilterType::eParamType_String;
	case Script::eVarType_Array: return EventFilterType::eParamType_Array;
	case Script::eVarType_Ref: return EventFilterType::eParamType_AnyForm;
	case Script::eVarType_Invalid:
		return EventFilterType::eParamType_Invalid;
	}
	return EventFilterType::eParamType_Invalid;
}

DataType ParamTypeToDataType(EventFilterType pType)
{
	switch (pType)
	{
	case EventFilterType::eParamType_Int:
	case EventFilterType::eParamType_Float: return kDataType_Numeric;
	case EventFilterType::eParamType_String: return kDataType_String;
	case EventFilterType::eParamType_Array: return kDataType_Array;
	case EventFilterType::eParamType_RefVar:
	case EventFilterType::eParamType_Reference:
	case EventFilterType::eParamType_BaseForm:
		return kDataType_Form;
	case NVSEEventManagerInterface::eParamType_Invalid:
	case NVSEEventManagerInterface::eParamType_Anything:
		return kDataType_Invalid;
	}
	return kDataType_Invalid;
}

bool ParamTypeMatches(EventFilterType from, EventFilterType to)
{
	if (from == to)
		return true;
	if (from == EventFilterType::eParamType_AnyForm)
	{
		switch (to) {
		case NVSEEventManagerInterface::eParamType_AnyForm:
		case NVSEEventManagerInterface::eParamType_Reference:
		case NVSEEventManagerInterface::eParamType_BaseForm: return true;
		default: break;
		}
	}
	if (to == NVSEEventManagerInterface::eParamType_Anything)
		return true;
	return false;
}

bool EventCallback::ShouldRemoveCallback(const EventCallback& toCheck, const EventInfo& evInfo) const
{
	if (source && (source != toCheck.source))
		return false;

	if (object && (object != toCheck.object))
		return false;

	if (filters.empty())
		return true;

	if (filters.size() > toCheck.filters.size())
		return false; // "this" is less generic than the arg callback.

	for (auto const& [index, toRemoveFilter] : filters)
	{
		if (auto const search = toCheck.filters.find(index);
			search != toCheck.filters.end())
		{
			auto const& existingFilter = search->second;

			EventFilterType paramType;
			if (index == 0)
			{
				paramType = EventFilterType::eParamType_Reference;
			}
			else
			{
				paramType = evInfo.TryGetNthParamType(index - 1);
			}
			
			if (void* param = toRemoveFilter.GetAsVoidArg(); 
				toRemoveFilter.DataType() == existingFilter.DataType())
			{
				if (!DoesFilterMatch<true>(existingFilter, param, paramType))
					return false;
			}
			else if (toRemoveFilter.DataType() == kDataType_Array)
			{
				// assume elements of array are filters
				// If the paramType is numeric (int or float), make sure our param void* is always read as a float.
				// (Since ArrayElements don't contain int-type values, they just store floats, floored or not.)
				if (!DoesParamMatchFiltersInArray<true>(*this, existingFilter, paramType, param, index))
					return false;
			}
			else [[unlikely]]
			{
				return false;	// Encountered weird type mismatch.
			}
		}
		else // toCheck does not have a filter at this index, thus "this" has a more specific filter.
			return false;
	}
	return true;
}


DispatchReturn vDispatchEvent(const char* eventName, DispatchCallback resultCallback, void* anyData,
	TESObjectREFR* thisObj, va_list args, bool deferIfOutsideMainThread, PostDispatchCallback postCallback)
{
	const auto eventPtr = TryGetEventInfoForName(eventName);
	if (!eventPtr)
		return DispatchReturn::kRetn_UnknownEvent;
	EventInfo& eventInfo = *eventPtr;

#if _DEBUG //shouldn't need to be checked normally; RegisterEvent verifies numParams.
	if (eventInfo.numParams > numMaxFilters)
		return DispatchReturn::kRetn_GenericError;
#endif

	ArgStack params;
	for (int i = 0; i < eventInfo.numParams; ++i)
		params->push_back(va_arg(args, void*));

	return DispatchEventRaw<false>(thisObj, eventInfo, params, nullptr, nullptr, 
		deferIfOutsideMainThread, postCallback);
}


bool DispatchEvent(const char* eventName, TESObjectREFR* thisObj, ...)
{
	va_list paramList;
	va_start(paramList, thisObj);

	DispatchReturn const result = vDispatchEvent(eventName, nullptr, nullptr,
		thisObj, paramList, false, nullptr);

	va_end(paramList);
	return result > DispatchReturn::kRetn_GenericError;
}
bool DispatchEventThreadSafe(const char* eventName, PostDispatchCallback postCallback, TESObjectREFR* thisObj, ...)
{
	va_list paramList;
	va_start(paramList, thisObj);

	DispatchReturn const result = vDispatchEvent(eventName, nullptr, nullptr,
		thisObj, paramList, true, postCallback);

	va_end(paramList);
	return result > DispatchReturn::kRetn_GenericError;
}

DispatchReturn DispatchEventAlt(const char* eventName, DispatchCallback resultCallback, void* anyData, TESObjectREFR* thisObj, ...)
{
	va_list paramList;
	va_start(paramList, thisObj);

	auto const result = vDispatchEvent(eventName, resultCallback, anyData,
		thisObj, paramList, false, nullptr);

	va_end(paramList);
	return result;
}
DispatchReturn DispatchEventAltThreadSafe(const char* eventName, DispatchCallback resultCallback, void* anyData, 
	PostDispatchCallback postCallback, TESObjectREFR* thisObj, ...)
{
	va_list paramList;
	va_start(paramList, thisObj);

	auto const result = vDispatchEvent(eventName, resultCallback, anyData,
		thisObj, paramList, true, postCallback);

	va_end(paramList);
	return result;
}

bool DispatchUserDefinedEvent (const char* eventName, Script* sender, UInt32 argsArrayId, const char* senderName)
{
	ScopedLock lock(s_criticalSection);

	// does an EventInfo entry already exist for this event?
	const auto eventPtr = TryGetEventInfoForName(eventName);
	if (!eventPtr)
		return true; //don't signal an error, it just means no one has registered for this event yet.

	EventInfo& eventInfo = *eventPtr;
	if (!eventInfo.IsUserDefined())
		return false;

	// get or create args array
	ArrayVar *arr;
	if (argsArrayId)
	{
		arr = g_ArrayMap.Get(argsArrayId);
		if (!arr || (arr->KeyType() != kDataType_String))
			return false;
	}
	else
	{
		arr = g_ArrayMap.Create (kDataType_String, false, sender->GetModIndex ());
		argsArrayId = arr->ID();
	}

	// populate args array
	arr->SetElementString("eventName", eventName);
	if (senderName == nullptr)
	{
		if (sender)
			senderName = DataHandler::Get()->GetNthModName (sender->GetModIndex ());
		else
			senderName = "NVSE";
	}

	arr->SetElementString("eventSender", senderName);

	// dispatch
	HandleEvent(eventInfo, (void*)argsArrayId, nullptr);
	return true;
}

std::deque<DeferredCallback<false>> s_deferredCallbacksDefault;
std::deque<DeferredCallback<true>> s_deferredCallbacksWithIntsPackedAsFloats;

void Tick()
{
	ScopedLock lock(s_criticalSection);

	// handle deferred events
	s_deferredDeprecatedCallbacks.Clear();
	s_deferredCallbacksDefault.clear();
	s_deferredCallbacksWithIntsPackedAsFloats.clear();

	// Clear callbacks pending removal.
	s_deferredRemoveList.Clear();

	s_lastObj = nullptr;
	s_lastTarget = nullptr;
	s_lastEvent = NULL;
	s_lastOnHitWithActor = nullptr;
	s_lastOnHitWithWeapon = nullptr;
	s_lastOnHitVictim = nullptr;
	s_lastOnHitAttacker = nullptr;
}

void Init()
{
	// Registering internal events.
#define EVENT_INFO(name, params, hookInstaller, eventMask) \
	EventManager::RegisterEventEx(name, nullptr, true, (params ? sizeof(params) : 0), \
		params, eventMask, hookInstaller)

#define EVENT_INFO_WITH_ALIAS(name, alias, params, hookInstaller, eventMask) \
	EventManager::RegisterEventEx(name, alias, true, (params ? sizeof(params) : 0), \
		params, eventMask, hookInstaller)

	// Must define the events in the same order for their eEventID.
	EVENT_INFO("onadd", kEventParams_GameEvent, &s_MainEventHook, ScriptEventList::kEvent_OnAdd);
	EVENT_INFO_WITH_ALIAS("onactorequip", "onequip", kEventParams_GameEvent, &s_ActorEquipHook, ScriptEventList::kEvent_OnEquip);
	EVENT_INFO("ondrop", kEventParams_GameEvent, &s_MainEventHook, ScriptEventList::kEvent_OnDrop);
	EVENT_INFO_WITH_ALIAS("onactorunequip", "onunequip", kEventParams_GameEvent, &s_MainEventHook, ScriptEventList::kEvent_OnUnequip);
	EVENT_INFO("ondeath", kEventParams_GameEvent, &s_MainEventHook, ScriptEventList::kEvent_OnDeath);
	EVENT_INFO("onmurder", kEventParams_GameEvent, &s_MainEventHook, ScriptEventList::kEvent_OnMurder);
	EVENT_INFO("oncombatend", kEventParams_GameEvent, &s_MainEventHook, ScriptEventList::kEvent_OnCombatEnd);
	EVENT_INFO("onhit", kEventParams_GameEvent, &s_MainEventHook, ScriptEventList::kEvent_OnHit);
	EVENT_INFO("onhitwith", kEventParams_GameEvent, &s_MainEventHook, ScriptEventList::kEvent_OnHitWith);
	EVENT_INFO("onpackagechange", kEventParams_GameEvent, &s_MainEventHook, ScriptEventList::kEvent_OnPackageChange);
	EVENT_INFO("onpackagestart", kEventParams_GameEvent, &s_MainEventHook, ScriptEventList::kEvent_OnPackageStart);
	EVENT_INFO("onpackagedone", kEventParams_GameEvent, &s_MainEventHook, ScriptEventList::kEvent_OnPackageDone);
	EVENT_INFO("onload", kEventParams_GameEvent, &s_MainEventHook, ScriptEventList::kEvent_OnLoad);
	EVENT_INFO("onmagiceffecthit", kEventParams_GameEvent, &s_MainEventHook, ScriptEventList::kEvent_OnMagicEffectHit);
	EVENT_INFO("onsell", kEventParams_GameEvent, &s_MainEventHook, ScriptEventList::kEvent_OnSell);
	EVENT_INFO("onstartcombat", kEventParams_GameEvent, &s_MainEventHook, ScriptEventList::kEvent_OnStartCombat);
	EVENT_INFO("saytodone", kEventParams_GameEvent, &s_MainEventHook, ScriptEventList::kEvent_SayToDone);
	EVENT_INFO_WITH_ALIAS("ongrab", "on0x0080000", kEventParams_GameEvent, &s_MainEventHook, ScriptEventList::kEvent_OnGrab);
	EVENT_INFO("onopen", kEventParams_GameEvent, &s_MainEventHook, ScriptEventList::kEvent_OnOpen);
	EVENT_INFO("onclose", kEventParams_GameEvent, &s_MainEventHook, ScriptEventList::kEvent_OnClose);
	EVENT_INFO_WITH_ALIAS("onfire", "on0x00400000", kEventParams_GameEvent, &s_MainEventHook, ScriptEventList::kEvent_OnFire);
	EVENT_INFO("ontrigger", kEventParams_GameEvent, &s_MainEventHook, ScriptEventList::kEvent_OnTrigger);
	EVENT_INFO("ontriggerenter", kEventParams_GameEvent, &s_MainEventHook, ScriptEventList::kEvent_OnTriggerEnter);
	EVENT_INFO("ontriggerleave", kEventParams_GameEvent, &s_MainEventHook, ScriptEventList::kEvent_OnTriggerLeave);
	EVENT_INFO("onreset", kEventParams_GameEvent, &s_MainEventHook, ScriptEventList::kEvent_OnReset);

	EVENT_INFO("onactivate", kEventParams_GameEvent, &s_ActivateHook, kEventMask_OnActivate);
	EVENT_INFO("ondropitem", kEventParams_GameEvent, &s_MainEventHook, 0);

	EVENT_INFO("exitgame", nullptr, nullptr, 0);
	EVENT_INFO("exittomainmenu", nullptr, nullptr, 0);
	EVENT_INFO("loadgame", kEventParams_OneString, nullptr, 0);
	EVENT_INFO("savegame", kEventParams_OneString, nullptr, 0);
	EVENT_INFO("qqq", nullptr, nullptr, 0);
	EVENT_INFO("postloadgame", kEventParams_OneInt, nullptr, 0);
	EVENT_INFO("runtimescripterror", kEventParams_OneString, nullptr, 0);
	EVENT_INFO("deletegame", kEventParams_OneString, nullptr, 0);
	EVENT_INFO("renamegame", kEventParams_OneString, nullptr, 0);
	EVENT_INFO("renamenewgame", kEventParams_OneString, nullptr, 0);
	EVENT_INFO("newgame", nullptr, nullptr, 0);
	EVENT_INFO("deletegamename", kEventParams_OneString, nullptr, 0);
	EVENT_INFO("renamegamename", kEventParams_OneString, nullptr, 0);
	EVENT_INFO("renamenewgamename", kEventParams_OneString, nullptr, 0);

	EVENT_INFO("nvsetestevent", kEventParams_OneInt_OneFloat_OneArray_OneString_OneForm_OneReference_OneBaseform,
		nullptr, 0); // dispatched via DispatchEventAlt, for unit tests

	ASSERT (kEventID_InternalMAX == s_eventInfos.size());


#undef EVENT_INFO

	InstallDestroyCIOSHook();	// handle a missing parameter value check.

}

bool RegisterEventEx(const char* name, const char* alias, bool isInternal, UInt8 numParams, EventFilterType* paramTypes,
                     UInt32 eventMask, EventHookInstaller* hookInstaller, EventFlags flags)
{
	if (numParams > numMaxFilters) [[unlikely]]
		return false;
	if (!name) [[unlikely]]
		return false;

	EventInfo** infoPtr;
	if (!s_eventInfoMap.Insert(name, &infoPtr))
		return false; // event with this name already exists
	*infoPtr = &s_eventInfos.emplace_back(name, alias, paramTypes, numParams, eventMask, hookInstaller, flags);

	EventInfo& info = **infoPtr;
	if (alias && s_eventInfoMap.Insert(alias, &infoPtr))
	{
		*infoPtr = &info;
	}

	if (isInternal)
	{
		// provide fast access for internal events.
		s_internalEventInfos.Append(&info);
	}

	return true;
}

bool RegisterEvent(const char* name, UInt8 numParams, EventFilterType* paramTypes, EventFlags flags)
{
	return RegisterEventEx(name, nullptr, false, numParams, paramTypes, 0, nullptr, flags);
}

bool RegisterEventWithAlias(const char* name, const char* alias, UInt8 numParams, EventFilterType* paramTypes, EventFlags flags)
{
	return RegisterEventEx(name, alias, false, numParams, paramTypes, 0, nullptr, flags);
}

bool SetNativeEventHandler(const char* eventName, EventHandler func)
{
	EventCallback event(func);
	return SetHandler(eventName, event);
}

bool RemoveNativeEventHandler(const char* eventName, EventHandler func)
{
	const EventCallback event(func);
	return RemoveHandler(eventName, event);
}

bool EventHandlerExist(const char* ev, const EventCallback& handler)
{
	ScopedLock lock(s_criticalSection);
	if (EventInfo* infoPtr = TryGetEventInfoForName(ev))
	{
		CallbackMap& callbacks = infoPtr->callbacks;
		auto const basicCallback = GetBasicCallback(handler.toCall);
		auto const range = callbacks.equal_range(basicCallback);
		// loop over all EventCallbacks with the same callback script/function.
		for (auto i = range.first; i != range.second; ++i)
		{
			if (i->second.EqualFilters(handler)) 
			{
				return true;
			}
		}
	}
	return false;
}


}
