namespace {
	struct RenderConsoleToFramebufferHook {
		static void thunk(RE::UI *ui, RE::RENDER_TARGETS::RENDER_TARGET target, const RE::BSFixedString *menuName, uint32_t targetWidth, uint32_t targetHeight) {
			if (!menuName || *menuName != RE::Console::MENU_NAME) {
				original(ui, target, menuName, targetWidth, targetHeight);
				return;
			}

			auto menu = ui->GetMenu(RE::Console::MENU_NAME);
			if (!menu || !menu->uiMovie) {
				original(ui, target, menuName, targetWidth, targetHeight);
				return;
			}

			RE::GViewport origViewport;
			menu->uiMovie->GetViewport(&origViewport);
			RE::GViewport tempViewport = origViewport; // copy

			// scale the console so its width fits the target buffer (desktop framebuffer), height is scaled to the ratio and it's attached to the bottom of the viewport
			auto bufferWidth = (int32_t) targetWidth;
			auto bufferHeight = (int32_t) targetHeight;
			auto ratio = (float) targetWidth / (float) origViewport.width;
			auto menuHeight = (int32_t) ((float) origViewport.height * ratio);
			tempViewport.bufferWidth = bufferWidth;
			tempViewport.bufferHeight = bufferHeight;
			tempViewport.left = 0;
			tempViewport.top = bufferHeight - menuHeight;
			tempViewport.width = bufferWidth;
			tempViewport.height = menuHeight;
			tempViewport.scissorLeft = 0;
			tempViewport.scissorTop = 0;
			tempViewport.scissorWidth = bufferWidth;
			tempViewport.scissorHeight = bufferHeight;
			tempViewport.scale = origViewport.scale / ratio;

			menu->uiMovie->SetViewport(tempViewport);
			original(ui, target, menuName, targetWidth, targetHeight);
			menu->uiMovie->SetViewport(origViewport);
		}

		static inline REL::Relocation<decltype(thunk)> original;

		static void Install() {
			REL::Relocation<uintptr_t> callSite{REL::Offset(0xF1C7CA)};
			auto &trampoline = SKSE::GetTrampoline();
			original = trampoline.write_call<5>(callSite.address(), thunk);
		}
	};

	struct MovedMenu {
		RE::BSFixedString name;
		int8_t origDepth;
	};

	std::vector<MovedMenu> movedMenus;

	// NOTE: all this order preserving code is probably an overkill for the problem at hand, but Claude was anal pointing out edge-cases, so here it is...

	bool FindMovedMenuOrigDepth(RE::IMenu *menu, int8_t *origDepth) {
		auto &name = menu->GetVRRuntimeData()->menuName;
		for (uint32_t i = 0; i < movedMenus.size(); i++) {
			if (movedMenus[i].name == name) {
				*origDepth = movedMenus[i].origDepth;
				return true;
			}
		}
		return false;
	}

	void MoveStackEntryBelowMist(RE::BSTArray<RE::GPtr<RE::IMenu> > &menuStack, uint32_t mistIndex, uint32_t targetIndex) {
		auto targetPtr = menuStack[targetIndex];

		// preserve the depth priority order when moving
		auto origDepth = targetPtr->depthPriority;
		uint32_t insertIndex = mistIndex;
		while (insertIndex > 0) {
			auto below = menuStack[insertIndex - 1].get();
			int8_t belowOrigDepth;
			if (!below || !FindMovedMenuOrigDepth(below, &belowOrigDepth) || belowOrigDepth <= origDepth) break;
			insertIndex--;
		}

		// move the menu below mist in the stack
		movedMenus.push_back({targetPtr->GetVRRuntimeData()->menuName, origDepth});
		targetPtr->depthPriority = (int8_t) (menuStack[mistIndex]->depthPriority - 1);

		for (uint32_t i = targetIndex; i > insertIndex; i--) {
			menuStack[i] = menuStack[i - 1];
		}
		menuStack[insertIndex] = targetPtr;
	}

	void MoveMenuBelowMist(RE::UI *ui, RE::IMenu *target) {
		auto &menuStack = ui->menuStack;

		uint32_t targetIndex = UINT32_MAX;
		uint32_t mistIndex = UINT32_MAX;
		for (uint32_t menuIndex = 0; menuIndex < menuStack.size(); menuIndex++) {
			if (!menuStack[menuIndex]) continue;
			auto menu = menuStack[menuIndex].get();
			if (menu == target) {
				targetIndex = menuIndex;
			} else if (menu->GetVRRuntimeData()->menuName == RE::MistMenu::MENU_NAME) {
				mistIndex = menuIndex;
			}
		}

		if (targetIndex != UINT32_MAX && mistIndex != UINT32_MAX && targetIndex > mistIndex) {
			MoveStackEntryBelowMist(menuStack, mistIndex, targetIndex);
		}
	}

