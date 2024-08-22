// Copyright (c) 2024 UltiMaker
// CuraEngine is released under the terms of the AGPLv3 or higher

#ifdef __EMSCRIPTEN__
#include "communication/EmscriptenCommunication.h"

#include <emscripten.h>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <fmt/format.h>
#include <range/v3/algorithm/contains.hpp>
#include <range/v3/iterator/operations.hpp>
#include <spdlog/spdlog.h>

#include "Application.h"
#include "FffProcessor.h"
#include "Slice.h"

namespace cura
{

EmscriptenCommunication::EmscriptenCommunication(const std::vector<std::string>& arguments)
    : CommandLine(arguments)
{
    spdlog::info("Emscripten communication initialized");
    if (auto progress_flag = ranges::find(arguments_, "--progress_cb"); progress_flag != arguments_.end())
    {
        progress_handler_ = *ranges::next(progress_flag);
    }
    if (auto slice_info_flag = ranges::find(arguments_, "--slice_info_cb"); slice_info_flag != arguments_.end())
    {
        slice_info_handler_ = *ranges::next(slice_info_flag);
    }
    if (auto gcode_header_flag = ranges::find(arguments_, "--gcode_header_cb"); gcode_header_flag != arguments_.end())
    {
        gcode_header_handler_ = *ranges::next(gcode_header_flag);
    }
}

void EmscriptenCommunication::sendGCodePrefix(const std::string& prefix) const
{
    emscripten_run_script(fmt::format("globalThis[\"{}\"]({})", gcode_prefix_handler_, prefix).c_str());
}

void EmscriptenCommunication::sendProgress(double progress) const
{
    emscripten_run_script(fmt::format("globalThis[\"{}\"]({})", progress_handler_, progress).c_str());
}

std::string EmscriptenCommunication::createSliceInfoMessage()
{
    // Construct a string with rapidjson containing the slice information
    rapidjson::Document doc;
    auto& allocator = doc.GetAllocator();
    doc.SetObject();

    // Set the slice UUID
    rapidjson::Value slice_uuid("slice_uuid", allocator);
    rapidjson::Value uuid(Application::getInstance().instance_uuid_.c_str(), allocator);
    doc.AddMember(slice_uuid, uuid, allocator);

    // Set the time estimates
    rapidjson::Value time_estimates_json(rapidjson::kObjectType);
    auto time_estimates = FffProcessor::getInstance()->getTotalPrintTimePerFeature();
    for (const auto& [feature, duration_idx] : std::vector<std::tuple<std::string, PrintFeatureType>>{ { "infill", PrintFeatureType::Infill },
                                                                                                       { "skin", PrintFeatureType::Skin },
                                                                                                       { "support", PrintFeatureType::Support },
                                                                                                       { "inner_wall", PrintFeatureType::InnerWall },
                                                                                                       { "move_combing", PrintFeatureType::MoveCombing },
                                                                                                       { "move_retraction", PrintFeatureType::MoveRetraction },
                                                                                                       { "outer_wall", PrintFeatureType::OuterWall },
                                                                                                       { "prime_tower", PrintFeatureType::PrimeTower },
                                                                                                       { "skirt_brim", PrintFeatureType::SkirtBrim },
                                                                                                       { "support_infill", PrintFeatureType::SupportInfill },
                                                                                                       { "support_interface", PrintFeatureType::SupportInterface } })
    {
        rapidjson::Value feature_time(feature.c_str(), allocator);
        rapidjson::Value feature_duration(time_estimates[static_cast<unsigned char>(duration_idx)]);
        time_estimates_json.AddMember(feature_time, feature_duration, allocator);
    }
    doc.AddMember("time_estimates", time_estimates_json, allocator);

    // Set the material estimates
    rapidjson::Value material_estimates_json(rapidjson::kObjectType);
    const Scene& scene = Application::getInstance().current_slice_->scene;

    for (size_t extruder_nr = 0; extruder_nr < Application::getInstance().current_slice_->scene.extruders.size(); extruder_nr++)
    {
        const double value = FffProcessor::getInstance()->getTotalFilamentUsed(static_cast<int>(extruder_nr));
        spdlog::info("Extruder {} used {} [mm] of filament", extruder_nr, value);
        rapidjson::Value extruder_id(fmt::format("{}", extruder_nr).c_str(), allocator);
        rapidjson::Value extruder_material_estimate(value);
        material_estimates_json.AddMember(extruder_id, extruder_material_estimate, allocator);
    }
    doc.AddMember("material_estimates", material_estimates_json, allocator);

    // Set CuraEngine information
    rapidjson::Value slicer_info_json(rapidjson::kObjectType);
    rapidjson::Value slicer_version(CURA_ENGINE_VERSION, allocator);
    doc.AddMember("slicer_info", slicer_info_json, allocator);

    // Serialize the JSON document to a string
    rapidjson::StringBuffer buffer;
    rapidjson::Writer writer(buffer);
    doc.Accept(writer);
    return buffer.GetString();
}

void EmscriptenCommunication::sliceNext()
{
    CommandLine::sliceNext();
    auto slice_info = createSliceInfoMessage();
    emscripten_run_script(fmt::format("globalThis[\"{}\"]({})", slice_info_handler_, slice_info).c_str());
};

} // namespace cura

#endif // __EMSCRIPTEN__
