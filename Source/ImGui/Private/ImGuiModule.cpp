// Distributed under the MIT License (MIT) (see accompanying LICENSE file)

#include "ImGuiPrivatePCH.h"

#include "ImGuiModuleManager.h"

#include "ImGuiTextureHandle.h"
#include "TextureManager.h"
#include "Utilities/WorldContext.h"
#include "Utilities/WorldContextIndex.h"

#if WITH_EDITOR
#include "ImGuiImplementation.h"
#include "Editor/ImGuiEditor.h"
#endif

#include <IPluginManager.h>


#define LOCTEXT_NAMESPACE "FImGuiModule"


namespace CVars
{
	extern TAutoConsoleVariable<int32> InputEnabled;
	extern TAutoConsoleVariable<int32> ShowDemo;
}

namespace Commands
{
	const TCHAR* SwitchInputMode = TEXT("ImGui.SwitchInputMode");
}

void SwitchImGuiInputMode()
{
	FImGuiModule::Get().ToggleInputMode();
}

FAutoConsoleCommand SwitchInputModeCommand = FAutoConsoleCommand(
	Commands::SwitchInputMode,
	TEXT("Changes ImGui input mode."),
	FConsoleCommandDelegate::CreateStatic(SwitchImGuiInputMode));

struct EDelegateCategory
{
	enum
	{
		// Default per-context draw events.
		Default,

		// Multi-context draw event defined in context manager.
		MultiContext
	};
};

static FImGuiModuleManager* ImGuiModuleManager = nullptr;

#if WITH_EDITOR
static FImGuiEditor* ImGuiEditor = nullptr;
#endif

#if WITH_EDITOR
FImGuiDelegateHandle FImGuiModule::AddEditorImGuiDelegate(const FImGuiDelegate& Delegate)
{
	checkf(ImGuiModuleManager, TEXT("Null pointer to internal module implementation. Is module available?"));

	return { ImGuiModuleManager->GetContextManager().GetEditorContextProxy().OnDraw().Add(Delegate),
		EDelegateCategory::Default, Utilities::EDITOR_CONTEXT_INDEX };
}
#endif

FImGuiDelegateHandle FImGuiModule::AddWorldImGuiDelegate(const FImGuiDelegate& Delegate)
{
	checkf(ImGuiModuleManager, TEXT("Null pointer to internal module implementation. Is module available?"));

#if WITH_EDITOR
	checkf(GEngine, TEXT("Null GEngine. AddWorldImGuiDelegate should be only called with GEngine initialized."));

	const FWorldContext* WorldContext = Utilities::GetWorldContext(GEngine->GameViewport);
	if (!WorldContext)
	{
		WorldContext = Utilities::GetWorldContextFromNetMode(ENetMode::NM_DedicatedServer);
	}

	checkf(WorldContext, TEXT("Couldn't find current world. AddWorldImGuiDelegate should be only called from a valid world."));

	int32 Index;
	FImGuiContextProxy& Proxy = ImGuiModuleManager->GetContextManager().GetWorldContextProxy(*WorldContext->World(), Index);
#else
	const int32 Index = Utilities::STANDALONE_GAME_CONTEXT_INDEX;
	FImGuiContextProxy& Proxy = ImGuiModuleManager->GetContextManager().GetWorldContextProxy();
#endif

	return{ Proxy.OnDraw().Add(Delegate), EDelegateCategory::Default, Index };
}

FImGuiDelegateHandle FImGuiModule::AddMultiContextImGuiDelegate(const FImGuiDelegate& Delegate)
{
	checkf(ImGuiModuleManager, TEXT("Null pointer to internal module implementation. Is module available?"));

	return { ImGuiModuleManager->GetContextManager().OnDrawMultiContext().Add(Delegate), EDelegateCategory::MultiContext };
}

void FImGuiModule::RemoveImGuiDelegate(const FImGuiDelegateHandle& Handle)
{
	if (ImGuiModuleManager)
	{
		if (Handle.Category == EDelegateCategory::MultiContext)
		{
			ImGuiModuleManager->GetContextManager().OnDrawMultiContext().Remove(Handle.Handle);
		}
		else if (auto* Proxy = ImGuiModuleManager->GetContextManager().GetContextProxy(Handle.Index))
		{
			Proxy->OnDraw().Remove(Handle.Handle);
		}
	}
}

