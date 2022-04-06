#include <stdarg.h>
#include "EventManager.h"
#include "ArrayVar.h"
#include "PluginAPI.h"
#include "GameAPI.h"
#include "Utilities.h"
#include "ScriptUtils.h"
#include "SafeWrite.h"
#include "FunctionScripts.h"
#include "GameObjects.h"
#include "ThreadLocal.h"
#include "common/ICriticalSection.h"
#include "Hooks_Gameplay.h"
#include "GameOSDepend.h"
//#include "InventoryReference.h"

#include "GameData.h"
#include "GameRTTI.h"
#include "StackVector.h"

namespace EventManager {

using FilterStack = StackVector<void*, numMaxFilters>;

static ICriticalSection s_criticalSection;

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

#if _DEBUG
Map<const char*, UInt32> s_eventNameToID(0x40);
#else
UnorderedMap<const char*, UInt32> s_eventNameToID(0x40);
#endif
UInt32 EventIDForString(const char* eventStr)
{
	UInt32 *idPtr = s_eventNameToID.GetPtr(eventStr);
	return idPtr ? *idPtr : kEventID_INVALID;
}

// hook installers
static EventHookInstaller s_MainEventHook = InstallHook;
static EventHookInstaller s_ActivateHook = InstallActivateHook;
static EventHookInstaller s_ActorEquipHook = InstallOnActorEquipHook;

// event handler param lists
static ParamType kEventParams_GameEvent[2] =
{
	ParamType::eParamType_AnyForm, ParamType::eParamType_AnyForm
};

static ParamType kEventParams_OneRef[1] =
{
	ParamType::eParamType_AnyForm,
};

static ParamType kEventParams_OneString[1] =
{
	ParamType::eParamType_String
};

static ParamType kEventParams_OneInteger[1] =
{
	ParamType::eParamType_Integer
};

static ParamType kEventParams_TwoIntegers[2] =
{
	ParamType::eParamType_Integer, ParamType::eParamType_Integer
};

static ParamType kEventParams_OneFloat_OneRef[2] =
{
	 ParamType::eParamType_Float, ParamType::eParamType_AnyForm
};

static ParamType kEventParams_OneRef_OneInt[2] =
{
	ParamType::eParamType_AnyForm, ParamType::eParamType_Integer
};

static ParamType kEventParams_OneArray[1] =
{
	ParamType::eParamType_Array
};

///////////////////////////
// internal functions
//////////////////////////

void __stdcall HandleEvent(UInt32 id, void * arg0, void * arg1);
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

UInt32 EventIDForMask(UInt32 eventMask)
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

UInt32 EventIDForMessage(UInt32 msgID)
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

EventInfoList s_eventInfos(0x30);

bool EventCallback::Equals(const EventCallback& rhs) const
{
	return (toCall == rhs.toCall) && (object == rhs.object) && (source == rhs.source)
	&& (filters == rhs.filters);
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

class EventHandlerCaller : public InternalFunctionCaller
{
public:
	EventHandlerCaller(Script* script, EventInfo* eventInfo, void* arg0, void* arg1) : InternalFunctionCaller(script, nullptr), m_eventInfo(eventInfo)
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
		return param->varType == ParamTypeToVarType(m_eventInfo->paramTypes[paramIndex]);
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


std::unique_ptr<ScriptToken> EventCallback::Invoke(EventInfo* eventInfo, void* arg0, void* arg1)
{
	return std::visit(overloaded
		{
			[=](const LambdaManager::Maybe_Lambda& script)
			{
				ScopedLock lock(s_criticalSection);	//for event stack

				// handle immediately
				s_eventStack.Push(eventInfo->evName);
				auto ret = UserFunctionManager::Call(EventHandlerCaller(script.Get(), eventInfo, arg0, arg1));
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
struct DeferredCallback
{
	EventCallback		*callback;	//assume this contains a Script* CallbackFunc.
	void				*arg0;
	void				*arg1;
	EventInfo			*eventInfo;

	DeferredCallback(EventCallback *pCallback, void *_arg0, void *_arg1, EventInfo *_eventInfo) :
		callback(pCallback), arg0(_arg0), arg1(_arg1), eventInfo(_eventInfo) {}

	~DeferredCallback()
	{
		if (callback->removed)
			return;

		// assume callback is owned by a global; prevent data race.
		ScopedLock lock(s_criticalSection);

		callback->Invoke(eventInfo, arg0, arg1);
	}
};

typedef Stack<DeferredCallback> DeferredCallbackList;
DeferredCallbackList s_deferredCallbacks;

struct DeferredRemoveCallback
{
	EventInfo				*eventInfo;
	CallbackList::Iterator	iterator;

	DeferredRemoveCallback(EventInfo *pEventInfo, const CallbackList::Iterator &iter) : eventInfo(pEventInfo), iterator(iter) {}

	~DeferredRemoveCallback()
	{
		if (iterator.Get().removed)
		{
			eventInfo->callbacks.Remove(iterator);
			if (eventInfo->callbacks.Empty() && eventInfo->eventMask)
				s_eventsInUse &= ~eventInfo->eventMask;
		}
		else iterator.Get().pendingRemove = false;
	}
};

typedef Stack<DeferredRemoveCallback> DeferredRemoveList;
DeferredRemoveList s_deferredRemoveList;

enum class RefState {NotSet, Invalid, Valid};

// Deprecated in favor of EventManager::DispatchEvent
void __stdcall HandleEvent(UInt32 id, void* arg0, void* arg1)
{
	ScopedLock lock(s_criticalSection);
	EventInfo* eventInfo = &s_eventInfos[id];
	if (eventInfo->callbacks.Empty()) return;

	auto isArg0Valid = RefState::NotSet;
	for (auto iter = eventInfo->callbacks.Begin(); !iter.End(); ++iter)
	{
		EventCallback &callback = iter.Get();

		if (callback.IsRemoved())
			continue;

		// Check filters
		if (callback.source && arg0 != callback.source && eventInfo->numParams && eventInfo->paramTypes[0] == ParamType::eParamType_RefVar)
		{
			if (isArg0Valid == RefState::NotSet)
				isArg0Valid = IsValidReference(arg0) ? RefState::Valid : RefState::Invalid;
			if (isArg0Valid == RefState::Invalid || static_cast<TESObjectREFR*>(arg0)->baseForm != callback.source)
				continue;
		}

		if (callback.object && (callback.object != arg1))
			continue;

		if (GetCurrentThreadId() != g_mainThreadID)
		{
			// avoid potential issues with invoking handlers outside of main thread by deferring event handling
			s_deferredCallbacks.Push(&callback, arg0, arg1, eventInfo);
		}
		else
		{
			callback.Invoke(eventInfo, arg0, arg1);
		}
	}
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

bool SetHandler(const char* eventName, EventCallback& handler)
{
	if (!handler.HasCallbackFunc())
		return false;

	// Only handles script callbacks, since this is only kept for legacy purposes anyways.
	if (auto const script = handler.TryGetScript())
	{
		UInt32 setted = 0;

		// trying to use a FormList to specify the source filter
		if (handler.source && handler.source->GetTypeID() == 0x055 && recursiveLevel < 100)
		{
			const auto formList = static_cast<BGSListForm*>(handler.source);
			for (tList<TESForm>::Iterator iter = formList->list.Begin(); !iter.End(); ++iter)
			{
				EventCallback listHandler(script, iter.Get(), handler.object);
				recursiveLevel++;
				if (SetHandler(eventName, listHandler)) setted++;
				recursiveLevel--;
			}
			return setted > 0;
		}

		// trying to use a FormList to specify the object filter
		if (handler.object && handler.object->GetTypeID() == 0x055 && recursiveLevel < 100)
		{
			const auto formList = static_cast<BGSListForm*>(handler.object);
			for (tList<TESForm>::Iterator iter = formList->list.Begin(); !iter.End(); ++iter)
			{
				EventCallback listHandler(script, handler.source, iter.Get());
				recursiveLevel++;
				if (SetHandler(eventName, listHandler)) setted++;
				recursiveLevel--;
			}
			return setted > 0;
		}
	}

	ScopedLock lock(s_criticalSection);

	UInt32 *idPtr;
	if (s_eventNameToID.Insert(eventName, &idPtr))
	{
		// have to assume registering for a user-defined event which has not been used before this point
		*idPtr = s_eventInfos.Size();
		char *nameCopy = CopyString(eventName);
		StrToLower(nameCopy);
		s_eventInfos.Append(nameCopy, kEventParams_OneArray, 1);
	}

	if (UInt32 const id = *idPtr;
		id < s_eventInfos.Size())
	{
		EventInfo* info = &s_eventInfos[id];
		// is hook installed for this event type?
		if (info->installHook)
		{
			if (*info->installHook)
			{
				// if this hook is used by more than one event type, prevent it being installed a second time
				(*info->installHook)();
			}
			// mark event as having had its hook installed
			info->installHook = nullptr;
		}

		if (!info->callbacks.Empty())
		{
			// if an existing handler matches this one exactly, don't duplicate it
			for (auto iter = info->callbacks.Begin(); !iter.End(); ++iter)
			{
				if (iter.Get().Equals(handler))
				{
					// may be re-adding a previously removed handler, so clear the Removed flag
					iter.Get().SetRemoved(false);
					return false;
				}
			}
		}

		handler.Confirm();
		info->callbacks.Append(std::move(handler));

		s_eventsInUse |= info->eventMask;

		return true;
	}

	return false; 
}

bool RemoveHandler(const char* id, const EventCallback& handler)
{
	if (!handler.HasCallbackFunc())
		return false;

	// Only handles script callbacks, since this is only kept for legacy purposes anyways.
	if (auto const script = handler.TryGetScript())
	{
		UInt32 removed = 0;

		// trying to use a FormList to specify the source filter
		if (handler.source && handler.source->GetTypeID() == 0x055 && recursiveLevel < 100)
		{
			const auto formList = static_cast<BGSListForm*>(handler.source);
			for (tList<TESForm>::Iterator iter = formList->list.Begin(); !iter.End(); ++iter)
			{

				EventCallback listHandler(script, iter.Get(), handler.object);
				recursiveLevel++;
				if (RemoveHandler(id, listHandler)) removed++;
				recursiveLevel--;
			}
			return removed > 0;
		}

		// trying to use a FormList to specify the object filter
		if (handler.object && handler.object->GetTypeID() == 0x055 && recursiveLevel < 100)
		{
			const auto formList = static_cast<BGSListForm*>(handler.object);
			for (tList<TESForm>::Iterator iter = formList->list.Begin(); !iter.End(); ++iter)
			{
				EventCallback listHandler(script, handler.source, iter.Get());
				recursiveLevel++;
				if (RemoveHandler(id, listHandler)) removed++;
				recursiveLevel--;
			}
			return removed > 0;
		}
	}
	

	ScopedLock lock(s_criticalSection);

	UInt32 const eventType = EventIDForString(id);
	bool bRemovedAtLeastOne = false;
	if (eventType < s_eventInfos.Size())
	{
		EventInfo *eventInfo = &s_eventInfos[eventType];
		if (!eventInfo->callbacks.Empty())
		{
			for (auto iter = eventInfo->callbacks.Begin(); !iter.End(); ++iter)
			{
				EventCallback &callback = iter.Get();

				if (handler.toCall != callback.toCall)
					continue;

				if (handler.source && (handler.source != callback.source))
					continue;

				if (handler.object && (handler.object != callback.object))
					continue;

				callback.SetRemoved(true);

				if (!callback.pendingRemove)
				{
					callback.pendingRemove = true;
					s_deferredRemoveList.Push(eventInfo, iter);
				}

				bRemovedAtLeastOne = true;
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

	const UInt32 eventID = EventIDForMask(eventMask);
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
	const UInt32 eventID = EventIDForMessage(msgID);
	if (eventID != kEventID_INVALID)
		HandleEvent(eventID, data, nullptr);
}

bool DoesFormMatchFilter(TESForm* form, TESForm* filter, const UInt32 recursionLevel = 0)
{
	if (!filter || filter == form)
		return true;
	if (recursionLevel > 100) [[unlikely]]
		return false;
	if (IS_ID(filter, BGSListForm))
	{
		const auto* list = static_cast<BGSListForm*>(filter);
		for (auto* listForm : list->list)
		{
			if (DoesFormMatchFilter(form, listForm, recursionLevel + 1))
				return true;
		}
	}
	return false;
}

bool DoDeprecatedFiltersMatch(const EventInfo& eventInfo, const EventCallback& callback, const FilterStack& params)
{
	if (!callback.source && !callback.object)
		return true;
	// old filter system
	TESForm* sourceForm{};
	TESForm* objectForm{};
	if (eventInfo.numParams > 0 && IsParamForm(eventInfo.paramTypes[0]))
		sourceForm = static_cast<TESForm*>(params->at(0));
	if (eventInfo.numParams > 1 && IsParamForm(eventInfo.paramTypes[1]))
		objectForm = static_cast<TESForm*>(params->at(1));
	if (!DoesFormMatchFilter(sourceForm, callback.source) || !DoesFormMatchFilter(objectForm, callback.object))
		return false;
	return true;
}


bool DoesFilterMatch(const ArrayElement& sourceFilter, void* param)
{
	const auto filterDataType = sourceFilter.DataType();

	switch (filterDataType) {
	case kDataType_Numeric:
	{
		double filterNumber{};
		sourceFilter.GetAsNumber(&filterNumber);
		const auto inputNumber = *reinterpret_cast<float*>(&param);
		if (!FloatEqual(inputNumber, static_cast<float>(filterNumber)))
			return false;
		break;
	}
	case kDataType_Form:
	{
		UInt32 filterFormId{};
		sourceFilter.GetAsFormID(&filterFormId);
		auto* inputForm = static_cast<TESForm*>(param);
		if (!inputForm)
			return false;
		auto* filterForm = LookupFormByID(filterFormId);
		if (!filterForm || !DoesFormMatchFilter(inputForm, filterForm))
			return false;
		break;
	}
	case kDataType_String:
	{
		const char* filterStr{};
		sourceFilter.GetAsString(&filterStr);
		const auto inputStr = static_cast<const char*>(param);
		if (inputStr == filterStr)
			return true;
		if (!filterStr || !inputStr || StrCompare(filterStr, inputStr) != 0)
			return false;
		break;
	}
	case kDataType_Array:
	{
		ArrayID filterArrayId{};
		sourceFilter.GetAsArray(&filterArrayId);
		const auto inputArrayId = *reinterpret_cast<ArrayID*>(&param);
		if (!inputArrayId)
			return false;
		const auto inputArray = g_ArrayMap.Get(inputArrayId);
		const auto filterArray = g_ArrayMap.Get(filterArrayId);
		if (!inputArray || !filterArray || !inputArray->Equals(filterArray))
			return false;
		break;
	}
	case kDataType_Invalid:
	default: break;
	}
	return true;
}

bool DoesParamMatchFiltersInArray(const EventCallback& callback, const EventCallback::Filter& filter, Script::VariableType paramType, void* param)
{

	ArrayID arrayFiltersId{};
	filter.GetAsArray(&arrayFiltersId);
	const auto invalidArrayError = _L(, ShowRuntimeError(callback.TryGetScript(), "Array passed to SetEventHandler as filter is invalid (array id: %d)", arrayFiltersId));
	if (!arrayFiltersId)
	{
		invalidArrayError();
		return false;
	}
	auto* arrayFilters = g_ArrayMap.Get(arrayFiltersId);
	if (!arrayFilters)
	{
		invalidArrayError();
		return false;
	}
	if (arrayFilters->GetContainerType() != kContainer_Array)
	{
		ShowRuntimeError(callback.TryGetScript(), "Maps may not be passed as filter array in SetEventHandler");
		return false;
	}
	auto& elementVector = *arrayFilters->GetRawContainer()->getArrayPtr();
	for (auto& iter : elementVector)
	{
		if (paramType != DataTypeToVarType(iter.DataType()))
			continue;
		if (DoesFilterMatch(iter, param))
			return true;
	}
	return false;
}

Script::VariableType ParamTypeToVarType(ParamType pType)
{
	switch (pType)
	{
	case ParamType::eParamType_Float: return Script::VariableType::eVarType_Float;
	case ParamType::eParamType_Integer: return Script::VariableType::eVarType_Integer;
	case ParamType::eParamType_String: return Script::VariableType::eVarType_String;
	case ParamType::eParamType_Array: return Script::VariableType::eVarType_Array;
	case ParamType::eParamType_RefVar:
	case ParamType::eParamType_Reference:
	case ParamType::eParamType_BaseForm:
		return Script::VariableType::eVarType_Ref;
	case ParamType::eParamType_Invalid: return Script::VariableType::eVarType_Invalid;
	}
	return Script::VariableType::eVarType_Invalid;
}

bool DoFiltersMatch(const EventInfo& eventInfo, const EventCallback& callback, const FilterStack& params)
{
	for (auto& [index, filter] : callback.filters)
	{
		auto const zeroBasedIndex = index - 1;
		if (zeroBasedIndex > params->size() - 1) [[unlikely]]
		{
			ShowRuntimeError(callback.TryGetScript(), "Index %d passed to SetEventHandler exceeds number of arguments provided by event %s (number of args: %d)",
				index, eventInfo.evName, params->size());
			continue;
		}
		const auto filterDataType = filter.DataType();
		const auto filterVarType = DataTypeToVarType(filterDataType);
		const auto paramVarType = ParamTypeToVarType(eventInfo.paramTypes[zeroBasedIndex]);
		void* param = params->at(zeroBasedIndex);
		if (filterVarType != paramVarType)
		{
			if (filterDataType != kDataType_Array)
			{
				ShowRuntimeError(callback.TryGetScript(), "Filter passed to SetEventHandler does not match type passed to event at index %d (%s != %s)",
					index, DataTypeToString(filterDataType), VariableTypeToName(paramVarType));
				continue;
			}
			// assume elements of array are filters
			if (!DoesParamMatchFiltersInArray(callback, filter, paramVarType, param))
				return false;
			continue;
		}
		if (!DoesFilterMatch(filter, param))
			return false;
	}
	return true;
}

DispatchReturn DispatchEventRaw(TESObjectREFR* thisObj, EventInfo& eventInfo, FilterStack& params,
	DispatchCallback resultCallback = nullptr, void* anyData = nullptr)
{
	DispatchReturn result = DispatchReturn::kRetn_Normal;
	for (auto iter = eventInfo.callbacks.Begin(); !iter.End(); ++iter)
	{
		const auto& callback = iter.Get();
		if (callback.IsRemoved())
			continue;

		if (!DoDeprecatedFiltersMatch(eventInfo, callback, params))
			continue;
		if (!DoFiltersMatch(eventInfo, callback, params))
			continue;

		result = std::visit(overloaded{
			[=, &params, &eventInfo](const LambdaManager::Maybe_Lambda& script) -> DispatchReturn
			{
				InternalFunctionCaller caller(script.Get(), thisObj);
				caller.SetArgsRaw(eventInfo.numParams, params->data());
				auto const res = UserFunctionManager::Call(std::move(caller));
				if (resultCallback)
				{
					NVSEArrayVarInterface::Element elem;
					if (PluginAPI::BasicTokenToPluginElem(res.get(), elem))
					{
						return resultCallback(elem, anyData) ? DispatchReturn::kRetn_Normal : DispatchReturn::kRetn_EarlyBreak;
					}
					return DispatchReturn::kRetn_Error;
				}
				return DispatchReturn::kRetn_Normal;
			},
			[&params, thisObj](EventHandler handler)-> DispatchReturn
			{
				handler(thisObj, params->data());
				return DispatchReturn::kRetn_Normal;
			},
			}, callback.toCall);

		if (result != DispatchReturn::kRetn_Normal)
			break;
	}
	return result;
}


bool DispatchEvent(const char* eventName, TESObjectREFR* thisObj, ...)
{
	const auto eventId = EventIDForString(eventName);
	if (eventId == static_cast<UInt32>(kEventID_INVALID))
		return false;

	EventInfo& eventInfo = s_eventInfos[eventId];
	if (eventInfo.numParams > numMaxFilters)
		return false;

	va_list paramList;
	va_start(paramList, thisObj);

	FilterStack params;
	for (int i = 0; i < eventInfo.numParams; ++i)
		params->push_back(va_arg(paramList, void*));

	bool const result = DispatchEventRaw(thisObj, eventInfo, params) != DispatchReturn::kRetn_Error;
	
	va_end(paramList);
	return result;
}

DispatchReturn DispatchEventAlt(const char* eventName, DispatchCallback resultCallback, void* anyData, TESObjectREFR* thisObj, ...)
{
	const auto eventId = EventIDForString(eventName);
	if (eventId == static_cast<UInt32>(kEventID_INVALID))
		return DispatchReturn::kRetn_Error;

	EventInfo& eventInfo = s_eventInfos[eventId];
	if (eventInfo.numParams > numMaxFilters)
		return DispatchReturn::kRetn_Error;

	va_list paramList;
	va_start(paramList, thisObj);

	FilterStack params;
	for (int i = 0; i < eventInfo.numParams; ++i)
		params->push_back(va_arg(paramList, void*));

	auto const result = DispatchEventRaw(thisObj, eventInfo, params, resultCallback, anyData);

	va_end(paramList);
	return result;
}

bool DispatchUserDefinedEvent (const char* eventName, Script* sender, UInt32 argsArrayId, const char* senderName)
{
	ScopedLock lock(s_criticalSection);

	// does an EventInfo entry already exist for this event?
	const UInt32 eventID = EventIDForString (eventName);
	if (kEventID_INVALID == eventID)
		return true;

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
	HandleEvent(eventID, (void*)argsArrayId, nullptr);
	return true;
}

void Tick()
{
	ScopedLock lock(s_criticalSection);

	// handle deferred events
	s_deferredCallbacks.Clear();

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
#define EVENT_INFO(name, params, hookInstaller, eventMask) \
	EventManager::RegisterEventEx(name, params ? sizeof(params) : 0, params, eventMask, hookInstaller)
	
	EVENT_INFO("onadd", kEventParams_GameEvent, &s_MainEventHook, ScriptEventList::kEvent_OnAdd);
	EVENT_INFO("onactorequip", kEventParams_GameEvent, &s_ActorEquipHook, ScriptEventList::kEvent_OnEquip);
	EVENT_INFO("ondrop", kEventParams_GameEvent, &s_MainEventHook, ScriptEventList::kEvent_OnDrop);
	EVENT_INFO("onactorunequip", kEventParams_GameEvent, &s_MainEventHook, ScriptEventList::kEvent_OnUnequip);
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
	EVENT_INFO("ongrab", kEventParams_GameEvent, &s_MainEventHook, ScriptEventList::kEvent_OnGrab);
	EVENT_INFO("onopen", kEventParams_GameEvent, &s_MainEventHook, ScriptEventList::kEvent_OnOpen);
	EVENT_INFO("onclose", kEventParams_GameEvent, &s_MainEventHook, ScriptEventList::kEvent_OnClose);
	EVENT_INFO("onfire", kEventParams_GameEvent, &s_MainEventHook, ScriptEventList::kEvent_OnFire);
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
	EVENT_INFO("postloadgame", kEventParams_OneInteger, nullptr, 0);
	EVENT_INFO("runtimescripterror", kEventParams_OneString, nullptr, 0);
	EVENT_INFO("deletegame", kEventParams_OneString, nullptr, 0);
	EVENT_INFO("renamegame", kEventParams_OneString, nullptr, 0);
	EVENT_INFO("renamenewgame", kEventParams_OneString, nullptr, 0);
	EVENT_INFO("newgame", nullptr, nullptr, 0);
	EVENT_INFO("deletegamename", kEventParams_OneString, nullptr, 0);
	EVENT_INFO("renamegamename", kEventParams_OneString, nullptr, 0);
	EVENT_INFO("renamenewgamename", kEventParams_OneString, nullptr, 0);

	s_eventNameToID["onequip"] = kEventID_OnActorEquip;
	s_eventNameToID["onunequip"] = kEventID_OnActorUnequip;
	s_eventNameToID["on0x0080000"] = kEventID_OnGrab;
	s_eventNameToID["on0x00400000"] = kEventID_OnFire;

	ASSERT (kEventID_InternalMAX == s_eventInfos.Size());

#undef EVENT_INFO

	InstallDestroyCIOSHook();	// handle a missing parameter value check.

}

bool RegisterEventEx(const char* name, UInt8 numParams, ParamType* paramTypes, 
	UInt32 eventMask, EventHookInstaller* hookInstaller, EventFlags flags)
{
	UInt32* idPtr;
	if (!s_eventNameToID.Insert(name, &idPtr))
		return false; // event with this name already exists
	*idPtr = s_eventInfos.Size();
	const auto event = EventInfo(name, paramTypes, numParams, eventMask, hookInstaller, flags);
	s_eventInfos.Append(event);
	return true;
}

bool RegisterEvent(const char* name, UInt8 numParams, ParamType* paramTypes, EventFlags flags)
{
	return RegisterEventEx(name, numParams, paramTypes, flags);
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
	const UInt32 eventType = EventIDForString(ev);
	if (eventType < s_eventInfos.Size()) 
	{
		CallbackList& callbacks = s_eventInfos[eventType].callbacks;
		for (auto iter = callbacks.Begin(); !iter.End(); ++iter) 
		{
			if (iter.Get().Equals(handler)) 
			{
				return true;
			}
		}
	}
	return false;
}


}