	bool IsProjectedMenu(RE::IMenu *menu) {
		auto renderTarget = menu->GetVRRuntimeData()->renderTarget;
		// kNone basically defaults to kProjected later, so we need to check both
		return renderTarget == RE::VRMenuRenderTarget::kNone || renderTarget == RE::VRMenuRenderTarget::kProjectedMenu;
	}

	void MoveOpenMenusBelowMist(RE::UI *ui) {
		auto &menuStack = ui->menuStack;
		uint32_t mistIndex = UINT32_MAX;
		for (uint32_t menuIndex = 0; menuIndex < menuStack.size(); menuIndex++) {
			if (!menuStack[menuIndex]) continue;
			auto menu = menuStack[menuIndex].get();
			if (menu->GetVRRuntimeData()->menuName == RE::MistMenu::MENU_NAME) {
				// found the mist, record its position
				mistIndex = menuIndex;
			} else if (mistIndex != UINT32_MAX && IsProjectedMenu(menu)) {
				// found the menu above mist, move it below
				MoveStackEntryBelowMist(menuStack, mistIndex, menuIndex);
				mistIndex++;
			}
		}
	}

	void OnMenuOpen(RE::UI *ui, const RE::BSFixedString &name) {
		if (name == RE::MistMenu::MENU_NAME) {
			MoveOpenMenusBelowMist(ui);
		} else {
			auto menu = ui->GetMenu(name);
			if (!menu || !IsProjectedMenu(menu.get())) return;
			MoveMenuBelowMist(ui, menu.get());
		}
	}

	void RestoreMovedMenus(RE::UI *ui) {
		auto &menuStack = ui->menuStack;
		for (uint32_t menuIndex = menuStack.size(); menuIndex-- > 0;) {
			if (!menuStack[menuIndex]) continue;
			auto menu = menuStack[menuIndex].get();
			int8_t origDepth;
			if (!FindMovedMenuOrigDepth(menu, &origDepth)) continue;

			menu->depthPriority = origDepth;
			auto menuPtr = menuStack[menuIndex];
			uint32_t i = menuIndex;
			while (i + 1 < menuStack.size() && menuStack[i + 1] && menuStack[i + 1]->depthPriority < origDepth) {
				menuStack[i] = menuStack[i + 1];
				i++;
			}
			menuStack[i] = menuPtr;
		}
		movedMenus.clear();
	}

	void OnMenuClose(RE::UI *ui, const RE::BSFixedString &name) {
		if (name == RE::MistMenu::MENU_NAME) {
			RestoreMovedMenus(ui);
			return;
		}

		for (uint32_t i = 0; i < movedMenus.size(); ++i) {
			if (movedMenus[i].name != name) continue;

			if (auto menu = ui->GetMenu(name)) {
				menu->depthPriority = movedMenus[i].origDepth;
			}
			movedMenus.erase(movedMenus.begin() + i);
			return;
		}
	}

	class MenuEventSink : public RE::BSTEventSink<RE::MenuOpenCloseEvent> {
	public:
		RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent *event, RE::BSTEventSource<RE::MenuOpenCloseEvent> *) override {
			if (event) {
				auto ui = RE::UI::GetSingleton();
				if (event->opening) {
					OnMenuOpen(ui, event->menuName);
				} else {
					OnMenuClose(ui, event->menuName);
				}
			}
			return RE::BSEventNotifyControl::kContinue;
		}
	};

	void OnSKSEMessage(SKSE::MessagingInterface::Message *message) {
		if (message->type == SKSE::MessagingInterface::kInputLoaded) {
			static MenuEventSink menuEventSink;
			RE::UI::GetSingleton()->AddEventSink<RE::MenuOpenCloseEvent>(&menuEventSink);
		}
	}
}

SKSEPluginLoad(const SKSE::LoadInterface *skse) {
	SKSE::Init(skse, {.log = false, .trampoline = true, .trampolineSize = 14}); // 6 op + 8 addr
	RenderConsoleToFramebufferHook::Install();
	SKSE::GetMessagingInterface()->RegisterListener(OnSKSEMessage);
	return true;
}
