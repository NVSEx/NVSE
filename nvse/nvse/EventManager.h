#pragma once
#include "ScriptUtils.h"
#include "StackVector.h"

#ifdef RUNTIME

#include <string>

#include "ArrayVar.h"
#include "LambdaManager.h"
#include "PluginAPI.h"
#include <variant>

#include "FunctionScripts.h"


class Script;
class TESForm;
class TESObjectREFR;
class BGSListForm;
class Actor;
typedef void (*EventHookInstaller)();

// For dispatching events to scripts.
// Scripts can register an event handler for any of the supported events.
// Can optionally specify filters to match against the event arguments.
// Event handler is a function script which must take the expected number and types of arguments associated with the event.
// Supporting hooks only installed if at least one handler is registered for a particular event.

namespace EventManager
{
	extern Stack<const char *> s_eventStack;
	extern UInt32 s_eventsInUse;

	struct EventInfo;
	typedef Vector<EventInfo> EventInfoList;
	extern EventInfoList s_eventInfos;
	extern UnorderedMap<const char *, UInt32> s_eventNameToID;

	UInt32 EventIDForString(const char *eventStr);

	using EventHandler = NVSEEventManagerInterface::EventHandler;
	using EventFilterType = NVSEEventManagerInterface::ParamType;
	using EventFlags = NVSEEventManagerInterface::EventFlags;
	using DispatchReturn = NVSEEventManagerInterface::DispatchReturn;
	using DispatchCallback = NVSEEventManagerInterface::DispatchCallback;

	inline bool IsParamForm(EventFilterType pType)
	{
		return NVSEEventManagerInterface::IsFormParam(pType);
	}
	Script::VariableType ParamTypeToVarType(EventFilterType pType);
	EventFilterType VarTypeToParamType(Script::VariableType varType);
	DataType ParamTypeToDataType(EventFilterType pType);

	bool ParamTypeMatches(EventFilterType from, EventFilterType to);

	enum eEventID
	{
		// correspond to ScriptEventList event masks
		kEventID_OnAdd,
		kEventID_OnActorEquip,
		kEventID_OnDrop,
		kEventID_OnActorUnequip,
		kEventID_OnDeath,
		kEventID_OnMurder,
		kEventID_OnCombatEnd,
		kEventID_OnHit,
		kEventID_OnHitWith,
		kEventID_OnPackageChange,
		kEventID_OnPackageStart,
		kEventID_OnPackageDone,
		kEventID_OnLoad,
		kEventID_OnMagicEffectHit,
		kEventID_OnSell,
		kEventID_OnStartCombat,
		kEventID_SayToDone,
		kEventID_OnGrab,
		kEventID_OnOpen,
		kEventID_OnClose,
		kEventID_OnFire,
		kEventID_OnTrigger,
		kEventID_OnTriggerEnter,
		kEventID_OnTriggerLeave,
		kEventID_OnReset,

		kEventID_ScriptEventListMAX,

		// special-cased game events
		kEventID_OnActivate = kEventID_ScriptEventListMAX,
		kEventID_OnDropItem,

		kEventID_GameEventMAX,

		// NVSE internal events, correspond to NVSEMessagingInterface messages
		kEventID_ExitGame = kEventID_GameEventMAX,
		kEventID_ExitToMainMenu,
		kEventID_LoadGame,
		kEventID_SaveGame,
		kEventID_QQQ,
		kEventID_PostLoadGame,
		kEventID_RuntimeScriptError,
		kEventID_DeleteGame,
		kEventID_RenameGame,
		kEventID_RenameNewGame,
		kEventID_NewGame,
		kEventID_DeleteGameName,
		kEventID_RenameGameName,
		kEventID_RenameNewGameName,

		kEventID_InternalMAX,
		kEventID_DebugEvent = kEventID_InternalMAX, //only used in debug mode, for unit tests

		// user or plugin defined
		kEventID_UserDefinedMIN = kEventID_InternalMAX,

		kEventID_INVALID = 0xFFFFFFFF
	};

