#pragma once

#include "tau-cooker/model_desc.h"

#include <string>

namespace tau::cooker::gltf {
    bool read(const std::string& path, model::read_e what, model::model_desc_t& out); } // namespace tau::cooker::gltf
