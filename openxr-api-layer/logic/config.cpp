// MIT License
//
// Copyright(c) 2022-2023 Matthieu Bucchianeri
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include "pch.h"
#include "config.h"
#include "framework/log.h"

namespace openxr_api_layer {

    using namespace log;

    namespace {
        // Normalize a config line: trim whitespace, remove inline comments.
        // This makes the parser forgiving of common user mistakes like
        // "peripheral_multiplier = 0.4" (spaces around =) or
        // "peripheral_multiplier=0.4 # comment" (inline comment).
        std::string NormalizeLine(const std::string& line) {
            std::string result = line;

            // Trim leading whitespace
            const auto start = result.find_first_not_of(" \t");
            if (start == std::string::npos) {
                return ""; // Line is all whitespace
            }
            result = result.substr(start);

            // Trim trailing whitespace
            const auto end = result.find_last_not_of(" \t");
            result = result.substr(0, end + 1);

            // Remove inline comments (everything after # or //)
            const auto commentPos = result.find_first_of("#/");
            if (commentPos != std::string::npos) {
                // Check if it's a // comment (not just a / in a value)
                if (result[commentPos] == '#' ||
                    (result[commentPos] == '/' && commentPos + 1 < result.size() && result[commentPos + 1] == '/')) {
                    result = result.substr(0, commentPos);
                    // Trim trailing whitespace again after removing comment
                    const auto end2 = result.find_last_not_of(" \t");
                    if (end2 != std::string::npos) {
                        result = result.substr(0, end2 + 1);
                    } else {
                        result = "";
                    }
                }
            }

            return result;
        }

        // Trim whitespace from both ends of a string.
        std::string Trim(const std::string& s) {
            const auto start = s.find_first_not_of(" \t");
            if (start == std::string::npos) {
                return "";
            }
            const auto end = s.find_last_not_of(" \t");
            return s.substr(start, end - start + 1);
        }
    } // namespace

    void FoveationConfig::LoadConfiguration(const std::filesystem::path& configPath) {
        // Look in %LocalAppData% first, then fallback to your installation folder.
        LogInformation("Trying to locate configuration file at '{}'...\n", configPath.string());
        std::ifstream configFile;
        configFile.open(configPath);
        if (configFile.is_open()) {
            bool active = true;
            unsigned int lineNumber = 0;
            std::string line;
            while (std::getline(configFile, line)) {
                lineNumber++;
                active = ParseConfigurationStatement(line, lineNumber, active, m_applicationName, m_applicationExecutableName);
            }
            configFile.close();
        } else {
            LogInformation("Not found\n");
        }
    }