	// Represents an event handler registered for an event.
	class EventCallback
	{
		void TrySaveLambdaContext();
		bool ValidateFirstOrSecondFilter(bool isFirst, const EventInfo& parent, std::string& outErrorMsg) const;
		bool ValidateFirstAndSecondFilter(const EventInfo& parent, std::string& outErrorMsg) const;

	public:
		// If variant is Maybe_Lambda, must try to capture lambda context once the EventCallback is confirmed to stay.
		using CallbackFunc = std::variant<LambdaManager::Maybe_Lambda, EventHandler>;

		EventCallback() = default;
		~EventCallback() = default;
		EventCallback(Script *funcScript, TESForm *sourceFilter = nullptr, TESForm *objectFilter = nullptr)
			: toCall(funcScript), source(sourceFilter), object(objectFilter) {}

		EventCallback(EventHandler func, TESForm *sourceFilter = nullptr, TESForm *objectFilter = nullptr)
			: toCall(func), source(sourceFilter), object(objectFilter) {}

		EventCallback(const EventCallback &other) = delete;
		EventCallback &operator=(const EventCallback &other) = delete;

		EventCallback(EventCallback &&other) noexcept;
		EventCallback &operator=(EventCallback &&other) noexcept;

		CallbackFunc toCall{};
		TESForm *source{}; // first arg to handler (reference or base form or form list)
		TESForm *object{}; // second arg to handler
		bool removed{};
		bool pendingRemove{};

		using Index = UInt32;
		using Filter = SelfOwningArrayElement;

		// Indexes for filters must respect the max amount of BaseFilters for the base event definition.
		// If no filter is at an index = it is unfiltered for the nth BaseFilter.
		// Using a map to avoid adding duplicate indexes.
		std::map<Index, Filter> filters;

		[[nodiscard]] bool ValidateFilters(std::string& outErrorMsg, const EventInfo& parent);

		[[nodiscard]] std::string GetFiltersAsStr() const;
		[[nodiscard]] ArrayVar* GetFiltersAsArray(const Script* scriptObj) const;
		[[nodiscard]] std::string GetCallbackFuncAsStr() const;

		[[nodiscard]] bool IsRemoved() const { return removed; }
		void SetRemoved(bool bSet) { removed = bSet; }
		[[nodiscard]] bool EqualFilters(const EventCallback &rhs) const; // return true if the two handlers have matching filters.

		[[nodiscard]] Script *TryGetScript() const;
		[[nodiscard]] bool HasCallbackFunc() const;

		// If the EventCallback is confirmed to stay, then call this to wrap up loose ends, e.g save lambda var context.
		void Confirm();

		// If "this" is has more or equally generic filters than the arg Callback, return true.
		// Assumes both have the same callbacks.
		[[nodiscard]] bool ShouldRemoveCallback(const EventCallback& toCheck, const EventInfo& evInfo) const;

		std::unique_ptr<ScriptToken> Invoke(EventInfo *eventInfo, void *arg0, void *arg1);
	};

	//Does not attempt to store lambda info for Script*.
	using BasicCallbackFunc = std::variant<Script*, EventHandler>;

	BasicCallbackFunc GetBasicCallback(const EventCallback::CallbackFunc& func);

	//Each callback function can have multiple EventCallbacks.
	using CallbackMap = std::multimap<BasicCallbackFunc, EventCallback>;

	struct EventInfo
	{
		EventInfo(const char *name_, EventFilterType *params_, UInt8 nParams_, UInt32 eventMask_, EventHookInstaller *installer_,
				  EventFlags flags = EventFlags::kFlags_None)
			: evName(name_), paramTypes(params_), numParams(nParams_), eventMask(eventMask_), installHook(installer_), flags(flags)
		{
		}

		EventInfo(const char *name_, EventFilterType *params_, UInt8 numParams_, EventFlags flags = EventFlags::kFlags_None)
			: evName(name_), paramTypes(params_), numParams(numParams_), flags(flags) {}

		EventInfo() : evName(""), paramTypes(nullptr) {}

