// SPDX-License-Identifier: BSD-2-Clause

// This code is part of the sfizz library and is licensed under a BSD 2-clause
// license. You should have receive a LICENSE.md file along with the code.
// If not, contact the sfizz maintainers at https://github.com/sfztools/sfizz

#pragma once
#include <clap/clap.h>

#if defined(_WIN32)
#define OSCAIRN_SAMPLER_EXPORT __declspec(dllexport)
#else
#define OSCAIRN_SAMPLER_EXPORT __attribute__((visibility("default")))
#endif

extern "C" OSCAIRN_SAMPLER_EXPORT const clap_plugin_entry_t clap_entry;

// For tests: how many times the instance's contexts opened a sample file
#include <cstddef>
size_t oscairnSamplerFileOpens(const clap_plugin_t* plugin);
// For tests: how many times a patch text was parsed to scan it (the load itself parses it too)
size_t oscairnSamplerPatchScanParses();