FImGuiTextureHandle FImGuiModule::FindTextureHandle(const FName& Name)
{
	const TextureIndex Index = ImGuiModuleManager->GetTextureManager().FindTextureIndex(Name);
	return (Index != INDEX_NONE) ? FImGuiTextureHandle{ Name, ImGuiInterops::ToImTextureID(Index) } : FImGuiTextureHandle{};
}

FImGuiTextureHandle FImGuiModule::RegisterTexture(const FName& Name, class UTexture2D* Texture, bool bMakeUnique)
{
	const TextureIndex Index = ImGuiModuleManager->GetTextureManager().CreateTextureResources(Name, Texture, bMakeUnique);
	return FImGuiTextureHandle{ Name, ImGuiInterops::ToImTextureID(Index) };
}

void FImGuiModule::ReleaseTexture(const FImGuiTextureHandle& Handle)
{
	if (Handle.IsValid())
	{
		ImGuiModuleManager->GetTextureManager().ReleaseTextureResources(ImGuiInterops::ToTextureIndex(Handle.GetTextureId()));
	}
}

void FImGuiModule::StartupModule()
{
	// Create managers that implements module logic.

	checkf(!ImGuiModuleManager, TEXT("Instance of the ImGui Module Manager already exists. Instance should be created only during module startup."));
	ImGuiModuleManager = new FImGuiModuleManager();

#if WITH_EDITOR
	checkf(!ImGuiEditor, TEXT("Instance of the ImGui Editor already exists. Instance should be created only during module startup."));
	ImGuiEditor = new FImGuiEditor();
#endif
}

void FImGuiModule::ShutdownModule()
{
	// Before we shutdown we need to delete managers that will do all the necessary cleanup.

#if WITH_EDITOR
	checkf(ImGuiEditor, TEXT("Null ImGui Editor. ImGui editor instance should be deleted during module shutdown."));
	delete ImGuiEditor;
	ImGuiEditor = nullptr;
#endif

	checkf(ImGuiModuleManager, TEXT("Null ImGui Module Manager. Module manager instance should be deleted during module shutdown."));
	delete ImGuiModuleManager;
	ImGuiModuleManager = nullptr;

#if WITH_EDITOR
	// When shutting down we leave the global ImGui context pointer and handle pointing to resources that are already
	// deleted. This can cause troubles after hot-reload when code in other modules calls ImGui interface functions
	// which are statically bound to the obsolete module. To keep ImGui code functional we can redirect context handle
	// to point to the new module.
	FModuleManager::Get().OnModulesChanged().AddLambda([this](FName Name, EModuleChangeReason Reason)
	{
		if (Reason == EModuleChangeReason::ModuleLoaded && Name == "ImGui")
		{
			FImGuiModule& LoadedModule = FImGuiModule::Get();
			if (&LoadedModule != this)
			{
				ImGuiImplementation::SetImGuiContextHandle(LoadedModule.GetImGuiContextHandle());
			}
		}
	});
#endif // WITH_EDITOR
}

#if WITH_EDITOR
ImGuiContext** FImGuiModule::GetImGuiContextHandle()
{
	return ImGuiImplementation::GetImGuiContextHandle();
}
#endif

bool FImGuiModule::IsInputMode() const
{
	return CVars::InputEnabled.GetValueOnAnyThread() > 0;
}

void FImGuiModule::SetInputMode(bool bEnabled)
{
	// This function is for supporting shortcut or subsitiute for console command, so we are using the same priority.
	CVars::InputEnabled->Set(bEnabled ? 1 : 0, ECVF_SetByConsole);
}

void FImGuiModule::ToggleInputMode()
{
	SetInputMode(!IsInputMode());
}

bool FImGuiModule::IsShowingDemo() const
{
	return CVars::ShowDemo.GetValueOnAnyThread() > 0;
}

void FImGuiModule::SetShowDemo(bool bShow)
{
	// This function is for supporting shortcut or subsitiute for console command, so we are using the same priority.
	CVars::ShowDemo->Set(bShow ? 1 : 0, ECVF_SetByConsole);
}

void FImGuiModule::ToggleShowDemo()
{
	SetShowDemo(!IsShowingDemo());
}

//----------------------------------------------------------------------------------------------------
// Partial implementations of other classes that needs access to ImGuiModuleManager
//----------------------------------------------------------------------------------------------------

bool FImGuiTextureHandle::HasValidEntry() const
{
	const TextureIndex Index = ImGuiInterops::ToTextureIndex(TextureId);
	return Index != INDEX_NONE && ImGuiModuleManager && ImGuiModuleManager->GetTextureManager().GetTextureName(Index) == Name;
}


#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FImGuiModule, ImGui)