		EventInfo(const EventInfo &other) = delete;
		EventInfo& operator=(const EventInfo& other) = delete;

		EventInfo(EventInfo&& other) noexcept :
			evName(other.evName), paramTypes(other.paramTypes), numParams(other.numParams),
			eventMask(other.eventMask), callbacks(std::move(other.callbacks)), installHook(other.installHook), flags(other.flags)
		{
		}

		const char *evName; // must be lowercase (??)
		EventFilterType *paramTypes;
		UInt8 numParams = 0;
		UInt32 eventMask = 0;
		CallbackMap callbacks;
		EventHookInstaller *installHook{}; // if a hook is needed for this event type, this will be non-null.
										   // install it once and then set *installHook to NULL. Allows multiple events
										   // to use the same hook, installing it only once.
		EventFlags flags = EventFlags::kFlags_None;

		[[nodiscard]] bool FlushesOnLoad() const
		{
			return flags & EventFlags::kFlag_FlushOnLoad;
		}
		[[nodiscard]] bool IsUserDefined() const
		{
			return flags & EventFlags::kFlag_IsUserDefined;
		}
		// n is 0-based
		[[nodiscard]] EventFilterType TryGetNthParamType(size_t n) const
		{
			return !IsUserDefined() ? paramTypes[n] : EventFilterType::eParamType_Anything;
		}
	};

	using ArgStack = StackVector<void*, kMaxUdfParams>;
	static constexpr auto numMaxFilters = kMaxUdfParams;

	bool SetHandler(const char *eventName, EventCallback &toSet, ExpressionEvaluator* eval = nullptr);

	// removes handler only if all filters match
	bool RemoveHandler(const char *id, const EventCallback &toRemove);

	// handle an NVSEMessagingInterface message
	void HandleNVSEMessage(UInt32 msgID, void *data);

	// handle an eventID directly
	void __stdcall HandleEvent(UInt32 id, void *arg0, void *arg1);

	// name of whatever event is currently being handled, empty string if none
	const char *GetCurrentEventName();

	// called each frame to update internal state
	void Tick();

	void Init();

	bool RegisterEventEx(const char *name, UInt8 numParams, EventFilterType *paramTypes,
						 UInt32 eventMask = 0, EventHookInstaller *hookInstaller = nullptr,
						 EventFlags flags = EventFlags::kFlags_None);

	bool RegisterEvent(const char *name, UInt8 numParams, EventFilterType *paramTypes,
					   EventFlags flags = EventFlags::kFlags_None);

	bool SetNativeEventHandler(const char *eventName, EventHandler func);
	bool RemoveNativeEventHandler(const char *eventName, EventHandler func);

	template <bool ExtractIntTypeAsFloat>
	DispatchReturn DispatchEventRaw(TESObjectREFR* thisObj, EventInfo& eventInfo, ArgStack& params,
		DispatchCallback resultCallback, void* anyData = nullptr);

	template <bool ExtractIntTypeAsFloat>
	bool DispatchEventRaw(TESObjectREFR* thisObj, EventInfo& eventInfo, ArgStack& params);

	//For plugins
	bool DispatchEvent(const char *eventName, TESObjectREFR *thisObj, ...);
	DispatchReturn DispatchEventAlt(const char *eventName, DispatchCallback resultCallback, void *anyData, TESObjectREFR *thisObj, ...);

	// dispatch a user-defined event from a script (for Cmd_DispatchEvent)
	// Cmd_DispatchEventAlt provides more flexibility with how args are passed.
	bool DispatchUserDefinedEvent(const char *eventName, Script *sender, UInt32 argsArrayId, const char *senderName);

	// event handler param lists
	static EventFilterType kEventParams_GameEvent[2] =
	{
		EventFilterType::eParamType_AnyForm, EventFilterType::eParamType_AnyForm
	};

	static EventFilterType kEventParams_OneRef[1] =
	{
		EventFilterType::eParamType_AnyForm,
	};

	static EventFilterType kEventParams_OneString[1] =
	{
		EventFilterType::eParamType_String
	};

