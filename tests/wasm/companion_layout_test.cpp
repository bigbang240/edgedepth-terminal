#include "imgui.h"
#include "imgui_internal.h"
#include <cstdio>
#include <cstdlib>
#include <string>

static void check(bool ok, const char* why) {
    if (!ok) { std::fprintf(stderr, "FAIL %s\n", why); std::exit(1); }
}
static void init() {
    ImGui::CreateContext();
    auto& io = ImGui::GetIO(); io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(1200,800); io.DeltaTime = 1.0f/60;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    unsigned char* pixels; int width,height;
    io.Fonts->GetTexDataAsRGBA32(&pixels,&width,&height);
}
static void frame(bool companions, bool build=false) {
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0,0)); ImGui::SetNextWindowSize(ImVec2(1200,800));
    ImGui::Begin("host",nullptr,ImGuiWindowFlags_NoTitleBar|ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoResize);
    auto root = ImGui::GetID("dock");
    if (build) {
        ImGui::DockBuilderRemoveNode(root);
        ImGui::DockBuilderAddNode(root,ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(root,ImVec2(1200,800));
        ImGuiID chart, right, dom, tape;
        ImGui::DockBuilderSplitNode(root,ImGuiDir_Right,.27f,&right,&chart);
        ImGui::DockBuilderSplitNode(right,ImGuiDir_Down,.36f,&tape,&dom);
        ImGui::DockBuilderDockWindow("chart",chart);
        ImGui::DockBuilderDockWindow("dom",dom);
        ImGui::DockBuilderDockWindow("tape",tape);
        ImGui::DockBuilderFinish(root);
    }
    ImGui::DockSpace(root); ImGui::End();
    ImGui::Begin("chart"); ImGui::End();
    if (companions) { ImGui::Begin("dom"); ImGui::End(); ImGui::Begin("tape"); ImGui::End(); }
    ImGui::Render();
}
int main() {
    init(); for(int i=0;i<5;++i) frame(true,i==0);
    const auto dom_id=ImGui::FindWindowByName("dom")->DockId;
    const auto tape_id=ImGui::FindWindowByName("tape")->DockId;
    const auto dom_size=ImGui::FindWindowByName("dom")->Size;
    const auto tape_pos=ImGui::FindWindowByName("tape")->Pos;
    const float candle_width=ImGui::FindWindowByName("chart")->Size.x;
    for(int cycle=0;cycle<3;++cycle) {
        for(int i=0;i<5;++i) frame(false);
        check(ImGui::FindWindowByName("chart")->Size.x > candle_width+200,"RT reclaims companion space");
        for(int i=0;i<5;++i) frame(true);
        const auto* dom=ImGui::FindWindowByName("dom"); const auto* tape=ImGui::FindWindowByName("tape");
        check(dom->DockId==dom_id && tape->DockId==tape_id,"stable dock identities across toggles");
        check(dom->Size.x==dom_size.x && dom->Size.y==dom_size.y && tape->Pos.y==tape_pos.y,"exact split geometry restored");
    }
    for(int i=0;i<5;++i) frame(false);
    const std::string ini=ImGui::SaveIniSettingsToMemory();
    ImGui::DestroyContext(); init(); ImGui::LoadIniSettingsFromMemory(ini.c_str());
    for(int i=0;i<5;++i) frame(false);
    for(int i=0;i<5;++i) frame(true);
    const auto* dom=ImGui::FindWindowByName("dom");
    check(dom->DockId==dom_id && dom->Size.x==dom_size.x && dom->Size.y==dom_size.y,"reload during RT preserves hidden layout");
    ImGui::DestroyContext();
    std::puts("PASS companion docking, repeated toggles and hidden reload");
}