    bool FoveationConfig::ParseConfigurationStatement(const std::string& line, unsigned int lineNumber, bool active,
                                                       const std::string& applicationName, const std::string& applicationExecutableName) {
        try {
            // Normalize the line: trim whitespace, remove inline comments.
            // This makes the parser forgiving of common user mistakes like
            // "peripheral_multiplier = 0.4" (spaces around =) or
            // "peripheral_multiplier=0.4 # comment" (inline comment).
            const std::string normalized = NormalizeLine(line);
            if (normalized.empty()) {
                return active;
            }

            // Handle comments.
            if ((normalized[0] == '/' && normalized.size() > 1 && normalized[1] == '/') || normalized[0] == '#') {
                return active;
            }

            // Toggle active section.
            if (normalized[0] == '[' && normalized[normalized.size() - 1] == ']') {
                // Guard against malformed/short section headers (e.g. "[]" or "[a]").
                if (normalized.size() < 3) {
                    LogWarning("L%u: Malformed section header (too short)\n", lineNumber);
                    return active;
                }
                if (normalized.size() >= 6 && normalized.substr(1, 4) == "app:") {
                    const std::string pattern = normalized.substr(5, normalized.size() - 6);
                    if (pattern.size() >= 6 && pattern.substr(0, 6) == "exact:") {
                        return applicationName == pattern.substr(6);
                    }
                    return applicationName.find(pattern) != std::string::npos;
                } else if (normalized.size() >= 6 && normalized.substr(1, 4) == "exe:") {
                    const std::string pattern = normalized.substr(5, normalized.size() - 6);
                    if (pattern.size() >= 6 && pattern.substr(0, 6) == "exact:") {
                        return applicationExecutableName == pattern.substr(6);
                    }
                    return applicationExecutableName.find(pattern) != std::string::npos;
                } else {
                    return m_runtimeName.find(normalized.substr(1, normalized.size() - 2)) != std::string::npos ||
                           m_systemName.find(normalized.substr(1, normalized.size() - 2)) != std::string::npos;
                }
            }

            // Skip sections not for the current runtime.
            if (!active) {
                return active;
            }

            const auto offset = normalized.find('=');
            if (offset != std::string::npos) {
                // Trim whitespace from name and value to handle "name = value" syntax.
                const std::string name = Trim(normalized.substr(0, offset));
                const std::string value = Trim(normalized.substr(offset + 1));

                bool parsed = false;
                if (name == "peripheral_multiplier") {
                    const float v = std::stof(value);
                    if (v < 0.1f) {
                        LogWarning("L%u: peripheral_multiplier {} below minimum 0.1, clamped\n", lineNumber, v);
                    }
                    m_peripheralPixelDensity = std::max(0.1f, v);
                    parsed = true;
                } else if (name == "focus_multiplier") {
                    const float v = std::stof(value);
                    if (v < 0.1f) {
                        LogWarning("L%u: focus_multiplier {} below minimum 0.1, clamped\n", lineNumber, v);
                    }
                    m_focusPixelDensity = std::max(0.1f, v);
                    parsed = true;
                } else if (name == "horizontal_fixed_section") {
                    const float v = std::stof(value);
                    m_horizontalFovSection[0] = std::clamp(v, 0.1f, 0.9f);
                    if (m_horizontalFovSection[0] != v) {
                        LogWarning("L%u: horizontal_fixed_section {} out of [0.1, 0.9], clamped to {}\n",
                                   lineNumber, v, m_horizontalFovSection[0]);
                    }
                    parsed = true;
                } else if (name == "vertical_fixed_section") {
                    const float v = std::stof(value);
                    m_verticalFovSection[0] = std::clamp(v, 0.1f, 0.9f);
                    if (m_verticalFovSection[0] != v) {
                        LogWarning("L%u: vertical_fixed_section {} out of [0.1, 0.9], clamped to {}\n",
                                   lineNumber, v, m_verticalFovSection[0]);
                    }
                    parsed = true;
                } else if (name == "horizontal_focus_section") {
                    const float v = std::stof(value);
                    m_horizontalFovSection[1] = std::clamp(v, 0.1f, 0.9f);
                    if (m_horizontalFovSection[1] != v) {
                        LogWarning("L%u: horizontal_focus_section {} out of [0.1, 0.9], clamped to {}\n",
                                   lineNumber, v, m_horizontalFovSection[1]);
                    }
                    parsed = true;
                } else if (name == "vertical_focus_section") {
                    const float v = std::stof(value);
                    m_verticalFovSection[1] = std::clamp(v, 0.1f, 0.9f);
                    if (m_verticalFovSection[1] != v) {
                        LogWarning("L%u: vertical_focus_section {} out of [0.1, 0.9], clamped to {}\n",
                                   lineNumber, v, m_verticalFovSection[1]);
                    }
                    parsed = true;
                } else if (name == "horizontal_fixed_offset") {
                    m_horizontalFixedOffset = std::clamp(std::stof(value), -0.5f, 0.5f);
                    parsed = true;
                } else if (name == "vertical_fixed_offset") {
                    m_verticalFixedOffset = std::clamp(std::stof(value), -0.5f, 0.5f);
                    parsed = true;
                } else if (name == "horizontal_focus_offset") {
                    m_horizontalFocusOffset = std::clamp(std::stof(value), -0.5f, 0.5f);
                    parsed = true;
                } else if (name == "vertical_focus_offset") {
                    m_verticalFocusOffset = std::clamp(std::stof(value), -0.5f, 0.5f);
                    parsed = true;
                } else if (name == "horizontal_focus_widening_multiplier") {
                    m_horizontalFocusWideningMultiplier = std::clamp(std::stof(value), 0.f, 2.f);
                    parsed = true;
                } else if (name == "vertical_focus_widening_multiplier") {
                    m_verticalFocusWideningMultiplier = std::clamp(std::stof(value), 0.f, 2.f);
                    parsed = true;
                } else if (name == "focus_widening_deadzone") {
                    m_focusWideningDeadzone = std::clamp(std::stof(value), 0.f, 0.5f);
                    parsed = true;
                } else if (name == "stabilize_focus_fov") {
                    m_stabilizeFocusFov = std::stoi(value) != 0;
                    parsed = true;
                } else if (name == "focus_fov_hysteresis") {
                    m_focusFovHysteresis = std::clamp(std::stof(value), 0.f, 0.5f);
                    parsed = true;
                } else if (name == "focus_fov_smoothing") {
                    m_focusFovSmoothing = std::clamp(std::stof(value), 0.f, 1.f);
                    parsed = true;
                } else if (name == "blink_freeze_focus_region") {
                    m_blinkFreezeFocusRegion = std::stoi(value) != 0;
                    parsed = true;
                } else if (name == "blink_freeze_grace_ms") {
                    m_blinkFreezeGraceMs = std::stoul(value);
                    parsed = true;
                } else if (name == "use_fsr1_easu") {
                    m_useFSR1EASU = std::stoi(value) != 0;
                    parsed = true;
                } else if (name == "use_easu_mip_gen") {
                    m_useEasuMipGen = std::stoi(value) != 0;
                    parsed = true;
                } else if (name == "peripheral_lod_bias") {
                    m_peripheralLodBias = std::clamp(std::stof(value), 0.f, 2.f);
                    parsed = true;
                } else if (name == "peripheral_anisotropy") {
                    m_peripheralAnisotropy = static_cast<uint32_t>(std::stoi(value));
                    parsed = true;
                } else if (name == "peripheral_edge_blur") {
                    m_peripheralEdgeBlur = std::clamp(std::stof(value), 0.f, 1.f);
                    parsed = true;
                } else if (name == "boundary_desaturation") {
                    m_boundaryDesaturation = std::clamp(std::stof(value), 0.f, 1.f);
                    parsed = true;
                } else if (name == "radial_lod_start") {
                    m_radialLodStart = std::clamp(std::stof(value), 0.f, 1.f);
                    parsed = true;
                } else if (name == "radial_lod_end") {
                    m_radialLodEnd = std::clamp(std::stof(value), 0.f, 1.f);
                    parsed = true;
                } else if (name == "radial_lod_max_boost") {
                    m_radialLodMaxBoost = std::clamp(std::stof(value), 0.f, 2.f);
                    parsed = true;
                } else if (name == "focus_aspect") {
                    m_focusAspect = std::clamp(std::stof(value), 0.1f, 10.f);
                    parsed = true;
                } else if (name == "fsr1_sharpness") {
                    m_fsr1Sharpness = std::clamp(std::stof(value), 0.f, 1.f);
                    parsed = true;
                } else if (name == "use_blue_noise_dither") {
                    m_useBlueNoiseDither = std::stoi(value) != 0;
                    parsed = true;
                } else if (name == "prefer_foveated_rendering") {
                    m_preferFoveatedRendering = std::stoi(value);
                    parsed = true;
                } else if (name == "force_no_eye_tracking") {
                    m_forceNoEyeTracking = std::stoi(value);
                    parsed = true;
                } else if (name == "force_focus_fov_quirk") {
                    m_needFocusFovCorrectionQuirk = m_needFocusFovCorrectionQuirk || std::stoi(value);
                    parsed = true;
                } else if (name == "smoothen_focus_view_edges") {
                    m_smoothenFocusViewEdges = std::clamp(std::stof(value), 0.f, 0.5f);
                    parsed = true;
                } else if (name == "sharpen_focus_view") {
                    m_sharpenFocusView = std::clamp(std::stof(value), 0.f, 1.f);
                    parsed = true;
                } else if (name == "feather_focus_edges") {
                    m_featherFocusEdges = std::clamp(std::stof(value), 0.f, 1.f);
                    parsed = true;
                } else if (name == "fov_tangent_x") {
                    m_fovTangentX = std::clamp(std::stof(value), 0.1f, 1.f);
                    parsed = true;
                } else if (name == "fov_tangent_y") {
                    m_fovTangentY = std::clamp(std::stof(value), 0.1f, 1.f);
                    parsed = true;
                } else if (name == "turbo_mode") {
                    m_useTurboMode = std::stoi(value);
                    parsed = true;
                } else if (name == "unadvertise") {
                    m_unadvertiseQuadViews = std::stoi(value);
                    parsed = true;
                } else if (name == "bypass_api_layer") {
                    m_bypassApiLayer = std::stoi(value) != 0;
                    parsed = true;
                } else if (name == "debug_simulate_tracking") {
                    m_debugSimulateTracking = std::stoi(value);
                    parsed = true;
                } else if (name == "debug_focus_view") {
                    m_debugFocusView = std::stoi(value);
                    parsed = true;
                } else if (name == "debug_eye_gaze") {
                    m_debugEyeGaze = std::stoi(value);
                    parsed = true;
                } else if (name == "debug_keys") {
                    m_debugKeys = std::stoi(value);
                    parsed = true;
                } else if (name == "eye_tracking_confidence_threshold") {
                    m_eyeTrackingConfidenceThreshold = std::clamp(std::stof(value), 0.f, 1.f);
                    parsed = true;
                } else if (name == "eye_gaze_cache_timeout_ms") {
                    m_eyeGazeCacheTimeoutMs = std::stoul(value);
                    parsed = true;
                } else if (name == "eye_tracking_min_cutoff") {
                    m_eyeTrackingMinCutoff = std::clamp(std::stof(value), 0.001f, 10.0f);
                    parsed = true;
                } else if (name == "eye_tracking_beta") {
                    m_eyeTrackingBeta = std::clamp(std::stof(value), 0.0f, 10.0f);
                    parsed = true;
                } else if (name == "transition_dither_amount" || name == "dithering_amount") {
                    m_transitionDitherAmount = std::clamp(std::stof(value), 0.f, 0.1f);
                    parsed = true;
                } else if (name == "log_level") {
                    if (!log::ParseLogLevel(value.c_str())) {
                        LogWarning("  Invalid log_level '{}', using default (Information).", value);
                    }
                    parsed = true;
                } else {
                    LogWarning("L%u: Unrecognized option\n", lineNumber);
                }

                if (parsed) {
                    LogInformation("  Found option '{}={}'\n", name, value);
                }
            } else {
                LogWarning("L%u: Improperly formatted option\n", lineNumber);
            }
        } catch (const std::exception& e) {
            LogWarning("L%u: Parsing error: {}\n", lineNumber, e.what());
        } catch (...) {
            LogWarning("L%u: Parsing error (unknown exception)\n", lineNumber);
        }

        return active;
    }

} // namespace openxr_api_layer