	static EventFilterType kEventParams_OneInt[1] =
	{
		EventFilterType::eParamType_Int
	};

	static EventFilterType kEventParams_TwoInts[2] =
	{
		EventFilterType::eParamType_Int, EventFilterType::eParamType_Int
	};

	static EventFilterType kEventParams_OneInt_OneRef[2] =
	{
		EventFilterType::eParamType_Int, EventFilterType::eParamType_AnyForm
	};

	static EventFilterType kEventParams_OneRef_OneInt[2] =
	{
		EventFilterType::eParamType_AnyForm, EventFilterType::eParamType_Int
	};

	static EventFilterType kEventParams_OneArray[1] =
	{
		EventFilterType::eParamType_Array
	};

	static EventFilterType kEventParams_OneInt_OneFloat_OneArray_OneString_OneForm_OneReference_OneBaseform[] =
	{
		EventFilterType::eParamType_Int,
		EventFilterType::eParamType_Float,
		EventFilterType::eParamType_Array,
		EventFilterType::eParamType_String,
		EventFilterType::eParamType_AnyForm,
		EventFilterType::eParamType_Reference,
		EventFilterType::eParamType_BaseForm,
	};





	// template definitions

	bool DoesFormMatchFilter(TESForm* form, TESForm* filter, bool expectReference, const UInt32 recursionLevel = 0);
	bool DoDeprecatedFiltersMatch(const EventCallback& callback, const ArgStack& params);

