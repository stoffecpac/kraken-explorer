#include "ui/gateway.h"

#include <cfloat>
#include <format>

#include <imgui.h>

#include "app.h"
#include "ui/raw_tx.h"
#include "ui/theme.h"

namespace
{

// Label cell of a two-column form table, the value cell follows.
void form_label(const char* text)
{
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(text);
    ImGui::TableNextColumn();
}

void draw_message_combo(App& app, Gateway& gw)
{
    const std::string preview = gw.name.empty() ? std::format("0x{:X}", gw.id) : std::format("{} (0x{:X})", gw.name, gw.id);
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16.0f);
    if (ImGui::BeginCombo("##msg", preview.c_str()))
    {
        for (const SetupNetwork& net : app.setup.networks)
        {
            for (const auto& db : net.can_dbs)
            {
                for (const auto& [raw_id, msg] : db->messages)
                {
                    const uint32_t id = raw_id & can_id_mask_extended;
                    if (ImGui::Selectable(std::format("{} (0x{:X})##{:X}", msg.name, id, raw_id).c_str(), gw.name == msg.name && gw.id == id))
                    {
                        gw.id = id;
                        gw.extended = (raw_id & 0x80000000u) != 0;
                        gw.name = msg.name;
                    }
                }
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6.0f);
    if (ImGui::InputScalar("ID", ImGuiDataType_U32, &gw.id, nullptr, nullptr, "%X", ImGuiInputTextFlags_CharsHexadecimal))
    {
        gw.name.clear(); // typed by hand
        gw.id &= gw.extended ? can_id_mask_extended : can_id_mask_standard;
    }
    ImGui::SameLine();
    if (ImGui::Checkbox("Ext", &gw.extended) && !gw.extended)
    {
        gw.id &= can_id_mask_standard;
    }
}

} // namespace

bool gateway_matches(const GatewayRule& rule, const BusMessage& m) noexcept
{
    return m.type == BusType::CAN && !has_flag(m, bus_flag::tx) && !is_error_frame(m) && m.iface == rule.src
        && m.id == rule.id && has_flag(m, bus_flag::extended) == rule.extended;
}

GatewayRuleProblem gateway_rule_problem(const Gateway& gw, uint16_t src, uint16_t dst) noexcept
{
    if (src == UINT16_MAX || dst == UINT16_MAX || src == dst)
    {
        return GatewayRuleProblem::SameInterface;
    }
    for (const GatewayRule& rule : gw.rules)
    {
        if (rule.src == dst && rule.dst == src)
        {
            return GatewayRuleProblem::Reverse;
        }
    }
    return GatewayRuleProblem::None;
}

void gateway_rx_consumer(void* user, const BusMessage& m)
{
    auto& gw = *static_cast<Gateway*>(user);
    std::scoped_lock lock(gw.mutex); // ponytail: one lock per RX frame; a snapshot pointer if it ever shows up
    if (!gw.enabled || gw.ifaces == nullptr)
    {
        return;
    }
    for (const GatewayRule& rule : gw.rules)
    {
        if (gateway_matches(rule, m) && rule.dst < gw.ifaces->size())
        {
            BusMessage out = m;
            out.iface = rule.dst;
            const bool ok = iface_send((*gw.ifaces)[rule.dst], out);
            ++(ok ? gw.forwarded : gw.failed);
        }
    }
}

void draw_gateway(App& app, Gateway& gw)
{
    if (!gw.open)
    {
        return;
    }
    ImGui::SetNextWindowSize(ImVec2(560.0f, 360.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("CAN Gateway", &gw.open))
    {
        ImGui::End();
        return;
    }
    {
        std::scoped_lock lock(gw.mutex);
        ImGui::Checkbox("Enable Gateway", &gw.enabled);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%llu forwarded, %llu failed", static_cast<unsigned long long>(gw.forwarded.load()),
                        static_cast<unsigned long long>(gw.failed.load()));
    ImGui::Separator();

    const bool enabled = gw.enabled; // only the UI writes it
    ImGui::BeginDisabled(!enabled);
    if (ImGui::BeginTable("##add", 2, ImGuiTableFlags_SizingStretchProp))
    {
        ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch);
        form_label("Message:");
        draw_message_combo(app, gw);
        form_label("Source:");
        ImGui::SetNextItemWidth(-FLT_MIN);
        draw_iface_combo("##src", app, gw.src, BusType::CAN);
        form_label("Destination:");
        ImGui::SetNextItemWidth(-FLT_MIN);
        draw_iface_combo("##dst", app, gw.dst, BusType::CAN);
        ImGui::EndTable();
    }
    const GatewayRuleProblem problem = gateway_rule_problem(gw, gw.src, gw.dst);
    ImGui::BeginDisabled(problem == GatewayRuleProblem::SameInterface);
    if (ImGui::Button("Add"))
    {
        std::scoped_lock lock(gw.mutex);
        gw.rules.push_back({.id = gw.id, .extended = gw.extended, .name = gw.name, .src = gw.src, .dst = gw.dst});
    }
    ImGui::EndDisabled();
    if (problem == GatewayRuleProblem::Reverse)
    {
        ImGui::SameLine();
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(theme_text(ThemeText::warn)), "Reverse of an existing rule: frames may loop between the buses");
    }

    ImGui::SeparatorText("Active Rules");
    const float footer = ImGui::GetFrameHeightWithSpacing();
    constexpr ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY;
    if (ImGui::BeginTable("##rules", 4, flags, ImVec2(0.0f, -footer)))
    {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("ID");
        ImGui::TableSetupColumn("Source", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Destination", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        for (int i = 0; i < static_cast<int>(gw.rules.size()); ++i)
        {
            GatewayRule& rule = gw.rules[static_cast<std::size_t>(i)];
            ImGui::PushID(i);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (ImGui::Selectable(rule.name.empty() ? "-" : rule.name.c_str(), gw.selected == i, ImGuiSelectableFlags_AllowOverlap))
            {
                gw.selected = i;
            }
            ImGui::TableNextColumn();
            ImGui::Text("0x%X%s", rule.id, rule.extended ? "x" : "");
            // Source/destination stay editable per row, as in the Qt table.
            uint16_t src = rule.src;
            uint16_t dst = rule.dst;
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-FLT_MIN);
            const bool src_changed = draw_iface_combo("##rsrc", app, src, BusType::CAN);
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (draw_iface_combo("##rdst", app, dst, BusType::CAN) || src_changed)
            {
                std::scoped_lock lock(gw.mutex);
                rule.src = src;
                rule.dst = dst;
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::BeginDisabled(gw.selected < 0 || gw.selected >= static_cast<int>(gw.rules.size()));
    if (ImGui::Button("Remove"))
    {
        std::scoped_lock lock(gw.mutex);
        gw.rules.erase(gw.rules.begin() + gw.selected);
        gw.selected = -1;
    }
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("OK"))
    {
        gw.open = false;
    }
    ImGui::End();
}
