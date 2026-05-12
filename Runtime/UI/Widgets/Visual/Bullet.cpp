#include "UI/Widgets/Visual/Bullet.h"
#include "ImGui/imgui.h"

namespace NLS::UI::Widgets
{
void Bullet::_Draw_Impl()
{
	ImGui::Bullet();
}
}