	// eParamType_Anything is treated as "use default param type" (usually for a User-Defined Event).
	template<bool ExtractIntTypeAsFloat>
	bool DoesFilterMatch(const ArrayElement& sourceFilter, void* param, EventFilterType filterType)
	{
		switch (sourceFilter.DataType()) {
		case kDataType_Numeric:
		{
			double filterNumber{};
			sourceFilter.GetAsNumber(&filterNumber);	//if the Event's paramType was Int, then this should be already Floored.
			float inputNumber;
			if constexpr (ExtractIntTypeAsFloat)
			{
				// this function could be being called by a function, where even ints are passed as floats.
				// Alternatively, it could be called by some internal function that got the param from an ArrayElement 
				inputNumber = *reinterpret_cast<float*>(&param);
				if (filterType == EventFilterType::eParamType_Int)
					inputNumber = floor(inputNumber);
			}
			else  
			{
				// this function is being called internally, via a va_arg-using function, so expect ints to be packed like ints.
				inputNumber = (filterType == EventFilterType::eParamType_Int)
					? static_cast<float>(*reinterpret_cast<UInt32*>(&param))
					: *reinterpret_cast<float*>(&param);
			}
			
			if (!FloatEqual(inputNumber, static_cast<float>(filterNumber)))
				return false;
			break;
		}
		case kDataType_Form:
		{
			UInt32 filterFormId{};
			sourceFilter.GetAsFormID(&filterFormId);
			auto* inputForm = static_cast<TESForm*>(param);
			auto* filterForm = LookupFormByID(filterFormId);
			// Allow matching a null form filter with a null input.
			bool const expectReference = (filterType != EventFilterType::eParamType_BaseForm)
				&& (filterType != EventFilterType::eParamType_AnyForm);
			if (!DoesFormMatchFilter(inputForm, filterForm, expectReference))
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
			break;
		}
		return true;
	}

	template<bool ExtractIntTypeAsFloat>
	bool DoesParamMatchFiltersInArray(const EventCallback& callback, const EventCallback::Filter& filter, EventFilterType paramType, void* param, int index)
	{
		ArrayID arrayFiltersId{};
		filter.GetAsArray(&arrayFiltersId);
		auto* arrayFilters = g_ArrayMap.Get(arrayFiltersId);
		if (!arrayFilters)
		{
			ShowRuntimeError(callback.TryGetScript(), "While checking event filters in array at index %d, the array was invalid/unitialized (array id: %d).", index, arrayFiltersId);
			return false;
		}
		// If array of filters is non-"array" type, then ignore the keys.
		for (auto iter = arrayFilters->GetRawContainer()->begin();
			iter != arrayFilters->GetRawContainer()->end(); ++iter)
		{
			auto const& elem = *iter.second();
			if (ParamTypeToVarType(paramType) != DataTypeToVarType(elem.DataType()))
				continue;
			if (DoesFilterMatch<ExtractIntTypeAsFloat>(elem, param, paramType))
				return true;
		}
		return false;
	}

	template<bool ExtractIntTypeAsFloat>
	bool DoFiltersMatch(TESObjectREFR* thisObj, const EventInfo& eventInfo, const EventCallback& callback, const ArgStack& params)
	{
		for (auto& [index, filter] : callback.filters)
		{
			bool const isCallingRefFilter = index == 0;

			if (index > params->size())
				return false; // insufficient params to match that filter.

			void* param = isCallingRefFilter ? thisObj : params->at(index - 1);

			if (eventInfo.IsUserDefined()) // Skip filter type checking.
			{
				if (!DoesFilterMatch<ExtractIntTypeAsFloat>(filter, param, EventFilterType::eParamType_Anything))
					return false;
				//TODO: add support for array of filters
			}
			else
			{
				auto const paramType = isCallingRefFilter ? EventFilterType::eParamType_Reference : eventInfo.paramTypes[index - 1];

				const auto filterDataType = filter.DataType();
				const auto filterVarType = DataTypeToVarType(filterDataType);
				const auto paramVarType = ParamTypeToVarType(paramType);

				if (filterVarType != paramVarType) //if true, can assume that the filterVar's type is Array (if it isn't, type mismatch should have been reported in SetEventHandler).
				{
					// assume elements of array are filters
					if (!DoesParamMatchFiltersInArray<ExtractIntTypeAsFloat>(callback, filter, paramType, param, index))
						return false;
					continue;
				}
				if (!DoesFilterMatch<ExtractIntTypeAsFloat>(filter, param, paramType))
					return false;
			}
		}
		return true;
	}

	template <bool ExtractIntTypeAsFloat>
	DispatchReturn DispatchEventRaw(TESObjectREFR* thisObj, EventInfo& eventInfo, ArgStack& params,
		DispatchCallback resultCallback, void* anyData)
	{
		using FunctionCaller = std::conditional_t<ExtractIntTypeAsFloat, InternalFunctionCallerAlt, InternalFunctionCaller>;

		DispatchReturn result = DispatchReturn::kRetn_Normal;
		for (auto& [funcKey, callback] : eventInfo.callbacks)
		{
			if (callback.IsRemoved())
				continue;

			if (!DoDeprecatedFiltersMatch(callback, params))
				continue;
			if (!DoFiltersMatch<ExtractIntTypeAsFloat>(thisObj, eventInfo, callback, params))
				continue;

			result = std::visit(overloaded{
				[=, &params](const LambdaManager::Maybe_Lambda& script) -> DispatchReturn
				{
					FunctionCaller caller(script.Get(), thisObj);
					caller.SetArgsRaw(params->size(), params->data());
					auto const res = UserFunctionManager::Call(std::move(caller));
					if (resultCallback)
					{
						NVSEArrayVarInterface::Element elem;
						if (PluginAPI::BasicTokenToPluginElem(res.get(), elem, script.Get()))
						{
							return resultCallback(elem, anyData) ? DispatchReturn::kRetn_Normal : DispatchReturn::kRetn_EarlyBreak;
						}
						return DispatchReturn::kRetn_Error;
					}
					return DispatchReturn::kRetn_Normal;
				},
				[&params, thisObj](EventHandler const handler) -> DispatchReturn
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

	template <bool ExtractIntTypeAsFloat>
	bool DispatchEventRaw(TESObjectREFR* thisObj, EventInfo& eventInfo, ArgStack& params)
	{
		return DispatchEventRaw<ExtractIntTypeAsFloat>(thisObj, eventInfo, params, nullptr, nullptr)
			!= DispatchReturn::kRetn_Error;
	}
};

#endif