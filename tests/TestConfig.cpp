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
#include <gtest/gtest.h>
#include "logic/config.h"

namespace openxr_api_layer {

    TEST(ConfigParserTest, ClampsOutOfRange) {
        FoveationConfig cfg;
        cfg.ParseConfigurationStatement("peripheral_multiplier=0.0", 1, true, "", "");
        EXPECT_FLOAT_EQ(cfg.m_peripheralPixelDensity, 0.1f); // clamped to min
    }

    TEST(ConfigParserTest, SectionAppExactMatch) {
        FoveationConfig cfg;
        // Should not activate for "OtherGame"
        EXPECT_FALSE(cfg.ParseConfigurationStatement("[app:exact:OtherGame]", 1, true, "DCS", ""));
        // Should activate for "DCS"
        EXPECT_TRUE(cfg.ParseConfigurationStatement("[app:exact:DCS]", 2, false, "DCS", ""));
    }

    TEST(ConfigParserTest, IgnoresComments) {
        FoveationConfig cfg;
        // Should return true (active state unchanged) and not crash
        EXPECT_TRUE(cfg.ParseConfigurationStatement("# a comment", 1, true, "", ""));
        EXPECT_TRUE(cfg.ParseConfigurationStatement("// another", 2, true, "", ""));
    }

    TEST(ConfigParserTest, MalformedSectionHeaderDoesNotCrash) {
        FoveationConfig cfg;
        EXPECT_NO_THROW(cfg.ParseConfigurationStatement("[]", 1, true, "", ""));
        EXPECT_NO_THROW(cfg.ParseConfigurationStatement("[a]", 2, true, "", ""));
    }

    TEST(ConfigParserTest, PeripheralSpatialAA) {
        FoveationConfig cfg;
        // Defaults — LOD bias set to 0.25 as a mild shimmer reducer (the
        // localized transition blur via peripheral_edge_blur complements it).
        EXPECT_FLOAT_EQ(cfg.m_peripheralLodBias, 0.25f);
        EXPECT_EQ(cfg.m_peripheralAnisotropy, 8u);
        EXPECT_FLOAT_EQ(cfg.m_peripheralEdgeBlur, 0.5f);

        cfg.ParseConfigurationStatement("peripheral_lod_bias=0.75", 1, true, "", "");
        EXPECT_FLOAT_EQ(cfg.m_peripheralLodBias, 0.75f);

        cfg.ParseConfigurationStatement("peripheral_anisotropy=16", 2, true, "", "");
        EXPECT_EQ(cfg.m_peripheralAnisotropy, 16u);

        // LOD bias clamps to [0, 2]
        cfg.ParseConfigurationStatement("peripheral_lod_bias=5.0", 3, true, "", "");
        EXPECT_FLOAT_EQ(cfg.m_peripheralLodBias, 2.0f);

        // peripheral_edge_blur clamps to [0, 1]
        cfg.ParseConfigurationStatement("peripheral_edge_blur=0.25", 4, true, "", "");
        EXPECT_FLOAT_EQ(cfg.m_peripheralEdgeBlur, 0.25f);
        cfg.ParseConfigurationStatement("peripheral_edge_blur=3.0", 5, true, "", "");
        EXPECT_FLOAT_EQ(cfg.m_peripheralEdgeBlur, 1.0f);
        cfg.ParseConfigurationStatement("peripheral_edge_blur=-1.0", 6, true, "", "");
        EXPECT_FLOAT_EQ(cfg.m_peripheralEdgeBlur, 0.0f);
    }

    TEST(ConfigParserTest, ToleratesSpacesAroundEquals) {
        FoveationConfig cfg;
        cfg.ParseConfigurationStatement("peripheral_multiplier = 0.4", 1, true, "", "");
        EXPECT_FLOAT_EQ(cfg.m_peripheralPixelDensity, 0.4f);

        cfg.ParseConfigurationStatement("  peripheral_lod_bias  =  0.75  ", 2, true, "", "");
        EXPECT_FLOAT_EQ(cfg.m_peripheralLodBias, 0.75f);
    }

    TEST(ConfigParserTest, StripsInlineComments) {
        FoveationConfig cfg;
        cfg.ParseConfigurationStatement("peripheral_multiplier=0.4 # half res", 1, true, "", "");
        EXPECT_FLOAT_EQ(cfg.m_peripheralPixelDensity, 0.4f);

        cfg.ParseConfigurationStatement("peripheral_lod_bias=0.75 // shimmer fix", 2, true, "", "");
        EXPECT_FLOAT_EQ(cfg.m_peripheralLodBias, 0.75f);
    }

    TEST(ConfigParserTest, ToleratesLeadingWhitespaceAndBlankLines) {
        FoveationConfig cfg;
        // Leading whitespace before a section header.
        EXPECT_TRUE(cfg.ParseConfigurationStatement("   [app:exact:DCS]", 1, false, "DCS", ""));
        // Whitespace-only lines keep the active state.
        EXPECT_TRUE(cfg.ParseConfigurationStatement("   ", 2, true, "", ""));
        EXPECT_FALSE(cfg.ParseConfigurationStatement("\t ", 3, false, "", ""));
    }

    TEST(ConfigParserTest, BypassApiLayerDefaultsOff) {
        FoveationConfig cfg;
        EXPECT_FALSE(cfg.m_bypassApiLayer);
    }

    TEST(ConfigParserTest, BypassApiLayerParses) {
        FoveationConfig cfg;
        cfg.ParseConfigurationStatement("bypass_api_layer=1", 1, true, "", "");
        EXPECT_TRUE(cfg.m_bypassApiLayer);
    }

    TEST(ConfigParserTest, BypassApiLayerSectionScoped) {
        FoveationConfig cfg;
        // Not active while a different application's section is selected.
        bool active = cfg.ParseConfigurationStatement("[exe:OtherGame.exe]", 1, true, "", "MyGame.exe");
        EXPECT_FALSE(active);
        cfg.ParseConfigurationStatement("bypass_api_layer=1", 2, active, "", "MyGame.exe");
        EXPECT_FALSE(cfg.m_bypassApiLayer);

        // Active once the matching section is selected.
        active = cfg.ParseConfigurationStatement("[exe:MyGame.exe]", 3, active, "", "MyGame.exe");
        EXPECT_TRUE(active);
        cfg.ParseConfigurationStatement("bypass_api_layer=1", 4, active, "", "MyGame.exe");
        EXPECT_TRUE(cfg.m_bypassApiLayer);
    }

} // namespace openxr_api_layer
