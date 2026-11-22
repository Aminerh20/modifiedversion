#include "pch-il2cpp.h"
#include "animations_tab.h"
#include "gui-helpers.hpp"
#include "state.hpp"

namespace AnimationsTab {
	void Render() {
		ImGui::SameLine(100 * State.dpiScale);
		ImGui::BeginChild("###Animations", ImVec2(500 * State.dpiScale, 0), true, ImGuiWindowFlags_NoBackground);

		ImGui::Dummy(ImVec2(4, 4) * State.dpiScale);
		if (ToggleButton("Moonwalk", &State.Moonwalk)) State.Save();

		ImGui::Dummy(ImVec2(7, 7) * State.dpiScale);
		ImGui::Separator();
		ImGui::Dummy(ImVec2(7, 7) * State.dpiScale);

		if (SteppedSliderFloat("Player Speed Multiplier", &State.PlayerSpeed, 0.f, 10.f, 0.05f, "%.2fx", ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_NoInput)) {
			State.PrevPlayerSpeed = State.PlayerSpeed;
			State.Save();
		}
		if (ToggleButton("Multiply Speed", &State.MultiplySpeed)) State.Save();

		ImGui::EndChild();
	}
}
