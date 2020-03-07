#include "UIWidget.h"

std::vector<UIWidget*> UIWidget::_widgets;

void UIWidget::RegisterType()
{
    i32 r = ScriptEngine::RegisterScriptClass("UIWidget", 0, asOBJ_REF | asOBJ_NOCOUNT);
    assert(r >= 0);
    {
        RegisterBase<UIWidget>();
        ScriptEngine::RegisterScriptFunction("UIWidget@ CreateWidget()", asFUNCTION(UIWidget::Create));
    }
}

std::string UIWidget::GetTypeName()
{
    return "UIWidget";
}
void UIWidget::SetPosition(f32 x, f32 y, f32 depth)
{
    _widget.SetPosition(Vector3(x, y, depth));
}
void UIWidget::SetSize(f32 width, f32 height)
{
    _widget.SetSize(Vector3(width, height));
}